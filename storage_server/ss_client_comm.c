#include "storage_server_all.h"
#include "../common/network_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

// Handle client connection (runs in separate thread)
void* handle_client_connection(void *arg) {
    ThreadArg *thread_arg = (ThreadArg*)arg;
    int client_sockfd = thread_arg->sockfd;
    char client_ip[MAX_IP_LEN];
    strncpy(client_ip, thread_arg->client_ip, MAX_IP_LEN - 1);
    free(thread_arg);
    
    printf("[Client Handler] Processing request from %s\n", client_ip);
    
    Message request;
    if (receive_message(client_sockfd, &request) < 0) {
        fprintf(stderr, "[Client Handler] Failed to receive message\n");
        close_socket(client_sockfd);
        return NULL;
    }
    
    printf("[Client Handler] Received operation: %d from user: %s\n", 
           request.operation, request.username);
    
    // Handle different client operations
    switch (request.operation) {
        case OP_READ:
            handle_read_request(client_sockfd, &request);
            break;
            
        case OP_WRITE:
            handle_write_request(client_sockfd, &request);
            break;
            
        case OP_STREAM:
            handle_stream_request(client_sockfd, &request);
            break;
            
        // Handle backup operations from primary server
        case OP_BACKUP_CREATE:
        case OP_BACKUP_DELETE:
        case OP_BACKUP_SYNC:
        case OP_BACKUP_INIT_SYNC:
        case OP_BACKUP_METADATA:
        case OP_BACKUP_FILE:
        case OP_BACKUP_UNDO_FILE:
        case OP_BACKUP_SYNC_COMPLETE:
            if (!server_config.is_primary || server_config.is_acting_primary) {
                printf("[Client Handler] Received backup operation from primary\n");
                handle_backup_request(client_sockfd);
            } else {
                fprintf(stderr, "[Client Handler] Primary server received backup operation (invalid)\n");
            }
            break;
            
        case OP_UNDO: {
            printf("[UNDO] User '%s' undoing file: %s\n", request.username, request.filename);
            
            Message response;
            memset(&response, 0, sizeof(Message));
            response.msg_type = MSG_ACK;
            response.operation = OP_UNDO;
            
            int result = undo_file_change_ll(request.filename);
            if (result < 0) {
                response.msg_type = MSG_ERROR;
                response.error_code = ERR_SERVER_ERROR;
            } else {
                response.error_code = ERR_SUCCESS;
                
                // CRITICAL FIX: Replicate restored file to backup server
                // UNDO changes the file content, so backup must be synchronized
                if (server_config.is_primary || server_config.is_acting_primary) {
                    enqueue_replication_task(REP_OP_SYNC, request.filename, NULL);
                    printf("[UNDO] Enqueued async replication for '%s'\n", request.filename);
                }
            }
            
            send_message(client_sockfd, &response);
            break;
        }
            
        default:
            printf("[Client Handler] Unknown operation: %d\n", request.operation);
            Message error_msg;
            memset(&error_msg, 0, sizeof(Message));
            error_msg.msg_type = MSG_ERROR;
            error_msg.error_code = ERR_INVALID_OPERATION;
            send_message(client_sockfd, &error_msg);
            break;
    }
    
    close_socket(client_sockfd);
    printf("[Client Handler] Connection closed\n");
    return NULL;
}

// Handle READ operation from client
int handle_read_request(int client_sockfd, Message *msg) {
    printf("[READ] User '%s' reading file: %s\n", msg->username, msg->filename);
    
    Message response;
    memset(&response, 0, sizeof(Message));
    response.msg_type = MSG_RESPONSE;
    response.operation = OP_READ;
    
    // Check read access
    if (!has_read_access_ll(msg->filename, msg->username)) {
        printf("[READ] Access denied for user '%s'\n", msg->username);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_NO_READ_ACCESS;
        send_message(client_sockfd, &response);
        return -1;
    }
    
    // CRITICAL FIX: Implement chunked transfer for large files
    // Get file path and size
    char filepath[MAX_PATH];
    get_file_path(msg->filename, filepath, MAX_PATH);
    
    FILE *file = fopen(filepath, "r");
    if (file == NULL) {
        printf("[READ] File not found: %s\n", msg->filename);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        send_message(client_sockfd, &response);
        return -1;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    printf("[READ] File size: %ld bytes\n", file_size);
    
    // Send initial response with file size
    response.error_code = ERR_SUCCESS;
    response.sentence_index = file_size;  // Use sentence_index to send file size
    snprintf(response.data, MAX_DATA_SIZE, "FILE_SIZE:%ld", file_size);
    
    if (send_message(client_sockfd, &response) < 0) {
        fclose(file);
        return -1;
    }
    
    // Send file content in chunks
    char buffer[MAX_DATA_SIZE - 100];  // Leave room for metadata
    size_t bytes_sent = 0;
    size_t chunk_num = 0;
    
    while (bytes_sent < (size_t)file_size) {
        size_t to_read = (file_size - bytes_sent > sizeof(buffer)) ? 
                         sizeof(buffer) : file_size - bytes_sent;
        
        size_t bytes_read = fread(buffer, 1, to_read, file);
        if (bytes_read == 0) break;
        
        Message chunk_msg;
        memset(&chunk_msg, 0, sizeof(Message));
        chunk_msg.msg_type = MSG_RESPONSE;
        chunk_msg.operation = OP_READ_CHUNK;
        chunk_msg.error_code = ERR_SUCCESS;
        chunk_msg.sentence_index = bytes_read;  // Chunk size
        memcpy(chunk_msg.data, buffer, bytes_read);
        
        if (send_message(client_sockfd, &chunk_msg) < 0) {
            fprintf(stderr, "[READ] Failed to send chunk %zu\n", chunk_num);
            fclose(file);
            return -1;
        }
        
        bytes_sent += bytes_read;
        chunk_num++;
        printf("[READ] Sent chunk %zu: %zu bytes (%zu/%ld total)\n", 
               chunk_num, bytes_read, bytes_sent, file_size);
    }
    
    fclose(file);
    
    // Send STOP message to indicate end of transfer
    Message stop_msg;
    memset(&stop_msg, 0, sizeof(Message));
    stop_msg.msg_type = MSG_RESPONSE;
    stop_msg.operation = OP_STOP;
    stop_msg.error_code = ERR_SUCCESS;
    strcpy(stop_msg.data, "READ_COMPLETE");
    send_message(client_sockfd, &stop_msg);
    
    // Update last accessed time
    update_file_accessed_time_ll(msg->filename, msg->username);
    
    printf("[READ] Successfully sent %zu bytes in %zu chunks\n", bytes_sent, chunk_num);
    return 0;
}

// Handle WRITE operation from client
int handle_write_request(int client_sockfd, Message *msg) {
    printf("[WRITE] User '%s' writing to file: %s, sentence: %d\n", 
           msg->username, msg->filename, msg->sentence_index);
    
    Message response;
    memset(&response, 0, sizeof(Message));
    response.msg_type = MSG_ACK;
    response.operation = OP_WRITE;
    
    // Check write access
    if (!has_write_access_ll(msg->filename, msg->username)) {
        printf("[WRITE] Access denied for user '%s'\n", msg->username);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_NO_WRITE_ACCESS;
        send_message(client_sockfd, &response);
        return -1;
    }
    
    // CRITICAL: Check file size to prevent memory exhaustion
    // WRITE uses in-memory linked lists, so we must limit file size
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", server_config.storage_dir, msg->filename);
    
    struct stat file_stat;
    if (stat(filepath, &file_stat) == 0) {
        // File exists - check size (10MB limit for WRITE operations)
        #define MAX_WRITE_FILE_SIZE (10 * 1024 * 1024)  // 10MB
        
        if (file_stat.st_size > MAX_WRITE_FILE_SIZE) {
            printf("[WRITE] File too large: %ld bytes (limit: %d bytes)\n", 
                   file_stat.st_size, MAX_WRITE_FILE_SIZE);
            response.msg_type = MSG_ERROR;
            response.error_code = ERR_SERVER_ERROR;
            snprintf(response.data, MAX_DATA_SIZE, 
                     "File too large for WRITE operation (limit: 10MB)");
            send_message(client_sockfd, &response);
            return -1;
        }
    }
    
    // Try to lock the sentence
    int lock_result = lock_sentence_ll(msg->filename, msg->sentence_index, msg->username);
    if (lock_result != 0) {
        printf("[WRITE] Failed to lock sentence: error code %d\n", lock_result);
        response.msg_type = MSG_ERROR;
        response.error_code = lock_result;  // Return the actual error code
        send_message(client_sockfd, &response);
        return -1;
    }
    
    // Send acknowledgment that lock is acquired
    response.error_code = ERR_SUCCESS;
    strcpy(response.data, "LOCKED");
    send_message(client_sockfd, &response);
    printf("[WRITE] Sentence locked, waiting for write commands\n");
    
    // CRITICAL FIX: Ensure file is cached before creating undo backup
    // get_file_from_cache() loads file from disk if not in memory
    if (get_file_from_cache(msg->filename) == NULL) {
        fprintf(stderr, "[WRITE] Failed to load file into cache for undo backup\n");
        unlock_sentence_ll(msg->filename, msg->sentence_index, msg->username);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        send_message(client_sockfd, &response);
        return -1;
    }
    
    // Save backup for undo before making changes
    save_undo_backup_ll(msg->filename);
    
    // Receive write commands until ETIRW
    int write_completed = 0;
    while (1) {
        Message write_cmd;
        if (receive_message(client_sockfd, &write_cmd) <= 0) {
            fprintf(stderr, "[WRITE] Failed to receive write command (client disconnected)\n");
            break;
        }
        
        // Check for end of write (ETIRW)
        if (strcmp(write_cmd.data, "ETIRW") == 0) {
            printf("[WRITE] Received ETIRW, completing write operation\n");
            
            // Ensure sentence has a delimiter (add newline if missing)
            ensure_sentence_delimiter_ll(msg->filename, msg->sentence_index);
            
            write_completed = 1;
            break;
        }
        
        // Perform the write operation
        int result = write_to_file_ll(msg->filename, msg->sentence_index, 
                                   write_cmd.word_index, write_cmd.data, msg->username);
        
        Message write_response;
        memset(&write_response, 0, sizeof(Message));
        write_response.msg_type = MSG_ACK;
        write_response.operation = OP_WRITE;
        
        if (result < 0) {
            write_response.msg_type = MSG_ERROR;
            if (result == ERR_SENTENCE_OUT_OF_RANGE) {
                write_response.error_code = ERR_SENTENCE_OUT_OF_RANGE;
            } else if (result == ERR_WORD_OUT_OF_RANGE) {
                write_response.error_code = ERR_WORD_OUT_OF_RANGE;
            } else {
                write_response.error_code = ERR_SERVER_ERROR;
            }
        } else {
            write_response.error_code = ERR_SUCCESS;
        }
        
        send_message(client_sockfd, &write_response);
    }
    
    // Always unlock the sentence, even if write didn't complete
    unlock_sentence_ll(msg->filename, msg->sentence_index, msg->username);
    printf("[WRITE] Unlocked sentence %d in file '%s' for user '%s'\n", 
           msg->sentence_index, msg->filename, msg->username);
    
    // If write didn't complete properly, rollback changes and return error
    if (!write_completed) {
        fprintf(stderr, "[WRITE] Write operation incomplete - rolling back changes\n");
        
        // Rollback: restore from undo backup
        if (undo_file_change_ll(msg->filename) == 0) {
            printf("[WRITE] Successfully rolled back incomplete write\n");
        } else {
            fprintf(stderr, "[WRITE] Failed to rollback - undo backup may not exist\n");
        }
        
        return -1;
    }
    
    // Write completed successfully - NOW sync to disk
    printf("[WRITE] ETIRW received - syncing changes to disk\n");
    sync_file_to_disk(msg->filename);
    
    // Update file modified timestamp
    update_file_modified_time_ll(msg->filename);
    
    // Replicate changes to backup server asynchronously (non-blocking)
    if (server_config.is_primary) {
        enqueue_replication_task(REP_OP_SYNC, msg->filename, NULL);
        printf("[WRITE] Enqueued async replication for '%s'\n", msg->filename);
    }
    
    // Send final success response immediately (don't wait for backup ACK)
    Message final_response;
    memset(&final_response, 0, sizeof(Message));
    final_response.msg_type = MSG_ACK;
    final_response.operation = OP_WRITE;
    final_response.error_code = ERR_SUCCESS;
    strcpy(final_response.data, "Write Successful!");
    send_message(client_sockfd, &final_response);
    
    printf("[WRITE] Write operation completed successfully\n");
    return 0;
}

// Handle STREAM operation from client
int handle_stream_request(int client_sockfd, Message *msg) {
    printf("[STREAM] User '%s' streaming file: %s\n", msg->username, msg->filename);
    
    // Check read access
    if (!has_read_access_ll(msg->filename, msg->username)) {
        printf("[STREAM] Access denied for user '%s'\n", msg->username);
        Message error_msg;
        memset(&error_msg, 0, sizeof(Message));
        error_msg.msg_type = MSG_ERROR;
        error_msg.error_code = ERR_NO_READ_ACCESS;
        send_message(client_sockfd, &error_msg);
        return -1;
    }
    
    // Get full file path
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", server_config.storage_dir, msg->filename);
    
    // Open file directly for streaming
    FILE *file = fopen(filepath, "r");
    if (!file) {
        printf("[STREAM] File not found: %s\n", msg->filename);
        Message error_msg;
        memset(&error_msg, 0, sizeof(Message));
        error_msg.msg_type = MSG_ERROR;
        error_msg.error_code = ERR_FILE_NOT_FOUND;
        send_message(client_sockfd, &error_msg);
        return -1;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    printf("[STREAM] File size: %ld bytes - starting word-by-word stream\n", file_size);
    
    // Update last accessed time
    update_file_accessed_time_ll(msg->filename, msg->username);
    
    // Stream file content word by word
    // Read file in chunks, parse into words, send with delays
    char buffer[MAX_DATA_SIZE - 100];
    char word_buffer[MAX_DATA_SIZE];
    int word_pos = 0;
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        for (size_t i = 0; i < bytes_read; i++) {
            char c = buffer[i];
            
            // Check if character is a word delimiter
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                // If we have accumulated a word, send it
                if (word_pos > 0) {
                    word_buffer[word_pos] = '\0';
                    
                    Message word_msg;
                    memset(&word_msg, 0, sizeof(Message));
                    word_msg.msg_type = MSG_RESPONSE;
                    word_msg.operation = OP_STREAM_WORD;
                    word_msg.error_code = ERR_SUCCESS;
                    strncpy(word_msg.data, word_buffer, MAX_DATA_SIZE - 1);
                    
                    if (send_message(client_sockfd, &word_msg) < 0) {
                        fprintf(stderr, "[STREAM] Failed to send word, client disconnected\n");
                        fclose(file);
                        return -1;
                    }
                    
                    // Sleep for 0.1 seconds (100,000 microseconds)
                    usleep(100000);
                    
                    word_pos = 0;
                }
            } else {
                // Accumulate character into current word
                if (word_pos < MAX_DATA_SIZE - 1) {
                    word_buffer[word_pos++] = c;
                }
            }
        }
    }
    
    // Send any remaining word
    if (word_pos > 0) {
        word_buffer[word_pos] = '\0';
        
        Message word_msg;
        memset(&word_msg, 0, sizeof(Message));
        word_msg.msg_type = MSG_RESPONSE;
        word_msg.operation = OP_STREAM_WORD;
        word_msg.error_code = ERR_SUCCESS;
        strncpy(word_msg.data, word_buffer, MAX_DATA_SIZE - 1);
        
        if (send_message(client_sockfd, &word_msg) < 0) {
            fprintf(stderr, "[STREAM] Failed to send word, client disconnected\n");
            fclose(file);
            return -1;
        }
    }
    
    fclose(file);
    
    // Send STOP message to indicate end of stream
    Message stop_msg;
    memset(&stop_msg, 0, sizeof(Message));
    stop_msg.msg_type = MSG_RESPONSE;
    stop_msg.operation = OP_STOP;
    stop_msg.error_code = ERR_SUCCESS;
    strcpy(stop_msg.data, "STOP");
    send_message(client_sockfd, &stop_msg);
    
    printf("[STREAM] Streaming completed\n");
    return 0;
}
