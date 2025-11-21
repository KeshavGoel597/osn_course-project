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
    
    // Try cache first for O(1) lookup
    int cached_primary_ss_id, cached_backup_ss_id;
    if (cache_lookup(msg->filename, &cached_primary_ss_id, &cached_backup_ss_id)) {
        // Cache hit! Use cached values
        printf("[File Location] Cache HIT - Using cached location\n");
        
        // Verify the server is still online
        int ss_index = find_storage_server(cached_primary_ss_id);
        if (ss_index >= 0 && nm_state->storage_servers[ss_index].status != SS_STATUS_OFFLINE) {
            // Send cached info
            Message response = {0};
            response.msg_type = MSG_RESPONSE;
            response.operation = OP_GET_SS_INFO;
            response.error_code = ERR_SUCCESS;
            response.ss_id = cached_primary_ss_id;
            strcpy(response.ip, nm_state->storage_servers[ss_index].ip);
            response.port1 = nm_state->storage_servers[ss_index].client_port;
            send_message(socket, &response);
            log_operation("GET_SS_INFO_CACHED", msg->filename);
            return;
        }
        // If server is offline, fall through to do full lookup and update cache
        printf("[File Location] Cached server offline, performing full lookup\n");
    }
    
    // Cache miss or stale - do full lookup
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
    
    // Update cache with current location
    cache_insert(msg->filename, file->primary_ss_id, file->backup_ss_id);
    
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
        
        // Trigger asynchronous replication to backup server
        replicate_all_writes_async(msg->filename, "CREATE", msg->username);
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
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "File not found");
        printf("[File Deletion] File '%s' not found\n", msg->filename);
        send_message(socket, &response);
        return;
    }
    
    // Check if user is the owner (simple ownership check)
    printf("[File Deletion] Checking ownership: file owner='%s', requesting user='%s'\n", 
           file->owner, msg->username);
    if (strcmp(file->owner, msg->username) != 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_NOT_OWNER;
        strcpy(response.data, "Only file owner can delete the file");
        printf("[File Deletion] User '%s' is not owner of file '%s' (owner is '%s')\n", 
               msg->username, msg->filename, file->owner);
        send_message(socket, &response);
        return;
    }
    printf("[File Deletion] Ownership check passed\n");
    
    // Send delete command to storage server
    int primary_ss_id = file->primary_ss_id;
    int ss_index = find_storage_server(primary_ss_id);
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    
    int ss_socket = connect_to_server(ss->ip, ss->nm_port);
    if (ss_socket < 0) {
        response.msg_type = MSG_ERROR;
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
        response.msg_type = MSG_ERROR;
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
        response.msg_type = MSG_ERROR;
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
        
        // Invalidate cache entry for deleted file
        cache_invalidate(msg->filename);
        
        printf("[File Deletion] File '%s' deleted successfully from SS%d\n", msg->filename, primary_ss_id);
        log_operation("FILE_DELETE", msg->filename);
        
        // Trigger asynchronous replication of deletion to backup server
        replicate_all_writes_async(msg->filename, "DELETE", "");
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
    printf("[User List] Client '%s' requesting user list\n", msg->username);
    
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
        // List each user on a new line (prepend with --> for formatting)
        for (int i = 0; i < nm_state->client_count; i++) {
            ClientInfo *client = &nm_state->clients[i];
            
            int written = snprintf(user_list + pos, MAX_DATA_SIZE - pos, 
                                  "--> %s\n", client->username);
            
            if (written > 0 && pos + written < MAX_DATA_SIZE) {
                pos += written;
            } else {
                break;  // Buffer full
            }
        }
        strcpy(response.data, user_list);
    }
    
    pthread_mutex_unlock(&nm_state->client_list_mutex);
    
    printf("[User List] Sent user list to client '%s' (%d users)\n", 
           msg->username, nm_state->client_count);
    send_message(socket, &response);
}

void handle_addaccess(int socket, Message *msg) {
    printf("[Access Control] Adding access for user '%s' to file '%s'\n", 
           msg->data, msg->filename);  // username to add is in data field
    printf("[DEBUG] handle_addaccess: username=%s, filename=%s, target_user=%s, access_type=%d\n",
           msg->username, msg->filename, msg->data, msg->sentence_index);
    
    FileInfo *file = find_file(msg->filename);
    
    Message response = {0};
    response.msg_type = MSG_ACK;
    response.operation = OP_ADDACCESS;
    
    if (!file) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "File not found");
        send_message(socket, &response);
        return;
    }
    
    // Check if requesting user is the owner
    if (strcmp(file->owner, msg->username) != 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_NOT_OWNER;
        strcpy(response.data, "Only file owner can modify access permissions");
        send_message(socket, &response);
        return;
    }
    
    // Forward to storage server
    int primary_ss_id = file->primary_ss_id;
    int ss_index = find_storage_server(primary_ss_id);
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    
    printf("[DEBUG] Connecting to storage server: %s:%d (ss_id=%d)\n", 
           ss->ip, ss->nm_port, primary_ss_id);
    int ss_socket = connect_to_server(ss->ip, ss->nm_port);
    if (ss_socket < 0) {
        printf("[DEBUG] Failed to connect to storage server!\n");
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "Failed to connect to storage server");
        send_message(socket, &response);
        return;
    }
    
    printf("[DEBUG] Successfully connected to storage server on socket %d\n", ss_socket);
    
    // Send addaccess command to storage server
    Message ss_msg = *msg;  // Copy original message
    ss_msg.operation = OP_SS_ADDACCESS;
    
    printf("[DEBUG] Prepared message for SS: operation=%d (should be %d)\n", 
           ss_msg.operation, OP_SS_ADDACCESS);
    printf("[DEBUG] Message details: msg_type=%d, username=%s, filename=%s, data=%s, sentence_index=%d\n",
           ss_msg.msg_type, ss_msg.username, ss_msg.filename, ss_msg.data, ss_msg.sentence_index);
    printf("[DEBUG] Sending OP_SS_ADDACCESS to storage server\n");
    
    if (send_message(ss_socket, &ss_msg) < 0) {
        printf("[DEBUG] Failed to send message to storage server\n");
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "Failed to send to storage server");
        close(ss_socket);
        send_message(socket, &response);
        return;
    }
    
    printf("[DEBUG] Message sent successfully to storage server\n");
    
    printf("[DEBUG] Waiting for response from storage server...\n");
    Message ss_response = {0};
    receive_message(ss_socket, &ss_response);
    printf("[DEBUG] Received response from storage server: error_code=%d, data=%s\n",
           ss_response.error_code, ss_response.data);
    close(ss_socket);
    
    // Forward response to client
    if (ss_response.error_code != ERR_SUCCESS) {
        response.msg_type = MSG_ERROR;
    }
    response.error_code = ss_response.error_code;
    strcpy(response.data, ss_response.data);
    
    printf("[DEBUG] Sending response to client: msg_type=%d, error_code=%d\n",
           response.msg_type, response.error_code);
    send_message(socket, &response);
}

void handle_remaccess(int socket, Message *msg) {
    printf("[Access Control] Removing access for user '%s' from file '%s'\n", 
           msg->data, msg->filename);  // username to remove is in data field
    printf("[DEBUG] handle_remaccess: username=%s, filename=%s, target_user=%s\n",
           msg->username, msg->filename, msg->data);
    
    FileInfo *file = find_file(msg->filename);
    
    Message response = {0};
    response.msg_type = MSG_ACK;
    response.operation = OP_REMACCESS;
    
    if (!file) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "File not found");
        send_message(socket, &response);
        return;
    }
    
    // Check if requesting user is the owner
    if (strcmp(file->owner, msg->username) != 0) {
        response.msg_type = MSG_ERROR;
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
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "Failed to connect to storage server");
        send_message(socket, &response);
        return;
    }
    
    // Send remaccess command to storage server
    Message ss_msg = *msg;  // Copy original message
    ss_msg.operation = OP_SS_REMACCESS;
    
    printf("[DEBUG] Sending OP_SS_REMACCESS to storage server\n");
    send_message(ss_socket, &ss_msg);
    
    printf("[DEBUG] Waiting for response from storage server...\n");
    Message ss_response = {0};
    receive_message(ss_socket, &ss_response);
    printf("[DEBUG] Received response from storage server: error_code=%d, data=%s\n",
           ss_response.error_code, ss_response.data);
    close(ss_socket);
    
    // Forward response to client
    if (ss_response.error_code != ERR_SUCCESS) {
        response.msg_type = MSG_ERROR;
    }
    response.error_code = ss_response.error_code;
    strcpy(response.data, ss_response.data);
    
    printf("[DEBUG] Sending response to client: msg_type=%d, error_code=%d\n",
           response.msg_type, response.error_code);
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
    
    // If the INFO request failed (file doesn't exist, etc.), user has no access
    if (info_response.error_code != ERR_SUCCESS && 
        info_response.error_code != ERR_NO_READ_ACCESS &&
        info_response.error_code != ERR_NO_WRITE_ACCESS) {
        return 0;
    }
    
    // 1. Check if the user is the owner
    char owner_search[MAX_USERNAME + 10];
    snprintf(owner_search, sizeof(owner_search), "Owner: %s", username);
    if (strstr(info_response.data, owner_search) != NULL) {
        return 1; // User is the owner
    }

    // 2. Check if the user is in the access list
    char *access_line = strstr(info_response.data, "Access: ");
    if (access_line == NULL) {
        return 0; 
    }

    // Create search strings with COLONS (not parentheses)
    char access_r[MAX_USERNAME + 5];
    char access_rw[MAX_USERNAME + 5];
    snprintf(access_r, sizeof(access_r), "%s:R", username);
    snprintf(access_rw, sizeof(access_rw), "%s:RW", username);

    // Check for "user:R" or "user:RW" in the access line
    if (strstr(access_line, access_r) != NULL || strstr(access_line, access_rw) != NULL) {
        return 1; // User found in access list
    }
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

void handle_info_request(int socket, Message *msg) {
    printf("[File Info] Client '%s' requesting info for file '%s'\n", 
           msg->username, msg->filename);
    
    FileInfo *file = find_file(msg->filename);
    
    Message response = {0};
    response.msg_type = MSG_RESPONSE;
    response.operation = OP_INFO;
    
    if (!file) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        snprintf(response.data, MAX_DATA_SIZE, "File '%s' not found", msg->filename);
        printf("[File Info] File '%s' not found\n", msg->filename);
        send_message(socket, &response);
        return;
    }
    
    // Get Storage Server info
    int ss_index = find_storage_server(file->primary_ss_id);
    if (ss_index < 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Storage server not available");
        printf("[File Info] Storage server SS%d not found\n", file->primary_ss_id);
        send_message(socket, &response);
        return;
    }
    
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    
    // Connect to storage server to get detailed info
    int ss_socket = connect_to_server(ss->ip, ss->nm_port);
    if (ss_socket < 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "Failed to connect to storage server");
        printf("[File Info] Failed to connect to SS%d\n", file->primary_ss_id);
        send_message(socket, &response);
        return;
    }
    
    // Forward INFO request to storage server
    Message ss_msg = {0};
    ss_msg.msg_type = MSG_REQUEST;
    ss_msg.operation = OP_INFO;
    strcpy(ss_msg.filename, msg->filename);
    strcpy(ss_msg.username, msg->username);
    
    if (send_message(ss_socket, &ss_msg) < 0) {
        close(ss_socket);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Failed to request file info");
        printf("[File Info] Failed to send INFO request to SS%d\n", file->primary_ss_id);
        send_message(socket, &response);
        return;
    }
    
    // Receive response from storage server
    Message ss_response = {0};
    if (receive_message(ss_socket, &ss_response) < 0) {
        close(ss_socket);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Failed to receive file info");
        printf("[File Info] Failed to receive response from SS%d\n", file->primary_ss_id);
        send_message(socket, &response);
        return;
    }
    
    close(ss_socket);
    
    // Forward storage server response to client
    response.msg_type = ss_response.msg_type;
    response.error_code = ss_response.error_code;
    strcpy(response.data, ss_response.data);
    
    if (ss_response.error_code == ERR_SUCCESS) {
        printf("[File Info] Successfully retrieved info for '%s'\n", msg->filename);
    } else {
        printf("[File Info] Failed to retrieve info for '%s': error %d\n", 
               msg->filename, ss_response.error_code);
    }
    
    send_message(socket, &response);
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
                       "---------------------------------------------------------\n");
        pos += snprintf(file_list + pos, MAX_DATA_SIZE - pos,
                       "| %-12s | %-7s | %-7s | %-16s | %-7s |\n",
                       "Filename", "Words", "Chars", "Last Access", "Owner");
        pos += snprintf(file_list + pos, MAX_DATA_SIZE - pos,
                       "|--------------|---------|---------|------------------|-------|\n");
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
            
            // With -a flag: show all files (no filtering)
            // Without -a flag: only show files user owns or has been granted access to
            if (!show_all) {
                int is_owner = (strcmp(file->owner, msg->username) == 0);
                int has_access = 0;
                
                if (!is_owner) {
                    // Check if user has access to this file
                    has_access = user_has_access(file->filename, msg->username, file->primary_ss_id);
                }
                
                // Skip files where user is NOT owner AND does NOT have access
                // In other words: only show files where user IS owner OR has access
                if (!is_owner && !has_access) {
                    continue;  // Skip files user has no access to
                }
            }
            
            int written;
            if (show_long) {
                // Get detailed info from storage server
                char details[512] = {0};
                if (get_file_details(file->filename, file->primary_ss_id, details, sizeof(details)) == 0) {
                    // Parse details: "File: <name>\nOwner: <owner>\n...\nWords: <count>\nCharacters: <count>\nLast Accessed: <time>\n..."
                    int words = 0, chars = 0;
                    char last_access[64] = "N/A";
                    
                    // Parse the multi-line response
                    char *words_line = strstr(details, "Words: ");
                    char *chars_line = strstr(details, "Characters: ");
                    char *access_line = strstr(details, "Last Accessed: ");
                    
                    if (words_line) sscanf(words_line, "Words: %d", &words);
                    if (chars_line) sscanf(chars_line, "Characters: %d", &chars);
                    if (access_line) {
                        // Extract timestamp (format: "Last Accessed: 2025-10-10 14:32")
                        sscanf(access_line, "Last Accessed: %63[^\n]", last_access);
                    }
                    
                    written = snprintf(file_list + pos, MAX_DATA_SIZE - pos,
                                      "| %-12s | %7d | %7d | %-16s | %-7s |\n",
                                      file->filename, words, chars, last_access, file->owner);
                } else {
                    // Fallback if can't get details
                    written = snprintf(file_list + pos, MAX_DATA_SIZE - pos,
                                      "| %-12s | %7s | %7s | %-16s | %-7s |\n",
                                      file->filename, "N/A", "N/A", "N/A", file->owner);
                }
            } else {
                // Simple format: just filename with --> prefix
                written = snprintf(file_list + pos, MAX_DATA_SIZE - pos,
                                  "--> %s\n", file->filename);
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
    
    if (pos == 0 || (show_long && pos <= 200)) {  // Only header
        strcpy(response.data, "No files found");
    } else {
        // Add closing border for long format
        if (show_long) {
            snprintf(file_list + pos, MAX_DATA_SIZE - pos,
                    "---------------------------------------------------------\n");
        }
        strcpy(response.data, file_list);
    }
    
    printf("[File View] Sent file view to client '%s' (%d bytes)\n", msg->username, pos);
    send_message(socket, &response);
}

// Execute file contents as shell commands on Name Server
void handle_exec(int socket, Message *msg) {
    printf("[File Exec] Client '%s' requesting execution of file '%s'\n", 
           msg->username, msg->filename);
    
    Message response = {0};
    response.msg_type = MSG_RESPONSE;
    response.operation = OP_EXEC;
    response.error_code = ERR_SUCCESS;
    
    // 1. Find the file in the system
    FileInfo *file = find_file(msg->filename);
    
    if (!file) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        snprintf(response.data, MAX_DATA_SIZE, "File '%s' not found", msg->filename);
        printf("[File Exec] File '%s' not found\n", msg->filename);
        send_message(socket, &response);
        return;
    }
    
    // 2. Check access permissions (user must have read access or be owner)
    int is_owner = (strcmp(file->owner, msg->username) == 0);
    int has_access = 0;
    
    if (!is_owner) {
        // Check if user has access to this file
        has_access = user_has_access(msg->filename, msg->username, file->primary_ss_id);
    }
    
    if (!is_owner && !has_access) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_NO_READ_ACCESS;
        snprintf(response.data, MAX_DATA_SIZE, "No read access to file '%s'", msg->filename);
        printf("[File Exec] User '%s' has no read access to file '%s'\n", 
               msg->username, msg->filename);
        send_message(socket, &response);
        return;
    }
    
    // 3. Request file content from Storage Server
    int ss_index = find_storage_server(file->primary_ss_id);
    if (ss_index < 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Storage server not available");
        printf("[File Exec] Storage server SS%d not found\n", file->primary_ss_id);
        send_message(socket, &response);
        return;
    }
    
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    
    // Connect to storage server
    int ss_socket = connect_to_server(ss->ip, ss->nm_port);
    if (ss_socket < 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "Failed to connect to storage server");
        printf("[File Exec] Failed to connect to SS%d at %s:%d\n", 
               file->primary_ss_id, ss->ip, ss->nm_port);
        send_message(socket, &response);
        return;
    }
    
    // Send EXEC request to SS to get file content
    Message exec_msg = {0};
    exec_msg.msg_type = MSG_REQUEST;
    exec_msg.operation = OP_EXEC;
    strcpy(exec_msg.filename, msg->filename);
    strcpy(exec_msg.username, msg->username);
    
    if (send_message(ss_socket, &exec_msg) < 0) {
        close(ss_socket);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Failed to request file content");
        printf("[File Exec] Failed to send EXEC request to SS%d\n", file->primary_ss_id);
        send_message(socket, &response);
        return;
    }
    
    // Receive file content from SS
    Message ss_response = {0};
    if (receive_message(ss_socket, &ss_response) < 0) {
        close(ss_socket);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Failed to receive file content");
        printf("[File Exec] Failed to receive response from SS%d\n", file->primary_ss_id);
        send_message(socket, &response);
        return;
    }
    
    // Check if SS returned an error
    if (ss_response.error_code != ERR_SUCCESS) {
        close(ss_socket);
        response.msg_type = MSG_ERROR;
        response.error_code = ss_response.error_code;
        strcpy(response.data, ss_response.data);
        printf("[File Exec] SS%d returned error: %d\n", file->primary_ss_id, ss_response.error_code);
        send_message(socket, &response);
        return;
    }
    
    // Get file size from initial response
    long file_size = ss_response.sentence_index;
    printf("[File Exec] File size: %ld bytes\n", file_size);
    
    // Allocate buffer for file content
    char *file_content = malloc(file_size + 1);
    if (!file_content) {
        close(ss_socket);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Out of memory");
        printf("[File Exec] Failed to allocate %ld bytes for file content\n", file_size);
        send_message(socket, &response);
        return;
    }
    
    // Receive file content in chunks
    size_t total_received = 0;
    int chunk_num = 0;
    
    while (total_received < file_size) {
        Message chunk_msg = {0};
        if (receive_message(ss_socket, &chunk_msg) < 0) {
            free(file_content);
            close(ss_socket);
            response.msg_type = MSG_ERROR;
            response.error_code = ERR_SERVER_ERROR;
            strcpy(response.data, "Failed to receive file chunk");
            printf("[File Exec] Failed to receive chunk from SS%d\n", file->primary_ss_id);
            send_message(socket, &response);
            return;
        }
        
        // Check for STOP message
        if (chunk_msg.operation == OP_STOP) {
            printf("[File Exec] Received STOP after %zu bytes\n", total_received);
            break;
        }
        
        if (chunk_msg.operation == OP_EXEC_CHUNK) {
            size_t chunk_size = chunk_msg.sentence_index;
            memcpy(file_content + total_received, chunk_msg.data, chunk_size);
            total_received += chunk_size;
            chunk_num++;
            
            if (chunk_num % 10 == 0) {
                printf("[File Exec] Received chunk %d: %zu/%ld bytes\n", 
                       chunk_num, total_received, file_size);
            }
        }
    }
    
    file_content[total_received] = '\0';
    printf("[File Exec] Received complete file: %zu bytes in %d chunks\n", 
           total_received, chunk_num);
    
    close(ss_socket);
    
    // 4. Execute the file content as shell commands on Name Server
    printf("[File Exec] Executing commands from file '%s'\n", msg->filename);
    printf("[File Exec] File content:\n%s\n", file_content);
    
    // Create a temporary file to hold the commands
    char temp_file[256];
    snprintf(temp_file, sizeof(temp_file), "/tmp/nm_exec_%s_%ld.sh", 
             msg->username, (long)time(NULL));
    
    FILE *fp = fopen(temp_file, "w");
    if (!fp) {
        free(file_content);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Failed to create temporary execution file");
        printf("[File Exec] Failed to create temp file: %s\n", temp_file);
        send_message(socket, &response);
        return;
    }
    
    // Write the commands to temp file
    fprintf(fp, "%s", file_content);
    fclose(fp);
    
    // Free the file content buffer (no longer needed)
    free(file_content);
    
    // Execute the commands and capture output
    char exec_cmd[512];
    snprintf(exec_cmd, sizeof(exec_cmd), "bash %s 2>&1", temp_file);
    
    FILE *pipe = popen(exec_cmd, "r");
    if (!pipe) {
        unlink(temp_file);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Failed to execute commands");
        printf("[File Exec] Failed to execute: %s\n", exec_cmd);
        send_message(socket, &response);
        return;
    }
    
    // Read output from command execution
    char output[MAX_DATA_SIZE] = {0};
    size_t output_len = 0;
    char buffer[256];
    
    while (fgets(buffer, sizeof(buffer), pipe) != NULL && output_len < MAX_DATA_SIZE - 1) {
        size_t len = strlen(buffer);
        if (output_len + len < MAX_DATA_SIZE - 1) {
            strcpy(output + output_len, buffer);
            output_len += len;
        } else {
            // Buffer full, truncate
            break;
        }
    }
    
    int exec_status = pclose(pipe);
    unlink(temp_file);  // Clean up temp file
    
    // 5. Send execution output back to client
    if (output_len == 0) {
        if (exec_status == 0) {
            strcpy(response.data, "[No output - command executed successfully]");
        } else {
            snprintf(response.data, MAX_DATA_SIZE, 
                    "[Command execution failed with status %d]", exec_status);
            response.error_code = ERR_SERVER_ERROR;
        }
    } else {
        strncpy(response.data, output, MAX_DATA_SIZE - 1);
        response.data[MAX_DATA_SIZE - 1] = '\0';
    }
    
    printf("[File Exec] Execution complete. Output length: %zu bytes, Status: %d\n", 
           output_len, exec_status);
    printf("[File Exec] Sending result to client '%s'\n", msg->username);
    
    send_message(socket, &response);
    
    log_operation("FILE_EXEC", msg->filename);
}

// Handle CREATEFOLDER request
void handle_createfolder(int socket, Message *msg) {
    printf("[Folder] Client '%s' creating folder '%s'\n", msg->username, msg->filename);
    
    Message response = {0};
    response.msg_type = MSG_RESPONSE;
    response.operation = OP_CREATEFOLDER;
    response.error_code = ERR_SUCCESS;
    
    // Check if folder already exists
    FileInfo *existing = find_file(msg->filename);
    if (existing) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_FILE_EXISTS;
        strcpy(response.data, "Folder already exists");
        send_message(socket, &response);
        return;
    }
    
    // Get available storage server
    int ss_id = get_available_primary_server();
    if (ss_id < 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "No storage server available");
        send_message(socket, &response);
        return;
    }
    
    // Get storage server info
    int ss_index = find_storage_server(ss_id);
    if (ss_index < 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Storage server not available");
        send_message(socket, &response);
        return;
    }
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    if (ss->status != SS_STATUS_ONLINE) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Storage server not available");
        send_message(socket, &response);
        return;
    }
    
    // Send folder creation request to storage server
    int ss_socket = connect_to_server(ss->ip, ss->nm_port);
    if (ss_socket < 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "Failed to connect to storage server");
        send_message(socket, &response);
        return;
    }
    
    Message ss_request = {0};
    ss_request.msg_type = MSG_REQUEST;
    ss_request.operation = OP_CREATEFOLDER;
    strcpy(ss_request.filename, msg->filename);
    strcpy(ss_request.username, msg->username);
    
    if (send_message(ss_socket, &ss_request) < 0) {
        close_socket(ss_socket);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Failed to send request to storage server");
        send_message(socket, &response);
        return;
    }
    
    Message ss_response;
    if (receive_message(ss_socket, &ss_response) < 0 || ss_response.msg_type == MSG_ERROR) {
        close_socket(ss_socket);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Storage server failed to create folder");
        send_message(socket, &response);
        return;
    }
    
    close_socket(ss_socket);
    
    // Add folder to file tracking (mark as directory)
    add_file_to_server(ss_id, msg->filename, msg->username);
    
    strcpy(response.data, "Folder created successfully");
    printf("[Folder] Folder '%s' created successfully on SS%d\n", msg->filename, ss_id);
    
    send_message(socket, &response);
    log_operation("CREATEFOLDER", msg->filename);
}

// Handle MOVE request
void handle_move_file(int socket, Message *msg) {
    printf("[File Move] Client '%s' moving file '%s' to '%s'\n", 
           msg->username, msg->filename, msg->target_path);
    
    Message response = {0};
    response.msg_type = MSG_RESPONSE;
    response.operation = OP_MOVE;
    response.error_code = ERR_SUCCESS;
    
    // Find source file
    FileInfo *file = find_file(msg->filename);
    if (!file) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "File not found");
        send_message(socket, &response);
        return;
    }
    
    // Check permissions (only owner can move)
    if (strcmp(file->owner, msg->username) != 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_ACCESS_DENIED;
        strcpy(response.data, "Only file owner can move files");
        send_message(socket, &response);
        return;
    }
    
    // Get storage server
    int ss_index = find_storage_server(file->primary_ss_id);
    if (ss_index < 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Storage server not available");
        send_message(socket, &response);
        return;
    }
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    if (ss->status != SS_STATUS_ONLINE) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Storage server not available");
        send_message(socket, &response);
        return;
    }
    
    // Send move request to storage server
    int ss_socket = connect_to_server(ss->ip, ss->nm_port);
    if (ss_socket < 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "Failed to connect to storage server");
        send_message(socket, &response);
        return;
    }
    
    Message ss_request = {0};
    ss_request.msg_type = MSG_REQUEST;
    ss_request.operation = OP_MOVE;
    strcpy(ss_request.filename, msg->filename);
    strcpy(ss_request.target_path, msg->target_path);
    strcpy(ss_request.username, msg->username);
    
    if (send_message(ss_socket, &ss_request) < 0) {
        close_socket(ss_socket);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Failed to send request to storage server");
        send_message(socket, &response);
        return;
    }
    
    Message ss_response;
    if (receive_message(ss_socket, &ss_response) < 0 || ss_response.msg_type == MSG_ERROR) {
        close_socket(ss_socket);
        response.msg_type = MSG_ERROR;
        response.error_code = ss_response.error_code;
        strcpy(response.data, ss_response.data);
        send_message(socket, &response);
        return;
    }
    
    close_socket(ss_socket);
    
    // CRITICAL FIX: Update hash table AND file path
    // After successful move, filename should be "foldername/filename"
    char new_path[MAX_FILENAME];
    snprintf(new_path, MAX_FILENAME, "%s/%s", msg->target_path, msg->filename);
    
    // Step 1: Remove OLD filename from hash table
    hash_remove_file(msg->filename);
    
    // Step 2: Update the filename in the FileInfo structure
    pthread_mutex_lock(&ss->ss_mutex);
    strncpy(file->filename, new_path, MAX_FILENAME - 1);
    file->filename[MAX_FILENAME - 1] = '\0';
    pthread_mutex_unlock(&ss->ss_mutex);
    
    // Step 3: Insert NEW filename into hash table
    hash_insert_file(file);
    
    // Step 4: Clear cache since file path has changed
    cache_clear();
    
    strcpy(response.data, "File moved successfully");
    printf("[File Move] File '%s' moved to '%s' successfully\n", msg->filename, new_path);
    
    send_message(socket, &response);
    log_operation("MOVE", msg->filename);
}

// Handle VIEWFOLDER request
void handle_viewfolder(int socket, Message *msg) {
    printf("[Folder View] Client '%s' viewing folder '%s'\n", msg->username, msg->filename);
    
    Message response = {0};
    response.msg_type = MSG_RESPONSE;
    response.operation = OP_VIEWFOLDER;
    response.error_code = ERR_SUCCESS;
    
    // Find folder
    FileInfo *folder = find_file(msg->filename);
    if (!folder) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "Folder not found");
        send_message(socket, &response);
        return;
    }
    
    // Get storage server
    int ss_index = find_storage_server(folder->primary_ss_id);
    if (ss_index < 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Storage server not available");
        send_message(socket, &response);
        return;
    }
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    if (ss->status != SS_STATUS_ONLINE) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Storage server not available");
        send_message(socket, &response);
        return;
    }
    
    // Send viewfolder request to storage server
    int ss_socket = connect_to_server(ss->ip, ss->nm_port);
    if (ss_socket < 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "Failed to connect to storage server");
        send_message(socket, &response);
        return;
    }
    
    Message ss_request = {0};
    ss_request.msg_type = MSG_REQUEST;
    ss_request.operation = OP_VIEWFOLDER;
    strcpy(ss_request.filename, msg->filename);
    strcpy(ss_request.username, msg->username);
    
    if (send_message(ss_socket, &ss_request) < 0) {
        close_socket(ss_socket);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Failed to send request to storage server");
        send_message(socket, &response);
        return;
    }
    
    Message ss_response;
    if (receive_message(ss_socket, &ss_response) < 0) {
        close_socket(ss_socket);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Failed to receive response from storage server");
        send_message(socket, &response);
        return;
    }
    
    close_socket(ss_socket);
    
    // Forward storage server response to client
    response.msg_type = ss_response.msg_type;
    response.error_code = ss_response.error_code;
    strcpy(response.data, ss_response.data);
    
    send_message(socket, &response);
    log_operation("VIEWFOLDER", msg->filename);
}

// Handle CHECKPOINT request
void handle_checkpoint(int socket, Message *msg) {
    printf("[Checkpoint] Client '%s' creating checkpoint '%s' for file '%s'\n", 
           msg->username, msg->checkpoint_tag, msg->filename);
    
    Message response = {0};
    response.msg_type = MSG_RESPONSE;
    response.operation = OP_CHECKPOINT;
    response.error_code = ERR_SUCCESS;
    
    // Find file
    FileInfo *file = find_file(msg->filename);
    if (!file) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "File not found");
        send_message(socket, &response);
        return;
    }
    
    // Check read access (simplified - just check owner for now)
    // In production, would check access control list
    
    // Get storage server
    int ss_index = find_storage_server(file->primary_ss_id);
    if (ss_index < 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Storage server not available");
        send_message(socket, &response);
        return;
    }
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    if (ss->status != SS_STATUS_ONLINE) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Storage server not available");
        send_message(socket, &response);
        return;
    }
    
    // Send checkpoint request to storage server
    int ss_socket = connect_to_server(ss->ip, ss->nm_port);
    if (ss_socket < 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "Failed to connect to storage server");
        send_message(socket, &response);
        return;
    }
    
    Message ss_request = {0};
    ss_request.msg_type = MSG_REQUEST;
    ss_request.operation = OP_CHECKPOINT;
    strcpy(ss_request.filename, msg->filename);
    strcpy(ss_request.checkpoint_tag, msg->checkpoint_tag);
    strcpy(ss_request.username, msg->username);
    
    if (send_message(ss_socket, &ss_request) < 0) {
        close_socket(ss_socket);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Failed to send request to storage server");
        send_message(socket, &response);
        return;
    }
    
    Message ss_response;
    if (receive_message(ss_socket, &ss_response) < 0 || ss_response.msg_type == MSG_ERROR) {
        close_socket(ss_socket);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Storage server failed to create checkpoint");
        send_message(socket, &response);
        return;
    }
    
    close_socket(ss_socket);
    
    strcpy(response.data, "Checkpoint created successfully");
    send_message(socket, &response);
    log_operation("CHECKPOINT", msg->filename);
}

// Handle VIEWCHECKPOINT request
void handle_viewcheckpoint(int socket, Message *msg) {
    printf("[Checkpoint View] Client '%s' viewing checkpoint '%s' for file '%s'\n", 
           msg->username, msg->checkpoint_tag, msg->filename);
    
    Message response = {0};
    response.msg_type = MSG_RESPONSE;
    response.operation = OP_VIEWCHECKPOINT;
    response.error_code = ERR_SUCCESS;
    
    // Find file
    FileInfo *file = find_file(msg->filename);
    if (!file) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "File not found");
        send_message(socket, &response);
        return;
    }
    
    // Check read access (simplified)
    
    // Get storage server
    int ss_index = find_storage_server(file->primary_ss_id);
    if (ss_index < 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Storage server not available");
        send_message(socket, &response);
        return;
    }
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    if (ss->status != SS_STATUS_ONLINE) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Storage server not available");
        send_message(socket, &response);
        return;
    }
    
    // Send viewcheckpoint request to storage server
    int ss_socket = connect_to_server(ss->ip, ss->nm_port);
    if (ss_socket < 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "Failed to connect to storage server");
        send_message(socket, &response);
        return;
    }
    
    Message ss_request = {0};
    ss_request.msg_type = MSG_REQUEST;
    ss_request.operation = OP_VIEWCHECKPOINT;
    strcpy(ss_request.filename, msg->filename);
    strcpy(ss_request.checkpoint_tag, msg->checkpoint_tag);
    strcpy(ss_request.username, msg->username);
    
    if (send_message(ss_socket, &ss_request) < 0) {
        close_socket(ss_socket);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Failed to send request to storage server");
        send_message(socket, &response);
        return;
    }
    
    Message ss_response;
    if (receive_message(ss_socket, &ss_response) < 0) {
        close_socket(ss_socket);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Failed to receive response from storage server");
        send_message(socket, &response);
        return;
    }
    
    close_socket(ss_socket);
    
    // Forward storage server response to client
    response.msg_type = ss_response.msg_type;
    response.error_code = ss_response.error_code;
    strcpy(response.data, ss_response.data);
    
    send_message(socket, &response);
    log_operation("VIEWCHECKPOINT", msg->filename);
}

// Handle REVERT request
void handle_revert(int socket, Message *msg) {
    printf("[Revert] Client '%s' reverting file '%s' to checkpoint '%s'\n", 
           msg->username, msg->filename, msg->checkpoint_tag);
    
    Message response = {0};
    response.msg_type = MSG_RESPONSE;
    response.operation = OP_REVERT;
    response.error_code = ERR_SUCCESS;
    
    // Find file
    FileInfo *file = find_file(msg->filename);
    if (!file) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "File not found");
        send_message(socket, &response);
        return;
    }
    
    // Check write access (simplified - check owner)
    if (strcmp(file->owner, msg->username) != 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_NO_WRITE_ACCESS;
        strcpy(response.data, "No write access to file");
        send_message(socket, &response);
        return;
    }
    
    // Get storage server
    int ss_index = find_storage_server(file->primary_ss_id);
    if (ss_index < 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Storage server not available");
        send_message(socket, &response);
        return;
    }
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    if (ss->status != SS_STATUS_ONLINE) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Storage server not available");
        send_message(socket, &response);
        return;
    }
    
    // Send revert request to storage server
    int ss_socket = connect_to_server(ss->ip, ss->nm_port);
    if (ss_socket < 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "Failed to connect to storage server");
        send_message(socket, &response);
        return;
    }
    
    Message ss_request = {0};
    ss_request.msg_type = MSG_REQUEST;
    ss_request.operation = OP_REVERT;
    strcpy(ss_request.filename, msg->filename);
    strcpy(ss_request.checkpoint_tag, msg->checkpoint_tag);
    strcpy(ss_request.username, msg->username);
    
    if (send_message(ss_socket, &ss_request) < 0) {
        close_socket(ss_socket);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Failed to send request to storage server");
        send_message(socket, &response);
        return;
    }
    
    Message ss_response;
    if (receive_message(ss_socket, &ss_response) < 0 || ss_response.msg_type == MSG_ERROR) {
        close_socket(ss_socket);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Storage server failed to revert file");
        send_message(socket, &response);
        return;
    }
    
    close_socket(ss_socket);
    
    strcpy(response.data, "File reverted successfully");
    send_message(socket, &response);
    log_operation("REVERT", msg->filename);
}

// Handle LISTCHECKPOINTS request
void handle_listcheckpoints(int socket, Message *msg) {
    printf("[List Checkpoints] Client '%s' listing checkpoints for file '%s'\n", 
           msg->username, msg->filename);
    
    Message response = {0};
    response.msg_type = MSG_RESPONSE;
    response.operation = OP_LISTCHECKPOINTS;
    response.error_code = ERR_SUCCESS;
    
    // Find file
    FileInfo *file = find_file(msg->filename);
    if (!file) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "File not found");
        send_message(socket, &response);
        return;
    }
    
    // Check read access (simplified)
    
    // Get storage server
    int ss_index = find_storage_server(file->primary_ss_id);
    if (ss_index < 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Storage server not available");
        send_message(socket, &response);
        return;
    }
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    if (ss->status != SS_STATUS_ONLINE) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Storage server not available");
        send_message(socket, &response);
        return;
    }
    
    // Send listcheckpoints request to storage server
    int ss_socket = connect_to_server(ss->ip, ss->nm_port);
    if (ss_socket < 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "Failed to connect to storage server");
        send_message(socket, &response);
        return;
    }
    
    Message ss_request = {0};
    ss_request.msg_type = MSG_REQUEST;
    ss_request.operation = OP_LISTCHECKPOINTS;
    strcpy(ss_request.filename, msg->filename);
    strcpy(ss_request.username, msg->username);
    
    if (send_message(ss_socket, &ss_request) < 0) {
        close_socket(ss_socket);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Failed to send request to storage server");
        send_message(socket, &response);
        return;
    }
    
    Message ss_response;
    if (receive_message(ss_socket, &ss_response) < 0) {
        close_socket(ss_socket);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Failed to receive response from storage server");
        send_message(socket, &response);
        return;
    }
    
    close_socket(ss_socket);
    
    // Forward storage server response to client
    response.msg_type = ss_response.msg_type;
    response.error_code = ss_response.error_code;
    strcpy(response.data, ss_response.data);
    
    send_message(socket, &response);
    log_operation("LISTCHECKPOINTS", msg->filename);
}

// Handle REQUESTACCESS request
void handle_requestaccess(int socket, Message *msg) {
    printf("[Access Request] User '%s' requesting access to file '%s'\n", 
           msg->username, msg->filename);
    
    Message response = {0};
    response.msg_type = MSG_RESPONSE;
    response.operation = OP_REQUESTACCESS;
    response.error_code = ERR_SUCCESS;
    
    // Find file
    FileInfo *file = find_file(msg->filename);
    if (!file) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "File not found");
        send_message(socket, &response);
        return;
    }
    
    // Check if requester is already the owner
    if (strcmp(file->owner, msg->username) == 0) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_INVALID_OPERATION;
        strcpy(response.data, "You already own this file");
        send_message(socket, &response);
        return;
    }
    
    pthread_mutex_lock(&nm_state->request_mutex);
    
    // Check if request limit reached
    if (nm_state->request_count >= MAX_ACCESS_REQUESTS) {
        pthread_mutex_unlock(&nm_state->request_mutex);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Too many pending requests");
        send_message(socket, &response);
        return;
    }
    
    // Check if request already exists
    for (int i = 0; i < nm_state->request_count; i++) {
        if (strcmp(nm_state->access_requests[i].filename, msg->filename) == 0 &&
            strcmp(nm_state->access_requests[i].requester, msg->username) == 0 &&
            nm_state->access_requests[i].status == 0) {  // Pending
            pthread_mutex_unlock(&nm_state->request_mutex);
            response.msg_type = MSG_ERROR;
            response.error_code = ERR_INVALID_OPERATION;
            strcpy(response.data, "You already have a pending request for this file");
            send_message(socket, &response);
            return;
        }
    }
    
    // Create new request
    AccessRequest *req = &nm_state->access_requests[nm_state->request_count];
    
    // Create request_id with safe truncation to avoid warning
    // Format: "filename:username:timestamp" - truncate to fit in MAX_FILENAME
    char safe_filename[100];
    char safe_username[50];
    strncpy(safe_filename, msg->filename, 99);
    safe_filename[99] = '\0';
    strncpy(safe_username, msg->username, 49);
    safe_username[49] = '\0';
    
    snprintf(req->request_id, MAX_FILENAME, "%s:%s:%ld", 
             safe_filename, safe_username, (long)time(NULL));
    
    strncpy(req->filename, msg->filename, MAX_FILENAME - 1);
    strncpy(req->requester, msg->username, MAX_USERNAME - 1);
    strncpy(req->owner, file->owner, MAX_USERNAME - 1);
    req->access_type = msg->sentence_index;  // Access type passed in sentence_index
    req->request_time = time(NULL);
    req->status = 0;  // Pending
    
    nm_state->request_count++;
    pthread_mutex_unlock(&nm_state->request_mutex);
    
    snprintf(response.data, MAX_DATA_SIZE, 
             "Access request sent to owner '%s'. Request ID: %s", 
             file->owner, req->request_id);
    
    send_message(socket, &response);
    log_operation("REQUESTACCESS", msg->filename);
}

// Handle VIEWREQUESTS request
void handle_viewrequests(int socket, Message *msg) {
    printf("[View Requests] User '%s' viewing access requests\n", msg->username);
    
    Message response = {0};
    response.msg_type = MSG_RESPONSE;
    response.operation = OP_VIEWREQUESTS;
    response.error_code = ERR_SUCCESS;
    
    pthread_mutex_lock(&nm_state->request_mutex);
    
    char *buffer = response.data;
    int offset = 0;
    int found = 0;
    
    for (int i = 0; i < nm_state->request_count && offset < MAX_DATA_SIZE - 200; i++) {
        AccessRequest *req = &nm_state->access_requests[i];
        
        // Only show pending requests for files owned by this user
        if (req->status == 0 && strcmp(req->owner, msg->username) == 0) {
            const char *access_str = (req->access_type == ACCESS_READ) ? "READ" : "WRITE";
            offset += snprintf(buffer + offset, MAX_DATA_SIZE - offset - 1,
                             "ID: %s\n  File: %s\n  User: %s\n  Access: %s\n\n",
                             req->request_id, req->filename, req->requester, access_str);
            found = 1;
        }
    }
    
    pthread_mutex_unlock(&nm_state->request_mutex);
    
    if (!found) {
        strcpy(response.data, "No pending access requests");
    }
    
    send_message(socket, &response);
    log_operation("VIEWREQUESTS", "");
}

// Handle APPROVEREQUEST request
void handle_approverequest(int socket, Message *msg) {
    printf("[Approve Request] User '%s' approving request '%s'\n", 
           msg->username, msg->checkpoint_tag);  // request_id in checkpoint_tag
    
    Message response = {0};
    response.msg_type = MSG_RESPONSE;
    response.operation = OP_APPROVEREQUEST;
    response.error_code = ERR_SUCCESS;
    
    pthread_mutex_lock(&nm_state->request_mutex);
    
    // Find the request
    AccessRequest *req = NULL;
    for (int i = 0; i < nm_state->request_count; i++) {
        if (strcmp(nm_state->access_requests[i].request_id, msg->checkpoint_tag) == 0) {
            req = &nm_state->access_requests[i];
            break;
        }
    }
    
    if (!req) {
        pthread_mutex_unlock(&nm_state->request_mutex);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "Request not found");
        send_message(socket, &response);
        return;
    }
    
    // Verify ownership
    if (strcmp(req->owner, msg->username) != 0) {
        pthread_mutex_unlock(&nm_state->request_mutex);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_NOT_OWNER;
        strcpy(response.data, "You are not the owner of this file");
        send_message(socket, &response);
        return;
    }
    
    // Check if already processed
    if (req->status != 0) {
        pthread_mutex_unlock(&nm_state->request_mutex);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_INVALID_OPERATION;
        strcpy(response.data, "Request already processed");
        send_message(socket, &response);
        return;
    }
    
    // Mark as approved
    req->status = 1;
    
    // Save request details for granting access
    char filename[MAX_FILENAME];
    char requester[MAX_USERNAME];
    int access_type = req->access_type;
    strncpy(filename, req->filename, MAX_FILENAME - 1);
    strncpy(requester, req->requester, MAX_USERNAME - 1);
    
    pthread_mutex_unlock(&nm_state->request_mutex);
    
    // Now grant the access using existing ADDACCESS logic
    Message addaccess_msg = {0};
    addaccess_msg.msg_type = MSG_REQUEST;
    addaccess_msg.operation = OP_ADDACCESS;
    strncpy(addaccess_msg.username, msg->username, MAX_USERNAME - 1);
    strncpy(addaccess_msg.filename, filename, MAX_FILENAME - 1);
    strncpy(addaccess_msg.data, requester, MAX_USERNAME - 1);
    addaccess_msg.sentence_index = access_type;
    
    // Call existing addaccess handler (but don't send response twice)
    FileInfo *file = find_file(filename);
    if (file) {
        int primary_ss_id = file->primary_ss_id;
        int ss_index = find_storage_server(primary_ss_id);
        if (ss_index >= 0) {
            StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
            int ss_socket = connect_to_server(ss->ip, ss->nm_port);
            if (ss_socket >= 0) {
                Message ss_msg = addaccess_msg;
                ss_msg.operation = OP_SS_ADDACCESS;
                send_message(ss_socket, &ss_msg);
                
                Message ss_response = {0};
                receive_message(ss_socket, &ss_response);
                close_socket(ss_socket);
            }
        }
    }
    
    snprintf(response.data, MAX_DATA_SIZE, 
             "Request approved. Access granted to '%s' for file '%s'", 
             requester, filename);
    
    send_message(socket, &response);
    log_operation("APPROVEREQUEST", filename);
}

// Handle REJECTREQUEST request
void handle_rejectrequest(int socket, Message *msg) {
    printf("[Reject Request] User '%s' rejecting request '%s'\n", 
           msg->username, msg->checkpoint_tag);  // request_id in checkpoint_tag
    
    Message response = {0};
    response.msg_type = MSG_RESPONSE;
    response.operation = OP_REJECTREQUEST;
    response.error_code = ERR_SUCCESS;
    
    pthread_mutex_lock(&nm_state->request_mutex);
    
    // Find the request
    AccessRequest *req = NULL;
    for (int i = 0; i < nm_state->request_count; i++) {
        if (strcmp(nm_state->access_requests[i].request_id, msg->checkpoint_tag) == 0) {
            req = &nm_state->access_requests[i];
            break;
        }
    }
    
    if (!req) {
        pthread_mutex_unlock(&nm_state->request_mutex);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "Request not found");
        send_message(socket, &response);
        return;
    }
    
    // Verify ownership
    if (strcmp(req->owner, msg->username) != 0) {
        pthread_mutex_unlock(&nm_state->request_mutex);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_NOT_OWNER;
        strcpy(response.data, "You are not the owner of this file");
        send_message(socket, &response);
        return;
    }
    
    // Check if already processed
    if (req->status != 0) {
        pthread_mutex_unlock(&nm_state->request_mutex);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_INVALID_OPERATION;
        strcpy(response.data, "Request already processed");
        send_message(socket, &response);
        return;
    }
    
    // Mark as rejected
    req->status = 2;
    
    char requester[MAX_USERNAME];
    char filename[MAX_FILENAME];
    strncpy(requester, req->requester, MAX_USERNAME - 1);
    strncpy(filename, req->filename, MAX_FILENAME - 1);
    
    pthread_mutex_unlock(&nm_state->request_mutex);
    
    snprintf(response.data, MAX_DATA_SIZE, 
             "Request rejected. Access denied to '%s' for file '%s'", 
             requester, filename);
    
    send_message(socket, &response);
    log_operation("REJECTREQUEST", filename);
}
