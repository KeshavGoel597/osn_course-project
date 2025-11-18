#include "storage_server_all.h"
#include "../common/network_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

// Register storage server with Name Server
int register_with_nm(int nm_port, int client_port, const char *ss_ip) {
    // Connect to Name Server
    int nm_sockfd = connect_to_server(server_config.nm_ip, NM_PORT);
    if (nm_sockfd < 0) {
        fprintf(stderr, "Failed to connect to Name Server at %s:%d\n", server_config.nm_ip, NM_PORT);
        return -1;
    }
    
    printf("Connected to Name Server at %s:%d\n", server_config.nm_ip, NM_PORT);
    
    // Prepare registration message
    Message reg_msg;
    memset(&reg_msg, 0, sizeof(Message));
    
    reg_msg.msg_type = MSG_REQUEST;
    reg_msg.operation = OP_SS_REGISTER;
    strncpy(reg_msg.ip, ss_ip, MAX_IP_LEN - 1);
    reg_msg.port1 = nm_port;      // Port for NM connections
    reg_msg.port2 = client_port;  // Port for Client connections
    reg_msg.ss_id = server_config.ss_id;  // Include SS_ID for backup pairing
    
    // Get list of files and add to data field
    if (get_file_list_ll(reg_msg.data, MAX_DATA_SIZE) < 0) {
        fprintf(stderr, "Warning: Failed to get file list\n");
        strcpy(reg_msg.data, "");  // Empty file list
    }
    
    // Send registration message
    if (send_message(nm_sockfd, &reg_msg) < 0) {
        fprintf(stderr, "Failed to send registration message\n");
        close_socket(nm_sockfd);
        return -1;
    }
    
    printf("Sent registration message to Name Server\n");
    printf("File list: %s\n", reg_msg.data[0] ? reg_msg.data : "(empty)");
    
    // Wait for acknowledgment
    Message ack_msg;
    if (receive_message(nm_sockfd, &ack_msg) < 0) {
        fprintf(stderr, "Failed to receive acknowledgment from Name Server\n");
        close_socket(nm_sockfd);
        return -1;
    }
    
    if (ack_msg.msg_type == MSG_ACK && ack_msg.error_code == ERR_SUCCESS) {
        printf("Registration acknowledged by Name Server\n");
        
        // If this is a primary server, wait for backup info
        if (server_config.is_primary) {
            printf("Waiting for backup info from Name Server...\n");
            
            Message backup_info;
            if (receive_message(nm_sockfd, &backup_info) >= 0) {
                if (backup_info.operation == OP_NM_BACKUP_INFO) {
                    printf("Received backup info from NM\n");
                    printf("Backup server: %s:%d\n", backup_info.backup_ip, backup_info.backup_port);
                    
                    // Handle backup info (connect and bulk sync)
                    if (handle_nm_backup_info(backup_info.backup_ip, backup_info.backup_port) < 0) {
                        fprintf(stderr, "Warning: Failed to setup backup connection\n");
                    } else {
                        printf("Successfully configured backup replication\n");
                    }
                } else {
                    printf("No backup info available (backup server may not be online yet)\n");
                }
            } else {
                printf("No backup info received (backup server may not be online yet)\n");
            }
        }
        
        close_socket(nm_sockfd);
        return 0;
    } else {
        fprintf(stderr, "Registration failed with error code: %d\n", ack_msg.error_code);
        close_socket(nm_sockfd);
        return -1;
    }
}

// Handle requests from Name Server
void* handle_nm_connection(void *arg) {
    ThreadArg *thread_arg = (ThreadArg*)arg;
    int nm_sockfd = thread_arg->sockfd;
    char client_ip[MAX_IP_LEN];
    strncpy(client_ip, thread_arg->client_ip, MAX_IP_LEN - 1);
    free(thread_arg);
    
    printf("[NM Handler] Processing request from %s\n", client_ip);
    
    Message request;
    memset(&request, 0, sizeof(Message));
    if (receive_message(nm_sockfd, &request) < 0) {
        fprintf(stderr, "[NM Handler] Failed to receive message\n");
        close_socket(nm_sockfd);
        return NULL;
    }
    
    printf("[NM Handler] Received operation: %d (msg_type=%d, error_code=%d)\n", 
           request.operation, request.msg_type, request.error_code);
    printf("[NM Handler] Message details: username=%s, filename=%s, data=%s\n",
           request.username, request.filename, request.data);
    
    Message response;
    memset(&response, 0, sizeof(Message));
    response.msg_type = MSG_ACK;
    response.operation = request.operation;
    response.error_code = ERR_SUCCESS;
    
    // Handle different operations from Name Server
    switch (request.operation) {
        case OP_SS_CREATE_FILE: {
            printf("[NM Handler] CREATE FILE request: %s by %s\n", 
                   request.filename, request.username);
            
            int result = create_file_ll(request.filename, request.username);
            if (result < 0) {
                response.error_code = (result == ERR_FILE_EXISTS) ? ERR_FILE_EXISTS : ERR_SERVER_ERROR;
            } else {
                // CRITICAL FIX: Save empty undo backup so first write can be undone
                save_undo_backup_ll(request.filename);
                printf("[NM Handler] Created initial UNDO backup for '%s'\n", request.filename);
                
                // Replicate to backup server asynchronously (non-blocking)
                if (server_config.is_primary) {
                    enqueue_replication_task(REP_OP_CREATE, request.filename, request.username);
                    printf("[NM Handler] Enqueued async CREATE replication for '%s'\n", request.filename);
                }
            }
            break;
        }
        
        case OP_SS_DELETE_FILE: {
            printf("[NM Handler] DELETE FILE request: %s\n", request.filename);
            
            int result = delete_file_ll(request.filename);
            if (result < 0) {
                response.error_code = ERR_FILE_NOT_FOUND;
            } else {
                // Replicate to backup server asynchronously (non-blocking)
                if (server_config.is_primary) {
                    enqueue_replication_task(REP_OP_DELETE, request.filename, NULL);
                    printf("[NM Handler] Enqueued async DELETE replication for '%s'\n", request.filename);
                }
            }
            break;
        }
        
        case OP_INFO: {
            printf("[NM Handler] INFO request: %s\n", request.filename);
            
            FileMetadata metadata;
            if (get_file_metadata_ll(request.filename, &metadata) < 0) {
                response.error_code = ERR_FILE_NOT_FOUND;
            } else {
                // Format metadata into response data
                snprintf(response.data, MAX_DATA_SIZE,
                        "File: %s\nOwner: %s\nCreated: %s\nLast Modified: %s\n"
                        "Size: %ld bytes\nWords: %d\nCharacters: %d\n"
                        "Access: %s\nLast Accessed: %s",
                        metadata.filename, metadata.owner, metadata.created_time,
                        metadata.modified_time, metadata.file_size, metadata.word_count,
                        metadata.char_count, metadata.access_list, metadata.accessed_time);
                response.msg_type = MSG_RESPONSE;
            }
            break;
        }
        
        case OP_EXEC: {
            printf("[NM Handler] EXEC request: %s\n", request.filename);
            
            // Get full file path
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s/%s", server_config.storage_dir, request.filename);
            
            // Open file directly
            FILE *file = fopen(filepath, "r");
            if (!file) {
                response.error_code = ERR_FILE_NOT_FOUND;
                break;
            }
            
            // Get file size
            fseek(file, 0, SEEK_END);
            long file_size = ftell(file);
            fseek(file, 0, SEEK_SET);
            
            printf("[NM Handler] EXEC file size: %ld bytes\n", file_size);
            
            // Send initial response with file size
            response.msg_type = MSG_RESPONSE;
            response.error_code = ERR_SUCCESS;
            response.sentence_index = file_size;  // Send file size
            
            if (send_message(nm_sockfd, &response) < 0) {
                fprintf(stderr, "[NM Handler] Failed to send initial EXEC response\n");
                fclose(file);
                close_socket(nm_sockfd);
                return NULL;
            }
            
            // Send file content in chunks
            char buffer[MAX_DATA_SIZE - 100];
            size_t bytes_read;
            size_t bytes_sent = 0;
            int chunk_num = 0;
            int send_failed = 0;
            
            while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
                Message chunk_msg;
                memset(&chunk_msg, 0, sizeof(Message));
                chunk_msg.msg_type = MSG_RESPONSE;
                chunk_msg.operation = OP_EXEC_CHUNK;
                chunk_msg.error_code = ERR_SUCCESS;
                chunk_msg.sentence_index = bytes_read;  // Chunk size
                memcpy(chunk_msg.data, buffer, bytes_read);
                
                if (send_message(nm_sockfd, &chunk_msg) < 0) {
                    fprintf(stderr, "[NM Handler] Failed to send EXEC chunk %d\n", chunk_num);
                    send_failed = 1;
                    break;
                }
                
                bytes_sent += bytes_read;
                chunk_num++;
                printf("[NM Handler] Sent EXEC chunk %d: %zu bytes (total: %zu/%ld)\n", 
                       chunk_num, bytes_read, bytes_sent, file_size);
            }
            
            fclose(file);
            
            if (!send_failed) {
                // Send STOP message to indicate completion
                Message stop_msg;
                memset(&stop_msg, 0, sizeof(Message));
                stop_msg.msg_type = MSG_RESPONSE;
                stop_msg.operation = OP_STOP;
                stop_msg.error_code = ERR_SUCCESS;
                
                send_message(nm_sockfd, &stop_msg);
                printf("[NM Handler] EXEC transfer complete: %zu bytes in %d chunks\n", 
                       bytes_sent, chunk_num);
            }
            
            // Close connection and return (don't send standard response)
            close_socket(nm_sockfd);
            return NULL;
        }
        
        case OP_SS_ADDACCESS: {
            printf("[NM Handler] ADDACCESS request: %s for user %s (requested by %s)\n", 
                   request.filename, request.data, request.username);
            
            // Parse access type from data field - wait, access_type should be in sentence_index
            // and target username should be in data field
            int access_type = request.sentence_index;  // Access type passed in sentence_index
            
            // Extract target username from data field
            char target_user[MAX_USERNAME];
            strncpy(target_user, request.data, MAX_USERNAME - 1);
            target_user[MAX_USERNAME - 1] = '\0';
            
            int result = add_user_access_ll(request.filename, target_user, access_type);
            if (result < 0) {
                response.error_code = ERR_SERVER_ERROR;
                snprintf(response.data, MAX_DATA_SIZE, "Failed to add access for user");
            } else {
                response.error_code = ERR_SUCCESS;
                snprintf(response.data, MAX_DATA_SIZE, "Access granted successfully");
                
                // Replicate metadata to backup asynchronously
                if (server_config.is_primary) {
                    enqueue_replication_task(REP_OP_METADATA, "", NULL);
                    printf("[NM Handler] Enqueued async metadata replication\n");
                }
            }
            break;
        }
        
        case OP_SS_REMACCESS: {
            printf("[NM Handler] REMACCESS request: %s for user %s (requested by %s)\n", 
                   request.filename, request.data, request.username);
            
            // Extract target username from data field
            char target_user[MAX_USERNAME];
            strncpy(target_user, request.data, MAX_USERNAME - 1);
            target_user[MAX_USERNAME - 1] = '\0';
            
            int result = remove_user_access_ll(request.filename, target_user);
            if (result < 0) {
                response.error_code = ERR_SERVER_ERROR;
                snprintf(response.data, MAX_DATA_SIZE, "Failed to remove access for user");
            } else {
                response.error_code = ERR_SUCCESS;
                snprintf(response.data, MAX_DATA_SIZE, "Access removed successfully");
                
                // Replicate metadata to backup asynchronously
                if (server_config.is_primary) {
                    enqueue_replication_task(REP_OP_METADATA, "", NULL);
                    printf("[NM Handler] Enqueued async metadata replication\n");
                }
            }
            break;
        }
        
        case OP_NM_BACKUP_INFO: {
            printf("[NM Handler] Received backup info from NM\n");
            
            // Extract backup server details from the correct fields
            char backup_ip[MAX_IP_LEN];
            int backup_port;
            strncpy(backup_ip, request.backup_ip, MAX_IP_LEN - 1);
            backup_port = request.backup_port;
            
            printf("[NM Handler] Backup server: %s:%d\n", backup_ip, backup_port);
            
            // Handle backup info (connect and bulk sync)
            if (handle_nm_backup_info(backup_ip, backup_port) < 0) {
                fprintf(stderr, "[NM Handler] ERROR: Failed to handle backup info\n");
                response.error_code = ERR_CONNECTION_FAILED;
            } else {
                printf("[NM Handler] Successfully handled backup info\n");
            }
            break;
        }
        
        case OP_RECOVERY_SYNC: {
            // CRITICAL FIX: NM commanding this primary to sync from backup
            printf("[Recovery Sync] Received recovery sync command from NM\n");
            printf("[Recovery Sync] Backup server: %s:%d (SS%d)\n", 
                   request.backup_ip, request.backup_port, request.ss_id);
            
            // Mark this server as syncing
            server_config.is_acting_primary = 0;
            
            // Request full bulk sync from backup (which is currently acting primary)
            if (request_recovery_sync_from_backup(request.backup_ip, request.backup_port) < 0) {
                fprintf(stderr, "[Recovery Sync] ERROR: Failed to sync from backup\n");
                response.error_code = ERR_SERVER_ERROR;
                strcpy(response.data, "Recovery sync failed");
            } else {
                printf("[Recovery Sync] Successfully completed recovery sync from backup\n");
                response.error_code = ERR_SUCCESS;
                strcpy(response.data, "Recovery sync completed");
                
                // Notify Name Server that recovery is complete
                int nm_sockfd = connect_to_server(server_config.nm_ip, NM_PORT);
                if (nm_sockfd >= 0) {
                    Message notify;
                    memset(&notify, 0, sizeof(Message));
                    notify.msg_type = MSG_REQUEST;
                    notify.operation = OP_RECOVERY_SYNC;
                    notify.ss_id = server_config.ss_id;
                    strcpy(notify.data, "Recovery sync complete");
                    
                    send_message(nm_sockfd, &notify);
                    
                    Message nm_ack;
                    receive_message(nm_sockfd, &nm_ack);
                    close(nm_sockfd);
                    
                    printf("[Recovery Sync] Notified NM of recovery completion\n");
                }
            }
            break;
        }
        
        case OP_CREATEFOLDER: {
            printf("[NM Handler] CREATEFOLDER request: %s by %s\n", 
                   request.filename, request.username);
            
            // Create folder in the files directory
            char folder_path[MAX_PATH];
            snprintf(folder_path, MAX_PATH, "%s/files/%s", server_config.storage_dir, request.filename);
            
            // Create the folder directory
            if (mkdir(folder_path, 0755) < 0) {
                if (errno == EEXIST) {
                    response.error_code = ERR_FILE_EXISTS;
                    snprintf(response.data, MAX_DATA_SIZE, "Folder already exists");
                } else {
                    response.error_code = ERR_SERVER_ERROR;
                    snprintf(response.data, MAX_DATA_SIZE, "Failed to create folder: %s", strerror(errno));
                }
            } else {
                // Add folder to metadata for persistence
                FileMetadata folder_meta;
                memset(&folder_meta, 0, sizeof(FileMetadata));
                strncpy(folder_meta.filename, request.filename, MAX_FILENAME - 1);
                strncpy(folder_meta.owner, request.username, MAX_USERNAME - 1);
                get_timestamp(folder_meta.created_time, sizeof(folder_meta.created_time));
                get_timestamp(folder_meta.modified_time, sizeof(folder_meta.modified_time));
                folder_meta.file_size = 0;  // Folders have size 0
                folder_meta.word_count = -1;  // Use -1 to mark as folder (not a file)
                folder_meta.char_count = 0;
                folder_meta.access_list[0] = '\0';
                
                add_metadata_ll(&folder_meta);
                
                response.error_code = ERR_SUCCESS;
                snprintf(response.data, MAX_DATA_SIZE, "Folder created successfully");
            }
            break;
        }
        
        case OP_MOVE: {
            printf("[NM Handler] MOVE request: %s to %s by %s\n", 
                   request.filename, request.target_path, request.username);
            
            char src_path[MAX_PATH];
            char dst_path[MAX_PATH];
            char dst_dir[MAX_PATH];
            
            // Source file is in files/ directory
            snprintf(src_path, MAX_PATH, "%s/files/%s", server_config.storage_dir, request.filename);
            
            // Destination folder should also be in files/ directory
            snprintf(dst_dir, MAX_PATH, "%s/files/%s", server_config.storage_dir, request.target_path);
            snprintf(dst_path, MAX_PATH, "%s/%s", dst_dir, request.filename);
            
            // Check if source exists
            if (access(src_path, F_OK) != 0) {
                response.error_code = ERR_FILE_NOT_FOUND;
                snprintf(response.data, MAX_DATA_SIZE, "Source file not found");
            } else if (access(dst_dir, F_OK) != 0) {
                response.error_code = ERR_FILE_NOT_FOUND;
                snprintf(response.data, MAX_DATA_SIZE, "Destination folder does not exist");
            } else if (rename(src_path, dst_path) < 0) {
                response.error_code = ERR_SERVER_ERROR;
                snprintf(response.data, MAX_DATA_SIZE, "Failed to move file: %s", strerror(errno));
            } else {
                response.error_code = ERR_SUCCESS;
                snprintf(response.data, MAX_DATA_SIZE, "File moved successfully");
                
                // Note: The file has been physically moved in the filesystem
                // The Name Server needs to update its file mapping to reflect new path
                // Storage server doesn't maintain path mapping - that's NM's responsibility
            }
            break;
        }
        
        case OP_VIEWFOLDER: {
            printf("[NM Handler] VIEWFOLDER request: %s by %s\n", 
                   request.filename, request.username);
            
            char folder_path[MAX_PATH];
            snprintf(folder_path, MAX_PATH, "%s/files/%s", server_config.storage_dir, request.filename);
            
            DIR *dir = opendir(folder_path);
            if (!dir) {
                response.error_code = ERR_FILE_NOT_FOUND;
                snprintf(response.data, MAX_DATA_SIZE, "Folder not found");
            } else {
                struct dirent *entry;
                response.data[0] = '\0';
                int offset = 0;
                
                while ((entry = readdir(dir)) != NULL && offset < MAX_DATA_SIZE - 100) {
                    if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                        offset += snprintf(response.data + offset, MAX_DATA_SIZE - offset - 1,
                                         "%s\n", entry->d_name);
                    }
                }
                closedir(dir);
                response.error_code = ERR_SUCCESS;
            }
            break;
        }
        
        case OP_CHECKPOINT: {
            printf("[NM Handler] CHECKPOINT request: %s tag=%s by %s\n", 
                   request.filename, request.checkpoint_tag, request.username);
            
            char src_path[MAX_PATH];
            char checkpoint_path[MAX_PATH];
            snprintf(src_path, MAX_PATH, "%s/files/%s", server_config.storage_dir, request.filename);
            snprintf(checkpoint_path, MAX_PATH, "%s/checkpoints/%s.%s", 
                     server_config.storage_dir, request.filename, request.checkpoint_tag);
            
            // Create checkpoints directory if needed
            char checkpoint_dir[MAX_PATH];
            snprintf(checkpoint_dir, MAX_PATH, "%s/checkpoints", server_config.storage_dir);
            mkdir(checkpoint_dir, 0755);
            
            // Copy file to checkpoint
            FILE *src = fopen(src_path, "r");
            if (!src) {
                response.error_code = ERR_FILE_NOT_FOUND;
                snprintf(response.data, MAX_DATA_SIZE, "File not found");
            } else {
                FILE *dst = fopen(checkpoint_path, "w");
                if (!dst) {
                    fclose(src);
                    response.error_code = ERR_SERVER_ERROR;
                    snprintf(response.data, MAX_DATA_SIZE, "Failed to create checkpoint");
                } else {
                    char buffer[4096];
                    size_t bytes;
                    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
                        fwrite(buffer, 1, bytes, dst);
                    }
                    fclose(src);
                    fclose(dst);
                    response.error_code = ERR_SUCCESS;
                    snprintf(response.data, MAX_DATA_SIZE, "Checkpoint created successfully");
                }
            }
            break;
        }
        
        case OP_VIEWCHECKPOINT: {
            printf("[NM Handler] VIEWCHECKPOINT request: %s tag=%s by %s\n", 
                   request.filename, request.checkpoint_tag, request.username);
            
            char checkpoint_path[MAX_PATH];
            snprintf(checkpoint_path, MAX_PATH, "%s/checkpoints/%s.%s", 
                     server_config.storage_dir, request.filename, request.checkpoint_tag);
            
            FILE *fp = fopen(checkpoint_path, "r");
            if (!fp) {
                response.error_code = ERR_FILE_NOT_FOUND;
                snprintf(response.data, MAX_DATA_SIZE, "Checkpoint not found");
            } else {
                size_t bytes = fread(response.data, 1, MAX_DATA_SIZE - 1, fp);
                response.data[bytes] = '\0';
                fclose(fp);
                response.error_code = ERR_SUCCESS;
            }
            break;
        }
        
        case OP_REVERT: {
            printf("[NM Handler] REVERT request: %s to tag=%s by %s\n", 
                   request.filename, request.checkpoint_tag, request.username);
            
            char file_path[MAX_PATH];
            char checkpoint_path[MAX_PATH];
            snprintf(file_path, MAX_PATH, "%s/files/%s", server_config.storage_dir, request.filename);
            snprintf(checkpoint_path, MAX_PATH, "%s/checkpoints/%s.%s", 
                     server_config.storage_dir, request.filename, request.checkpoint_tag);
            
            // Check if checkpoint exists
            if (access(checkpoint_path, F_OK) != 0) {
                response.error_code = ERR_FILE_NOT_FOUND;
                snprintf(response.data, MAX_DATA_SIZE, "Checkpoint not found");
            } else {
                // Copy checkpoint to file
                FILE *src = fopen(checkpoint_path, "r");
                FILE *dst = fopen(file_path, "w");
                
                if (!src || !dst) {
                    if (src) fclose(src);
                    if (dst) fclose(dst);
                    response.error_code = ERR_SERVER_ERROR;
                    snprintf(response.data, MAX_DATA_SIZE, "Failed to revert file");
                } else {
                    char buffer[4096];
                    size_t bytes;
                    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
                        fwrite(buffer, 1, bytes, dst);
                    }
                    fclose(src);
                    fclose(dst);
                    response.error_code = ERR_SUCCESS;
                    snprintf(response.data, MAX_DATA_SIZE, "File reverted successfully");
                    
                    // Update metadata - use the simpler function that just updates modified time
                    update_file_modified_time_ll(request.filename);
                }
            }
            break;
        }
        
        case OP_LISTCHECKPOINTS: {
            printf("[NM Handler] LISTCHECKPOINTS request: %s by %s\n", 
                   request.filename, request.username);
            
            char checkpoint_dir[MAX_PATH];
            snprintf(checkpoint_dir, MAX_PATH, "%s/checkpoints", server_config.storage_dir);
            
            DIR *dir = opendir(checkpoint_dir);
            if (!dir) {
                response.error_code = ERR_SUCCESS;
                snprintf(response.data, MAX_DATA_SIZE, "No checkpoints found");
            } else {
                struct dirent *entry;
                response.data[0] = '\0';
                int offset = 0;
                int found = 0;
                
                // Look for checkpoints matching this filename
                char prefix[MAX_FILENAME + 2];
                snprintf(prefix, sizeof(prefix), "%s.", request.filename);
                
                while ((entry = readdir(dir)) != NULL && offset < MAX_DATA_SIZE - 100) {
                    if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0) {
                        // Extract tag (everything after filename.)
                        const char *tag = entry->d_name + strlen(prefix);
                        offset += snprintf(response.data + offset, MAX_DATA_SIZE - offset - 1,
                                         "%s\n", tag);
                        found = 1;
                    }
                }
                closedir(dir);
                
                if (!found) {
                    snprintf(response.data, MAX_DATA_SIZE, "No checkpoints found for this file");
                }
                response.error_code = ERR_SUCCESS;
            }
            break;
        }
        
        case OP_BACKUP_INIT_SYNC: {
            printf("[NM Handler] BACKUP_INIT_SYNC request from NM\n");
            
            // This is a request to perform bulk synchronization to backup server
            // The backup server ID is in ss_id field
            int target_ss_id = request.ss_id;
            
            if (server_config.is_primary && server_config.backup_sockfd > 0) {
                printf("[Recovery] Starting bulk sync to backup SS%d\n", target_ss_id);
                
                // Send all files to backup server
                if (perform_bulk_sync_to_backup() < 0) {
                    response.error_code = ERR_SERVER_ERROR;
                    strcpy(response.data, "Bulk sync failed");
                } else {
                    response.error_code = ERR_SUCCESS;
                    strcpy(response.data, "Bulk sync completed successfully");
                }
            } else {
                response.error_code = ERR_INVALID_OPERATION;
                strcpy(response.data, "Not a primary server or backup not connected");
            }
            break;
        }
        
        case OP_BACKUP_METADATA: {
            printf("[NM Handler] BACKUP_METADATA request from NM\n");
            
            // This is a request to sync metadata to backup server
            if (server_config.is_primary) {
                // Enqueue metadata replication
                enqueue_replication_task(REP_OP_METADATA, "", NULL);
                response.error_code = ERR_SUCCESS;
                strcpy(response.data, "Metadata sync initiated");
            } else {
                response.error_code = ERR_INVALID_OPERATION;
                strcpy(response.data, "Not a primary server");
            }
            break;
        }
        
        case OP_BACKUP_SYNC: {
            printf("[NM Handler] BACKUP_SYNC request for file: %s\n", request.filename);
            
            // This is a request to sync a specific file to backup server
            if (server_config.is_primary) {
                enqueue_replication_task(REP_OP_SYNC, request.filename, NULL);
                response.error_code = ERR_SUCCESS;
                strcpy(response.data, "File sync initiated");
            } else {
                response.error_code = ERR_INVALID_OPERATION;
                strcpy(response.data, "Not a primary server");
            }
            break;
        }
        
        default:
            printf("[NM Handler] Unknown operation: %d\n", request.operation);
            response.error_code = ERR_INVALID_OPERATION;
            break;
    }
    
    // Send response back to Name Server
    if (send_message(nm_sockfd, &response) < 0) {
        fprintf(stderr, "[NM Handler] Failed to send response\n");
    } else {
        printf("[NM Handler] Sent response with error code: %d\n", response.error_code);
    }
    
    close_socket(nm_sockfd);
    return NULL;
}

// Send acknowledgment to Name Server
int send_ack_to_nm(int nm_sockfd, int operation, int error_code) {
    Message ack;
    memset(&ack, 0, sizeof(Message));
    
    ack.msg_type = MSG_ACK;
    ack.operation = operation;
    ack.error_code = error_code;
    
    return send_message(nm_sockfd, &ack);
}
