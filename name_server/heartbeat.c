#include "name_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

void* heartbeat_monitor(void* arg) {
    (void)arg;  // Suppress unused parameter warning
    printf("[Heartbeat Monitor] Started\n");
    
    // CRITICAL FIX: Track consecutive failures per server
    int consecutive_failures[MAX_STORAGE_SERVERS] = {0};
    const int FAILURE_THRESHOLD = 3;  // Require 3 consecutive failures before marking offline
    
    while (nm_state->running) {
        sleep(HEARTBEAT_INTERVAL);
        
        pthread_mutex_lock(&nm_state->ss_list_mutex);
        
        time_t current_time = time(NULL);
        
        for (int i = 0; i < nm_state->ss_count; i++) {
            StorageServerInfo *ss = &nm_state->storage_servers[i];
            
            pthread_mutex_lock(&ss->ss_mutex);
            
            // CRITICAL FIX: Don't heartbeat servers that are recovering
            if (ss->status == SS_STATUS_RECOVERING) {
                pthread_mutex_unlock(&ss->ss_mutex);
                continue;  // Skip heartbeat checks for recovering servers
            }
            
            if (ss->status == SS_STATUS_ONLINE || ss->status == SS_STATUS_ACTING_PRIMARY) {
                // Check if heartbeat is overdue
                time_t time_since_heartbeat = current_time - ss->last_heartbeat;
                
                if (time_since_heartbeat > HEARTBEAT_TIMEOUT) {
                    consecutive_failures[i]++;
                    printf("[Heartbeat Monitor] SS%d heartbeat timeout (%ld seconds) - failure %d/%d\n", 
                           ss->ss_id, time_since_heartbeat, consecutive_failures[i], FAILURE_THRESHOLD);
                    
                    // Only mark offline after multiple consecutive failures
                    if (consecutive_failures[i] >= FAILURE_THRESHOLD) {
                        // Mark server as offline
                        int old_status = ss->status;
                        ss->status = SS_STATUS_OFFLINE;
                        
                        printf("[Failover] SS%d marked as OFFLINE after %d consecutive failures\n", 
                               ss->ss_id, consecutive_failures[i]);
                        
                        // If this was a primary server, handle failover
                        if (old_status == SS_STATUS_ONLINE && ss->is_primary) {
                            handle_storage_server_failure(ss->ss_id);
                        }
                        
                        pthread_mutex_unlock(&ss->ss_mutex);
                    } else {
                        // Still within grace period - send heartbeat anyway
                        pthread_mutex_unlock(&ss->ss_mutex);
                        send_heartbeat_to_ss(ss->ss_id);
                    }
                } else {
                    // Heartbeat is fresh - reset failure counter
                    if (consecutive_failures[i] > 0) {
                        printf("[Heartbeat Monitor] SS%d recovered (failures reset)\n", ss->ss_id);
                        consecutive_failures[i] = 0;
                    }
                    
                    // Send heartbeat ping
                    pthread_mutex_unlock(&ss->ss_mutex);
                    send_heartbeat_to_ss(ss->ss_id);
                }
            } else {
                pthread_mutex_unlock(&ss->ss_mutex);
            }
        }
        
        pthread_mutex_unlock(&nm_state->ss_list_mutex);
    }
    
    printf("[Heartbeat Monitor] Stopped\n");
    return NULL;
}

void send_heartbeat_to_ss(int ss_id) {
    int ss_index = find_storage_server(ss_id);
    if (ss_index < 0) return;
    
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    
    pthread_mutex_lock(&ss->ss_mutex);
    
    if (ss->status == SS_STATUS_OFFLINE) {
        pthread_mutex_unlock(&ss->ss_mutex);
        return;
    }
    
    int ss_socket = connect_to_server(ss->ip, ss->nm_port);
    if (ss_socket < 0) {
        printf("[Heartbeat] Failed to connect to SS%d for heartbeat\n", ss_id);
        
        // Connection failed - mark as offline after timeout
        time_t current_time = time(NULL);
        if (current_time - ss->last_heartbeat > HEARTBEAT_TIMEOUT) {
            int old_status = ss->status;
            ss->status = SS_STATUS_OFFLINE;
            printf("[Heartbeat] SS%d marked as OFFLINE due to connection failure\n", ss_id);
            
            if (old_status == SS_STATUS_ONLINE && ss->is_primary) {
                pthread_mutex_unlock(&ss->ss_mutex);
                handle_storage_server_failure(ss_id);
                return;
            }
        }
        
        pthread_mutex_unlock(&ss->ss_mutex);
        return;
    }
    
    // Send heartbeat message
    Message heartbeat_msg = {0};
    heartbeat_msg.msg_type = MSG_REQUEST;
    heartbeat_msg.operation = OP_SS_REGISTER;  // Reuse registration for heartbeat
    heartbeat_msg.ss_id = ss_id;
    strcpy(heartbeat_msg.data, "HEARTBEAT");
    
    if (send_message(ss_socket, &heartbeat_msg) >= 0) {
        Message response = {0};
        if (receive_message(ss_socket, &response) >= 0) {
            // Update last heartbeat time
            ss->last_heartbeat = time(NULL);
            
            // Ensure server is marked as online if it responded
            if (ss->status == SS_STATUS_OFFLINE) {
                ss->status = ss->is_primary ? SS_STATUS_ONLINE : SS_STATUS_ONLINE;
                printf("[Heartbeat] SS%d back online\n", ss_id);
                
                // Handle reconnection with enhanced fault tolerance
                pthread_mutex_unlock(&ss->ss_mutex);
                handle_ss_reconnection(ss_id);
                return;  // Exit early since we unlocked the mutex
            }
        }
    }
    
    pthread_mutex_unlock(&ss->ss_mutex);
    close(ss_socket);
}

int is_storage_server_alive(int ss_id) {
    int ss_index = find_storage_server(ss_id);
    if (ss_index < 0) return 0;
    
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    
    pthread_mutex_lock(&ss->ss_mutex);
    int alive = (ss->status == SS_STATUS_ONLINE || ss->status == SS_STATUS_ACTING_PRIMARY);
    pthread_mutex_unlock(&ss->ss_mutex);
    
    return alive;
}

void handle_storage_server_failure(int failed_ss_id) {
    printf("[Failover] Handling failure of primary server SS%d\n", failed_ss_id);
    
    int failed_ss_index = find_storage_server(failed_ss_id);
    if (failed_ss_index < 0) return;
    
    StorageServerInfo *failed_ss = &nm_state->storage_servers[failed_ss_index];
    
    pthread_mutex_lock(&failed_ss->ss_mutex);
    int backup_ss_id = failed_ss->backup_ss_id;
    pthread_mutex_unlock(&failed_ss->ss_mutex);
    
    // CRITICAL FIX: Clear cache when server fails to prevent stale lookups
    cache_clear();
    printf("[Failover] Cache cleared due to server failure\n");
    
    if (backup_ss_id > 0) {
        int backup_ss_index = find_storage_server(backup_ss_id);
        if (backup_ss_index >= 0) {
            StorageServerInfo *backup_ss = &nm_state->storage_servers[backup_ss_index];
            
            pthread_mutex_lock(&backup_ss->ss_mutex);
            if (backup_ss->status == SS_STATUS_ONLINE) {
                // Promote backup to acting primary
                backup_ss->status = SS_STATUS_ACTING_PRIMARY;
                
                printf("[Failover] SS%d promoted to acting primary for failed SS%d\n", 
                       backup_ss_id, failed_ss_id);
                
                // Update file records to point to backup as primary
                promote_backup_to_primary(backup_ss_id, failed_ss_id);
                
                log_operation("FAILOVER", "Primary SS failed, backup promoted");
                
            } else {
                printf("[Failover] Warning: Backup SS%d is also offline\n", backup_ss_id);
            }
            pthread_mutex_unlock(&backup_ss->ss_mutex);
        }
    } else {
        printf("[Failover] Warning: No backup server for failed SS%d\n", failed_ss_id);
    }
}

void promote_backup_to_primary(int backup_ss_id, int failed_primary_ss_id) {
    pthread_mutex_lock(&nm_state->ss_list_mutex);
    
    // Find backup server and copy files from failed primary
    int backup_ss_index = find_storage_server(backup_ss_id);
    int failed_ss_index = find_storage_server(failed_primary_ss_id);
    
    if (backup_ss_index >= 0 && failed_ss_index >= 0) {
        StorageServerInfo *backup_ss = &nm_state->storage_servers[backup_ss_index];
        StorageServerInfo *failed_ss = &nm_state->storage_servers[failed_ss_index];
        
        pthread_mutex_lock(&backup_ss->ss_mutex);
        pthread_mutex_lock(&failed_ss->ss_mutex);
        
        // Copy file records from failed primary to backup
        for (int i = 0; i < failed_ss->file_count; i++) {
            FileInfo *file = &failed_ss->files[i];
            
            // Update file to point to backup as new primary
            file->primary_ss_id = backup_ss_id;
            file->backup_ss_id = 0;  // No backup for now
            
            // Add to backup server's file list if not already there
            int found = 0;
            for (int j = 0; j < backup_ss->file_count; j++) {
                if (strcmp(backup_ss->files[j].filename, file->filename) == 0) {
                    // Update existing entry
                    backup_ss->files[j] = *file;
                    found = 1;
                    break;
                }
            }
            
            if (!found && backup_ss->file_count < MAX_FILES_PER_SERVER) {
                // Add new entry
                backup_ss->files[backup_ss->file_count] = *file;
                backup_ss->file_count++;
            }
            
            printf("[Failover] File '%s' ownership transferred to SS%d\n", 
                   file->filename, backup_ss_id);
        }
        
        pthread_mutex_unlock(&failed_ss->ss_mutex);
        pthread_mutex_unlock(&backup_ss->ss_mutex);
    }
    
    pthread_mutex_unlock(&nm_state->ss_list_mutex);
}

void log_operation(const char *operation, const char *details) {
    time_t now = time(NULL);
    char *time_str = ctime(&now);
    time_str[strlen(time_str) - 1] = '\0';  // Remove newline
    
    printf("[LOG] %s | %s | %s\n", time_str, operation, details);
    
    // Optionally write to log file
    FILE *log_file = fopen("name_server.log", "a");
    if (log_file) {
        fprintf(log_file, "%s | %s | %s\n", time_str, operation, details);
        fclose(log_file);
    }
}

void print_server_status() {
    printf("\n=== Name Server Status ===\n");
    printf("Running: %s\n", nm_state->running ? "YES" : "NO");
    printf("Storage Servers: %d\n", nm_state->ss_count);
    printf("Active Clients: %d\n", nm_state->client_count);
    
    pthread_mutex_lock(&nm_state->ss_list_mutex);
    
    printf("\nStorage Servers:\n");
    for (int i = 0; i < nm_state->ss_count; i++) {
        StorageServerInfo *ss = &nm_state->storage_servers[i];
        
        pthread_mutex_lock(&ss->ss_mutex);
        
        const char *status_str;
        switch (ss->status) {
            case SS_STATUS_ONLINE: status_str = "ONLINE"; break;
            case SS_STATUS_OFFLINE: status_str = "OFFLINE"; break;
            case SS_STATUS_ACTING_PRIMARY: status_str = "ACTING_PRIMARY"; break;
            case SS_STATUS_RECOVERING: status_str = "RECOVERING"; break;
            default: status_str = "UNKNOWN"; break;
        }
        
        printf("  SS%d: %s | %s:%d | %s | Files: %d | Backup: SS%d\n", 
               ss->ss_id, status_str, ss->ip, ss->client_port,
               ss->is_primary ? "PRIMARY" : "BACKUP", 
               ss->file_count, ss->backup_ss_id);
        
        pthread_mutex_unlock(&ss->ss_mutex);
    }
    
    pthread_mutex_unlock(&nm_state->ss_list_mutex);
    
    printf("\nActive Clients:\n");
    pthread_mutex_lock(&nm_state->client_list_mutex);
    for (int i = 0; i < nm_state->client_count; i++) {
        ClientInfo *client = &nm_state->clients[i];
        if (client->is_connected) {
            printf("  %s: %s:%d\n", client->username, client->ip, client->port);
        }
    }
    pthread_mutex_unlock(&nm_state->client_list_mutex);
    
    printf("========================\n\n");
}
