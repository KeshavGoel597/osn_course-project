#include "name_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int register_storage_server(Message *msg) {
    pthread_mutex_lock(&nm_state->ss_list_mutex);
    
    // Check if SS already exists
    int existing_index = find_storage_server(msg->ss_id);
    
    if (existing_index >= 0) {
        // Server reconnecting - update info
        StorageServerInfo *ss = &nm_state->storage_servers[existing_index];
        
        pthread_mutex_lock(&ss->ss_mutex);
        strcpy(ss->ip, msg->ip);
        ss->nm_port = msg->port1;
        ss->client_port = msg->port2;
        ss->status = SS_STATUS_ONLINE;
        ss->last_heartbeat = time(NULL);
        pthread_mutex_unlock(&ss->ss_mutex);
        
        printf("[SS Registration] Storage Server %d reconnected\n", msg->ss_id);
        log_operation("SS_RECONNECT", msg->data);
    } else {
        // New server registration
        if (nm_state->ss_count >= MAX_STORAGE_SERVERS) {
            pthread_mutex_unlock(&nm_state->ss_list_mutex);
            printf("[SS Registration] Error: Maximum storage servers reached\n");
            return ERR_SERVER_ERROR;
        }
        
        StorageServerInfo *ss = &nm_state->storage_servers[nm_state->ss_count];
        
        pthread_mutex_lock(&ss->ss_mutex);
        ss->ss_id = msg->ss_id;
        strcpy(ss->ip, msg->ip);
        ss->nm_port = msg->port1;
        ss->client_port = msg->port2;
        ss->status = SS_STATUS_ONLINE;
        ss->last_heartbeat = time(NULL);
        ss->file_count = 0;
        ss->backup_ss_id = 0;
        ss->is_primary = (msg->ss_id % 2 == 1) ? 1 : 0;  // Odd = primary, Even = backup
        pthread_mutex_unlock(&ss->ss_mutex);
        
        nm_state->ss_count++;
        
        printf("[SS Registration] Storage Server %d registered as %s\n", 
               msg->ss_id, ss->is_primary ? "PRIMARY" : "BACKUP");
        
        // Parse file list from data field
        if (strlen(msg->data) > 0) {
            char *file_list = strdup(msg->data);
            char *filename = strtok(file_list, ",");
            
            while (filename != NULL && ss->file_count < MAX_FILES_PER_SERVER) {
                strcpy(ss->files[ss->file_count].filename, filename);
                strcpy(ss->files[ss->file_count].owner, "system");  // Default owner
                ss->files[ss->file_count].primary_ss_id = msg->ss_id;
                ss->files[ss->file_count].backup_ss_id = 0;  // Will be set during pairing
                ss->files[ss->file_count].created_time = time(NULL);
                ss->files[ss->file_count].modified_time = time(NULL);
                ss->file_count++;
                
                printf("[SS Registration] Registered file: %s\n", filename);
                filename = strtok(NULL, ",");
            }
            
            free(file_list);
        }
        
        log_operation("SS_REGISTER", msg->data);
    }
    
    // Setup backup pairing if this is a primary server
    StorageServerInfo *ss = &nm_state->storage_servers[existing_index >= 0 ? existing_index : nm_state->ss_count - 1];
    if (ss->is_primary) {
        setup_backup_pairing(msg->ss_id);
    }
    
    pthread_mutex_unlock(&nm_state->ss_list_mutex);
    
    print_server_status();
    return ERR_SUCCESS;
}

void handle_storage_server_registration(int socket, Message *msg) {
    printf("[SS Handler] Processing Storage Server registration\n");
    
    int result = register_storage_server(msg);
    
    Message response = {0};
    response.msg_type = MSG_ACK;
    response.operation = OP_SS_REGISTER;
    response.error_code = result;
    
    if (result == ERR_SUCCESS) {
        strcpy(response.data, "Registration successful");
    } else {
        strcpy(response.data, "Registration failed");
    }
    
    send_message(socket, &response);
    
    // If this is a primary server and has a backup, send backup info
    if (result == ERR_SUCCESS && (msg->ss_id % 2 == 1)) {  // Primary server
        int backup_ss_id = msg->ss_id + 1;
        int backup_index = find_storage_server(backup_ss_id);
        
        if (backup_index >= 0) {
            StorageServerInfo *backup_ss = &nm_state->storage_servers[backup_index];
            
            if (backup_ss->status == SS_STATUS_ONLINE) {
                // Send backup info to primary
                Message backup_info = {0};
                backup_info.msg_type = MSG_REQUEST;
                backup_info.operation = OP_NM_BACKUP_INFO;
                backup_info.ss_id = backup_ss_id;
                strcpy(backup_info.backup_ip, backup_ss->ip);
                backup_info.backup_port = backup_ss->client_port;  // Use client port for replication
                
                send_message(socket, &backup_info);
                
                printf("[Backup Pairing] Sent backup info to SS%d: backup is SS%d at %s:%d\n",
                       msg->ss_id, backup_ss_id, backup_ss->ip, backup_ss->client_port);
            }
        }
    }
}

int find_storage_server(int ss_id) {
    for (int i = 0; i < nm_state->ss_count; i++) {
        if (nm_state->storage_servers[i].ss_id == ss_id) {
            return i;
        }
    }
    return -1;  // Not found
}

void setup_backup_pairing(int primary_ss_id) {
    int backup_ss_id = primary_ss_id + 1;  // Backup is always primary + 1
    
    int primary_index = find_storage_server(primary_ss_id);
    int backup_index = find_storage_server(backup_ss_id);
    
    if (primary_index >= 0 && backup_index >= 0) {
        StorageServerInfo *primary_ss = &nm_state->storage_servers[primary_index];
        StorageServerInfo *backup_ss = &nm_state->storage_servers[backup_index];
        
        pthread_mutex_lock(&primary_ss->ss_mutex);
        pthread_mutex_lock(&backup_ss->ss_mutex);
        
        primary_ss->backup_ss_id = backup_ss_id;
        backup_ss->backup_ss_id = primary_ss_id;  // Backup points to primary
        
        // Update file info to include backup
        for (int i = 0; i < primary_ss->file_count; i++) {
            primary_ss->files[i].backup_ss_id = backup_ss_id;
        }
        
        pthread_mutex_unlock(&backup_ss->ss_mutex);
        pthread_mutex_unlock(&primary_ss->ss_mutex);
        
        printf("[Backup Pairing] SS%d (primary) paired with SS%d (backup)\n", 
               primary_ss_id, backup_ss_id);
    }
}

int get_available_primary_server() {
    pthread_mutex_lock(&nm_state->assignment_mutex);
    
    // Round-robin assignment among primary servers
    int start_ss = nm_state->next_primary_ss;
    int selected_ss = -1;
    
    for (int attempts = 0; attempts < MAX_STORAGE_SERVERS; attempts++) {
        int current_ss = start_ss + (attempts * 2);  // Only check odd numbers (primaries)
        
        if (current_ss > MAX_STORAGE_SERVERS) {
            current_ss = 1 + (attempts % 2) * 2;  // Wrap around to 1, 3, 5...
        }
        
        int ss_index = find_storage_server(current_ss);
        if (ss_index >= 0) {
            StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
            
            pthread_mutex_lock(&ss->ss_mutex);
            if (ss->status == SS_STATUS_ONLINE && ss->is_primary) {
                selected_ss = current_ss;
                nm_state->next_primary_ss = current_ss + 2;  // Next primary
                if (nm_state->next_primary_ss > MAX_STORAGE_SERVERS) {
                    nm_state->next_primary_ss = 1;
                }
                pthread_mutex_unlock(&ss->ss_mutex);
                break;
            }
            pthread_mutex_unlock(&ss->ss_mutex);
        }
    }
    
    pthread_mutex_unlock(&nm_state->assignment_mutex);
    
    if (selected_ss > 0) {
        printf("[Load Balancing] Assigned primary server: SS%d\n", selected_ss);
    } else {
        printf("[Load Balancing] Error: No available primary servers\n");
    }
    
    return selected_ss;
}

int add_file_to_server(int ss_id, const char *filename, const char *owner) {
    int ss_index = find_storage_server(ss_id);
    if (ss_index < 0) {
        return ERR_SERVER_ERROR;
    }
    
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    
    pthread_mutex_lock(&ss->ss_mutex);
    
    if (ss->file_count >= MAX_FILES_PER_SERVER) {
        pthread_mutex_unlock(&ss->ss_mutex);
        return ERR_SERVER_ERROR;
    }
    
    // Check if file already exists
    for (int i = 0; i < ss->file_count; i++) {
        if (strcmp(ss->files[i].filename, filename) == 0) {
            pthread_mutex_unlock(&ss->ss_mutex);
            return ERR_FILE_EXISTS;
        }
    }
    
    // Add new file
    FileInfo *file = &ss->files[ss->file_count];
    strcpy(file->filename, filename);
    strcpy(file->owner, owner);
    file->primary_ss_id = ss_id;
    file->backup_ss_id = ss->backup_ss_id;
    file->created_time = time(NULL);
    file->modified_time = time(NULL);
    
    ss->file_count++;
    
    pthread_mutex_unlock(&ss->ss_mutex);
    
    printf("[File Management] Added file '%s' to SS%d (owner: %s)\n", filename, ss_id, owner);
    return ERR_SUCCESS;
}

int remove_file_from_server(int ss_id, const char *filename) {
    int ss_index = find_storage_server(ss_id);
    if (ss_index < 0) {
        return ERR_SERVER_ERROR;
    }
    
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    
    pthread_mutex_lock(&ss->ss_mutex);
    
    // Find and remove file
    for (int i = 0; i < ss->file_count; i++) {
        if (strcmp(ss->files[i].filename, filename) == 0) {
            // Shift remaining files
            for (int j = i; j < ss->file_count - 1; j++) {
                ss->files[j] = ss->files[j + 1];
            }
            ss->file_count--;
            
            pthread_mutex_unlock(&ss->ss_mutex);
            printf("[File Management] Removed file '%s' from SS%d\n", filename, ss_id);
            return ERR_SUCCESS;
        }
    }
    
    pthread_mutex_unlock(&ss->ss_mutex);
    return ERR_FILE_NOT_FOUND;
}

FileInfo* find_file(const char *filename) {
    pthread_mutex_lock(&nm_state->ss_list_mutex);
    
    for (int i = 0; i < nm_state->ss_count; i++) {
        StorageServerInfo *ss = &nm_state->storage_servers[i];
        
        pthread_mutex_lock(&ss->ss_mutex);
        for (int j = 0; j < ss->file_count; j++) {
            if (strcmp(ss->files[j].filename, filename) == 0) {
                FileInfo *file = &ss->files[j];
                pthread_mutex_unlock(&ss->ss_mutex);
                pthread_mutex_unlock(&nm_state->ss_list_mutex);
                return file;
            }
        }
        pthread_mutex_unlock(&ss->ss_mutex);
    }
    
    pthread_mutex_unlock(&nm_state->ss_list_mutex);
    return NULL;
}

int find_primary_for_file(const char *filename) {
    FileInfo *file = find_file(filename);
    if (file) {
        return file->primary_ss_id;
    }
    return -1;
}
