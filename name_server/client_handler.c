#include "name_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int register_client(Message *msg) {
    pthread_mutex_lock(&nm_state->client_list_mutex);
    
    // Check if client already exists
    int existing_index = find_client(msg->username);
    
    if (existing_index >= 0) {
        // Client reconnecting - update info
        ClientInfo *client = &nm_state->clients[existing_index];
        strcpy(client->ip, msg->ip);
        client->port = msg->port1;
        client->last_activity = time(NULL);
        client->is_connected = 1;
        
        printf("[Client Registration] Client '%s' reconnected from %s:%d\n", 
               msg->username, msg->ip, msg->port1);
    } else {
        // New client registration
        if (nm_state->client_count >= MAX_CLIENTS) {
            pthread_mutex_unlock(&nm_state->client_list_mutex);
            printf("[Client Registration] Error: Maximum clients reached\n");
            return ERR_SERVER_ERROR;
        }
        
        ClientInfo *client = &nm_state->clients[nm_state->client_count];
        strcpy(client->username, msg->username);
        strcpy(client->ip, msg->ip);
        client->port = msg->port1;
        client->last_activity = time(NULL);
        client->is_connected = 1;
        
        nm_state->client_count++;
        
        printf("[Client Registration] New client '%s' registered from %s:%d\n", 
               msg->username, msg->ip, msg->port1);
    }
    
    pthread_mutex_unlock(&nm_state->client_list_mutex);
    
    log_operation("CLIENT_REGISTER", msg->username);
    return ERR_SUCCESS;
}

void handle_client_registration(int socket, Message *msg) {
    printf("[Client Handler] Processing client registration for '%s'\n", msg->username);
    
    int result = register_client(msg);
    
    Message response = {0};
    response.msg_type = MSG_ACK;
    response.operation = OP_CLIENT_REGISTER;
    response.error_code = result;
    
    if (result == ERR_SUCCESS) {
        strcpy(response.data, "Client registration successful");
        printf("[Client Handler] Client '%s' registered successfully\n", msg->username);
    } else {
        strcpy(response.data, "Client registration failed");
        printf("[Client Handler] Client '%s' registration failed\n", msg->username);
    }
    
    send_message(socket, &response);
}

int find_client(const char *username) {
    for (int i = 0; i < nm_state->client_count; i++) {
        if (strcmp(nm_state->clients[i].username, username) == 0) {
            return i;
        }
    }
    return -1;  // Not found
}

void handle_get_ss_info(int socket, Message *msg) {
    printf("[File Location] Client '%s' requesting location for file '%s'\n", 
           msg->username, msg->filename);
    
    FileInfo *file = find_file(msg->filename);
    
    Message response = {0};
    response.msg_type = MSG_RESPONSE;
    response.operation = OP_GET_SS_INFO;
    
    if (!file) {
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "File not found");
        printf("[File Location] File '%s' not found\n", msg->filename);
        send_message(socket, &response);
        return;
    }
    
    printf("[File Location] File found: primary_ss_id=%d, backup_ss_id=%d, owner=%s\n",
           file->primary_ss_id, file->backup_ss_id, file->owner);
    
    // Find the appropriate server (primary or acting backup)
    int target_ss_id = file->primary_ss_id;
    int ss_index = find_storage_server(target_ss_id);
    
    if (ss_index < 0) {
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Storage server not found");
        printf("[File Location] Primary server SS%d not found (ss_index=%d)\n", target_ss_id, ss_index);
        send_message(socket, &response);
        return;
    }
    
    printf("[File Location] Found storage server at index %d\n", ss_index);
    
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    
    pthread_mutex_lock(&ss->ss_mutex);
    
    // Check if primary server is online
    if (ss->status != SS_STATUS_ONLINE) {
        // Primary is down, try backup
        int backup_ss_id = file->backup_ss_id;
        pthread_mutex_unlock(&ss->ss_mutex);
        
        if (backup_ss_id > 0) {
            int backup_index = find_storage_server(backup_ss_id);
            if (backup_index >= 0) {
                StorageServerInfo *backup_ss = &nm_state->storage_servers[backup_index];
                
                pthread_mutex_lock(&backup_ss->ss_mutex);
                if (backup_ss->status == SS_STATUS_ONLINE || backup_ss->status == SS_STATUS_ACTING_PRIMARY) {
                    // Use backup server
                    target_ss_id = backup_ss_id;
                    strcpy(response.ip, backup_ss->ip);
                    response.port1 = backup_ss->client_port;
                    response.error_code = ERR_SUCCESS;
                    
                    // Promote backup to acting primary if not already
                    if (backup_ss->status == SS_STATUS_ONLINE) {
                        backup_ss->status = SS_STATUS_ACTING_PRIMARY;
                        printf("[Failover] SS%d promoted to acting primary for failed SS%d\n", 
                               backup_ss_id, file->primary_ss_id);
                    }
                    
                    printf("[File Location] Redirected '%s' to backup server SS%d at %s:%d\n", 
                           msg->filename, backup_ss_id, backup_ss->ip, backup_ss->client_port);
                } else {
                    response.error_code = ERR_SERVER_ERROR;
                    strcpy(response.data, "Both primary and backup servers are down");
                    printf("[File Location] Both primary SS%d and backup SS%d are down\n", 
                           file->primary_ss_id, backup_ss_id);
                }
                pthread_mutex_unlock(&backup_ss->ss_mutex);
            } else {
                response.error_code = ERR_SERVER_ERROR;
                strcpy(response.data, "Backup server not found");
                printf("[File Location] Backup server SS%d not found\n", backup_ss_id);
            }
        } else {
            response.error_code = ERR_SERVER_ERROR;
            strcpy(response.data, "Primary server down and no backup available");
            printf("[File Location] Primary SS%d down and no backup configured\n", file->primary_ss_id);
        }
    } else {
        // Primary server is online
        strcpy(response.ip, ss->ip);
        response.port1 = ss->client_port;
        response.error_code = ERR_SUCCESS;
        
        printf("[File Location] Primary server online. IP=%s, client_port=%d\n", 
               ss->ip, ss->client_port);
        printf("[File Location] File '%s' located on primary server SS%d at %s:%d\n", 
               msg->filename, target_ss_id, ss->ip, ss->client_port);
        
        pthread_mutex_unlock(&ss->ss_mutex);
    }
    
    if (response.error_code == ERR_SUCCESS) {
        response.ss_id = target_ss_id;
        snprintf(response.data, sizeof(response.data), 
                "File located on SS%d at %s:%d", target_ss_id, response.ip, response.port1);
        printf("[File Location] Sending response: ip=%s, port=%d, ss_id=%d\n",
               response.ip, response.port1, response.ss_id);
    }
    
    send_message(socket, &response);
}

void handle_create_file(int socket, Message *msg) {
    printf("[File Creation] Client '%s' creating file '%s'\n", msg->username, msg->filename);
    
    // Check if file already exists
    FileInfo *existing_file = find_file(msg->filename);
    
    Message response = {0};
    response.msg_type = MSG_ACK;
    response.operation = OP_CREATE;
    
    if (existing_file) {
        response.error_code = ERR_FILE_EXISTS;
        strcpy(response.data, "File already exists");
        printf("[File Creation] File '%s' already exists\n", msg->filename);
        send_message(socket, &response);
        return;
    }
    
    // Get available primary server
    int primary_ss_id = get_available_primary_server();
    if (primary_ss_id < 0) {
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "No available storage servers");
        printf("[File Creation] No available storage servers\n");
        send_message(socket, &response);
        return;
    }
    
    // Add file to server's file list
    int result = add_file_to_server(primary_ss_id, msg->filename, msg->username);
    if (result != ERR_SUCCESS) {
        response.error_code = result;
        strcpy(response.data, "Failed to register file with storage server");
        printf("[File Creation] Failed to register file '%s' with SS%d\n", msg->filename, primary_ss_id);
        send_message(socket, &response);
        return;
    }
    
    // Send create command to storage server
    int ss_index = find_storage_server(primary_ss_id);
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    
    int ss_socket = connect_to_server(ss->ip, ss->nm_port);
    if (ss_socket < 0) {
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "Failed to connect to storage server");
        printf("[File Creation] Failed to connect to SS%d\n", primary_ss_id);
        
        // Remove file from our records since SS connection failed
        remove_file_from_server(primary_ss_id, msg->filename);
        send_message(socket, &response);
        return;
    }
    
    // Send create file command to storage server
    Message ss_msg = {0};
    ss_msg.msg_type = MSG_REQUEST;
    ss_msg.operation = OP_SS_CREATE_FILE;
    strcpy(ss_msg.filename, msg->filename);
    strcpy(ss_msg.username, msg->username);
    strcpy(ss_msg.data, "CREATE command from Name Server");
    
    if (send_message(ss_socket, &ss_msg) < 0) {
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "Failed to send command to storage server");
        printf("[File Creation] Failed to send CREATE command to SS%d\n", primary_ss_id);
        
        close(ss_socket);
        remove_file_from_server(primary_ss_id, msg->filename);
        send_message(socket, &response);
        return;
    }
    
    // Receive response from storage server
    Message ss_response = {0};
    if (receive_message(ss_socket, &ss_response) < 0) {
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "Failed to receive response from storage server");
        printf("[File Creation] Failed to receive response from SS%d\n", primary_ss_id);
        
        close(ss_socket);
        remove_file_from_server(primary_ss_id, msg->filename);
        send_message(socket, &response);
        return;
    }
    
    close(ss_socket);
    
    // Forward storage server response to client
    response.error_code = ss_response.error_code;
    strcpy(response.data, ss_response.data);
    
    if (ss_response.error_code == ERR_SUCCESS) {
        printf("[File Creation] File '%s' created successfully on SS%d\n", msg->filename, primary_ss_id);
        log_operation("FILE_CREATE", msg->filename);
    } else {
        printf("[File Creation] File '%s' creation failed on SS%d: %s\n", 
               msg->filename, primary_ss_id, ss_response.data);
        remove_file_from_server(primary_ss_id, msg->filename);
    }
    
    send_message(socket, &response);
}

void handle_delete_file(int socket, Message *msg) {
    printf("[File Deletion] Client '%s' deleting file '%s'\n", msg->username, msg->filename);
    
    FileInfo *file = find_file(msg->filename);
    
    Message response = {0};
    response.msg_type = MSG_ACK;
    response.operation = OP_DELETE;
    
    if (!file) {
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "File not found");
        printf("[File Deletion] File '%s' not found\n", msg->filename);
        send_message(socket, &response);
        return;
    }
    
    // Check if user is the owner (simple ownership check)
    if (strcmp(file->owner, msg->username) != 0) {
        response.error_code = ERR_NOT_OWNER;
        strcpy(response.data, "Only file owner can delete the file");
        printf("[File Deletion] User '%s' is not owner of file '%s'\n", msg->username, msg->filename);
        send_message(socket, &response);
        return;
    }
    
    // Send delete command to storage server
    int primary_ss_id = file->primary_ss_id;
    int ss_index = find_storage_server(primary_ss_id);
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    
    int ss_socket = connect_to_server(ss->ip, ss->nm_port);
    if (ss_socket < 0) {
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "Failed to connect to storage server");
        printf("[File Deletion] Failed to connect to SS%d\n", primary_ss_id);
        send_message(socket, &response);
        return;
    }
    
    // Send delete file command to storage server
    Message ss_msg = {0};
    ss_msg.msg_type = MSG_REQUEST;
    ss_msg.operation = OP_SS_DELETE_FILE;
    strcpy(ss_msg.filename, msg->filename);
    strcpy(ss_msg.username, msg->username);
    strcpy(ss_msg.data, "DELETE command from Name Server");
    
    if (send_message(ss_socket, &ss_msg) < 0) {
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "Failed to send command to storage server");
        printf("[File Deletion] Failed to send DELETE command to SS%d\n", primary_ss_id);
        close(ss_socket);
        send_message(socket, &response);
        return;
    }
    
    // Receive response from storage server
    Message ss_response = {0};
    if (receive_message(ss_socket, &ss_response) < 0) {
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "Failed to receive response from storage server");
        printf("[File Deletion] Failed to receive response from SS%d\n", primary_ss_id);
        close(ss_socket);
        send_message(socket, &response);
        return;
    }
    
    close(ss_socket);
    
    // If storage server deletion was successful, remove from our records
    if (ss_response.error_code == ERR_SUCCESS) {
        remove_file_from_server(primary_ss_id, msg->filename);
        printf("[File Deletion] File '%s' deleted successfully from SS%d\n", msg->filename, primary_ss_id);
        log_operation("FILE_DELETE", msg->filename);
    } else {
        printf("[File Deletion] File '%s' deletion failed on SS%d: %s\n", 
               msg->filename, primary_ss_id, ss_response.data);
    }
    
    // Forward storage server response to client
    response.error_code = ss_response.error_code;
    strcpy(response.data, ss_response.data);
    
    send_message(socket, &response);
}

void handle_list_files(int socket, Message *msg) {
    printf("[File List] Client '%s' requesting user list\n", msg->username);
    
    Message response = {0};
    response.msg_type = MSG_RESPONSE;
    response.operation = OP_LIST;
    response.error_code = ERR_SUCCESS;
    
    char user_list[MAX_DATA_SIZE] = {0};
    int pos = 0;
    
    pthread_mutex_lock(&nm_state->client_list_mutex);
    
    if (nm_state->client_count == 0) {
        strcpy(response.data, "No users found");
    } else {
        // List each user on a new line
        for (int i = 0; i < nm_state->client_count; i++) {
            ClientInfo *client = &nm_state->clients[i];
            
            int written = snprintf(user_list + pos, MAX_DATA_SIZE - pos, 
                                  "%s\n", client->username);
            
            if (written > 0 && pos + written < MAX_DATA_SIZE) {
                pos += written;
            } else {
                break;  // Buffer full
            }
        }
        strcpy(response.data, user_list);
    }
    
    pthread_mutex_unlock(&nm_state->client_list_mutex);
    
    printf("[File List] Sent user list to client '%s' (%d users)\n", 
           msg->username, nm_state->client_count);
    send_message(socket, &response);
}

void handle_addaccess(int socket, Message *msg) {
    printf("[Access Control] Adding access for user '%s' to file '%s'\n", 
           msg->data, msg->filename);  // username to add is in data field
    
    FileInfo *file = find_file(msg->filename);
    
    Message response = {0};
    response.msg_type = MSG_ACK;
    response.operation = OP_ADDACCESS;
    
    if (!file) {
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "File not found");
        send_message(socket, &response);
        return;
    }
    
    // Check if requesting user is the owner
    if (strcmp(file->owner, msg->username) != 0) {
        response.error_code = ERR_NOT_OWNER;
        strcpy(response.data, "Only file owner can modify access permissions");
        send_message(socket, &response);
        return;
    }
    
    // Forward to storage server
    int primary_ss_id = file->primary_ss_id;
    int ss_index = find_storage_server(primary_ss_id);
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    
    int ss_socket = connect_to_server(ss->ip, ss->nm_port);
    if (ss_socket < 0) {
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "Failed to connect to storage server");
        send_message(socket, &response);
        return;
    }
    
    // Send addaccess command to storage server
    Message ss_msg = *msg;  // Copy original message
    ss_msg.operation = OP_SS_ADDACCESS;
    
    send_message(ss_socket, &ss_msg);
    
    Message ss_response = {0};
    receive_message(ss_socket, &ss_response);
    close(ss_socket);
    
    // Forward response to client
    response.error_code = ss_response.error_code;
    strcpy(response.data, ss_response.data);
    
    send_message(socket, &response);
}

void handle_remaccess(int socket, Message *msg) {
    printf("[Access Control] Removing access for user '%s' from file '%s'\n", 
           msg->data, msg->filename);  // username to remove is in data field
    
    FileInfo *file = find_file(msg->filename);
    
    Message response = {0};
    response.msg_type = MSG_ACK;
    response.operation = OP_REMACCESS;
    
    if (!file) {
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "File not found");
        send_message(socket, &response);
        return;
    }
    
    // Check if requesting user is the owner
    if (strcmp(file->owner, msg->username) != 0) {
        response.error_code = ERR_NOT_OWNER;
        strcpy(response.data, "Only file owner can modify access permissions");
        send_message(socket, &response);
        return;
    }
    
    // Forward to storage server
    int primary_ss_id = file->primary_ss_id;
    int ss_index = find_storage_server(primary_ss_id);
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    
    int ss_socket = connect_to_server(ss->ip, ss->nm_port);
    if (ss_socket < 0) {
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "Failed to connect to storage server");
        send_message(socket, &response);
        return;
    }
    
    // Send remaccess command to storage server
    Message ss_msg = *msg;  // Copy original message
    ss_msg.operation = OP_SS_REMACCESS;
    
    send_message(ss_socket, &ss_msg);
    
    Message ss_response = {0};
    receive_message(ss_socket, &ss_response);
    close(ss_socket);
    
    // Forward response to client
    response.error_code = ss_response.error_code;
    strcpy(response.data, ss_response.data);
    
    send_message(socket, &response);
}

// Helper function to check if user has access to a file
static int user_has_access(const char *filename, const char *username, int ss_id) {
    // Find the storage server
    int ss_index = find_storage_server(ss_id);
    if (ss_index < 0) return 0;
    
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    
    // Connect to storage server to check access
    int ss_socket = connect_to_server(ss->ip, ss->nm_port);
    if (ss_socket < 0) return 0;
    
    // Send INFO request to get file details
    Message info_msg = {0};
    info_msg.msg_type = MSG_REQUEST;
    info_msg.operation = OP_INFO;
    strcpy(info_msg.filename, filename);
    strcpy(info_msg.username, username);
    
    if (send_message(ss_socket, &info_msg) < 0) {
        close(ss_socket);
        return 0;
    }
    
    Message info_response = {0};
    if (receive_message(ss_socket, &info_response) < 0) {
        close(ss_socket);
        return 0;
    }
    
    close(ss_socket);
    
    // If we got SUCCESS or NO_READ_ACCESS, user is known to the file
    // If NO_READ_ACCESS, user is in access list but doesn't have read permission
    // We consider this as "has access" for VIEW without -a flag
    if (info_response.error_code == ERR_SUCCESS || 
        info_response.error_code == ERR_NO_READ_ACCESS) {
        return 1;
    }
    char owner_search[MAX_USERNAME + 10];
    snprintf(owner_search, sizeof(owner_search), "Owner: %s", username);
    if (strstr(info_response.data, owner_search) != NULL) {
        return 1; // User is the owner
    }

    // 2. Check if the user is in the access list
    char *access_line = strstr(info_response.data, "Access: ");
    if (access_line == NULL) {
        return 0; // No access line found, default to no access
    }

    // Create search strings for Read or Read/Write access
    char access_r[MAX_USERNAME + 5];
    char access_rw[MAX_USERNAME + 5];
    snprintf(access_r, sizeof(access_r), "%s(R)", username);
    snprintf(access_rw, sizeof(access_rw), "%s(RW)", username);

    if (strstr(access_line, access_r) != NULL || strstr(access_line, access_rw) != NULL) {
        return 1; // User found in access list
    }
    
    // *** END OF FIX ***

    // User is not the owner and not in the access list
    return 0;
}

// Helper function to get detailed file info from storage server
static int get_file_details(const char *filename, int ss_id, char *details, int max_len) {
    int ss_index = find_storage_server(ss_id);
    if (ss_index < 0) return -1;
    
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    
    int ss_socket = connect_to_server(ss->ip, ss->nm_port);
    if (ss_socket < 0) return -1;
    
    // Send INFO request
    Message info_msg = {0};
    info_msg.msg_type = MSG_REQUEST;
    info_msg.operation = OP_INFO;
    strcpy(info_msg.filename, filename);
    strcpy(info_msg.username, "system");  // Use system to get full info
    
    if (send_message(ss_socket, &info_msg) < 0) {
        close(ss_socket);
        return -1;
    }
    
    Message info_response = {0};
    if (receive_message(ss_socket, &info_response) < 0) {
        close(ss_socket);
        return -1;
    }
    
    close(ss_socket);
    
    if (info_response.error_code == ERR_SUCCESS) {
        strncpy(details, info_response.data, max_len - 1);
        return 0;
    }
    
    return -1;
}

void handle_view_files(int socket, Message *msg) {
    printf("[File View] Client '%s' requesting file view with flags %d\n", 
           msg->username, msg->sentence_index);
    
    Message response = {0};
    response.msg_type = MSG_RESPONSE;
    response.operation = OP_VIEW;
    response.error_code = ERR_SUCCESS;
    
    int view_flags = msg->sentence_index;  // Flags passed in sentence_index field
    int show_all = (view_flags == VIEW_FLAG_ALL || view_flags == VIEW_FLAG_ALL_LONG);
    int show_long = (view_flags == VIEW_FLAG_LONG || view_flags == VIEW_FLAG_ALL_LONG);
    
    char file_list[MAX_DATA_SIZE] = {0};
    int pos = 0;
    
    // Add header for long format
    if (show_long) {
        pos += snprintf(file_list + pos, MAX_DATA_SIZE - pos,
                       "%-30s %-15s %-10s %-10s %-10s\n",
                       "FILENAME", "OWNER", "WORDS", "CHARS", "SS");
        pos += snprintf(file_list + pos, MAX_DATA_SIZE - pos,
                       "%-30s %-15s %-10s %-10s %-10s\n",
                       "--------", "-----", "-----", "-----", "--");
    }
    
    pthread_mutex_lock(&nm_state->ss_list_mutex);
    
    for (int i = 0; i < nm_state->ss_count; i++) {
        StorageServerInfo *ss = &nm_state->storage_servers[i];
        
        pthread_mutex_lock(&ss->ss_mutex);
        for (int j = 0; j < ss->file_count; j++) {
            FileInfo *file = &ss->files[j];
            
            // Access control logic:
            // - Without -a flag: show files user has access to (owned OR granted access)
            // - With -a flag: show ALL files in the system (no filtering)
            
            int is_owner = (strcmp(file->owner, msg->username) == 0);
            int has_access = 0;
            
            if (!is_owner) {
                // Check if user has access to this file
                has_access = user_has_access(file->filename, msg->username, file->primary_ss_id);
            }
            
            // With -a flag: show all files (no filtering)
            // Without -a flag: only show files user owns or has been granted access to
            if (!show_all) {
                // User must be owner OR have access to see the file
                if (!is_owner && !has_access) {
                    continue;  // Skip files user has no access to
                }
            }
            
            int written;
            if (show_long) {
                // Get detailed info from storage server
                char details[512] = {0};
                if (get_file_details(file->filename, file->primary_ss_id, details, sizeof(details)) == 0) {
                    // Parse details: "File: <name>\nOwner: <owner>\n...\nWords: <count>\nCharacters: <count>\n..."
                    int words = 0, chars = 0;
                    // Parse the multi-line response
                    char *words_line = strstr(details, "Words: ");
                    char *chars_line = strstr(details, "Characters: ");
                    if (words_line) sscanf(words_line, "Words: %d", &words);
                    if (chars_line) sscanf(chars_line, "Characters: %d", &chars);
                    
                    written = snprintf(file_list + pos, MAX_DATA_SIZE - pos,
                                      "%-30s %-15s %-10d %-10d SS%-8d\n",
                                      file->filename, file->owner, words, chars, file->primary_ss_id);
                } else {
                    // Fallback if can't get details
                    written = snprintf(file_list + pos, MAX_DATA_SIZE - pos,
                                      "%-30s %-15s %-10s %-10s SS%-8d\n",
                                      file->filename, file->owner, "N/A", "N/A", file->primary_ss_id);
                }
            } else {
                // Simple format: just filename
                written = snprintf(file_list + pos, MAX_DATA_SIZE - pos,
                                  "%s\n", file->filename);
            }
            
            if (written > 0 && pos + written < MAX_DATA_SIZE) {
                pos += written;
            } else {
                pthread_mutex_unlock(&ss->ss_mutex);
                goto buffer_full;
            }
        }
        pthread_mutex_unlock(&ss->ss_mutex);
    }
    
buffer_full:
    pthread_mutex_unlock(&nm_state->ss_list_mutex);
    
    if (pos == 0 || (show_long && pos <= 122)) {  // Only header
        strcpy(response.data, "No files found");
    } else {
        strcpy(response.data, file_list);
    }
    
    printf("[File View] Sent file view to client '%s' (%d bytes)\n", msg->username, pos);
    send_message(socket, &response);
}
