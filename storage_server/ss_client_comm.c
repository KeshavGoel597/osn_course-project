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
            // CRITICAL FIX: Only backup servers (not primary, not acting primary) should handle backup operations
            if (!server_config.is_primary && !server_config.is_acting_primary) {
                printf("[Client Handler] Received backup operation from primary\n");
                handle_backup_request(client_sockfd);
            } else {
                fprintf(stderr, "[Client Handler] Cannot handle backup operation (this is a primary or acting-primary server)\n");
            }
            break;
            
        case OP_UNDO: {
            printf("[UNDO] User '%s' undoing file: %s\n", request.username, request.filename);
            
            Message response;
            memset(&response, 0, sizeof(Message));
            response.msg_type = MSG_ACK;
            response.operation = OP_UNDO;
            
            // CRITICAL: Acquire global commit lock to prevent race conditions
            lock_commit();
            
            int result = undo_file_change_ll(request.filename);
            if (result < 0) {
                response.msg_type = MSG_ERROR;
                response.error_code = ERR_INVALID_OPERATION;
                strcpy(response.data, "No undo history available. Perform a WRITE operation first.");
            } else {
                response.error_code = ERR_SUCCESS;
                strcpy(response.data, "Undo successful");
                
                // Update file statistics after UNDO
                update_file_statistics_ll(request.filename);
                
                // Replicate restored file to backup server
                if (server_config.is_primary || server_config.is_acting_primary) {
                    enqueue_replication_task(REP_OP_SYNC, request.filename, NULL);
                    printf("[UNDO] Enqueued async replication for '%s'\n", request.filename);
                }
            }
            
            unlock_commit();
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
    
    response.error_code = ERR_SUCCESS;
    response.sentence_index = file_size;  // Use sentence_index to send file size
    
    // For small files, send content directly in first response
    if (file_size <= MAX_DATA_SIZE - 100) {
        size_t bytes_read = fread(response.data, 1, file_size, file);
        response.data[bytes_read] = '\0';  // Null terminate for safety
        
        if (send_message(client_sockfd, &response) < 0) {
            fclose(file);
            return -1;
        }
        
        printf("[READ] Sent small file directly: %ld bytes\n", file_size);
    } else {
        // Large file - send file size first, then chunks
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
        
        // Send STOP message to indicate end of transfer
        Message stop_msg;
        memset(&stop_msg, 0, sizeof(Message));
        stop_msg.msg_type = MSG_RESPONSE;
        stop_msg.operation = OP_STOP;
        stop_msg.error_code = ERR_SUCCESS;
        strcpy(stop_msg.data, "READ_COMPLETE");
        send_message(client_sockfd, &stop_msg);
        
        printf("[READ] Successfully sent %zu bytes in %zu chunks\n", bytes_sent, chunk_num);
    }
    
    fclose(file);
    
    // Update last accessed time
    update_file_accessed_time_ll(msg->filename, msg->username);
    
    return 0;
}

// Handle WRITE operation from client
// Global mutex for commit operations to prevent race conditions
static pthread_mutex_t commit_mutex = PTHREAD_MUTEX_INITIALIZER;

// Lock/unlock functions for commit serialization
void lock_commit() {
    pthread_mutex_lock(&commit_mutex);
}

void unlock_commit() {
    pthread_mutex_unlock(&commit_mutex);
}

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
    
    char filepath[MAX_PATH];
    get_file_path(msg->filename, filepath, MAX_PATH);
    
    // CRITICAL FIX: Validate sentence index based on splitting file content by '.' (period only)
    // This implements the authoritative sentence-splitting rule from the prompt
    FILE *validation_fp = fopen(filepath, "r");
    if (validation_fp != NULL) {
        // Get file size
        fseek(validation_fp, 0, SEEK_END);
        long file_size = ftell(validation_fp);
        fseek(validation_fp, 0, SEEK_SET);
        
        // Read entire file content
        char *file_content = (char*)malloc(file_size + 1);
        if (file_content != NULL) {
            size_t bytes_read = fread(file_content, 1, file_size, validation_fp);
            file_content[bytes_read] = '\0';
            fclose(validation_fp);
            
            // Count sentence slots by splitting on '.' and keeping empty segments
            // The number of sentence slots = number of '.' delimiters + 1
            int sentence_slots = 1; // Start with 1 (initial segment before any '.')
            for (size_t i = 0; i < bytes_read; i++) {
                if (file_content[i] == '.') {
                    sentence_slots++;
                }
            }
            
            free(file_content);
            
            // Validate sentence_index: must be in range [0, sentence_slots-1]
            if (msg->sentence_index < 0 || msg->sentence_index >= sentence_slots) {
                printf("[WRITE] Sentence index %d out of range (valid: 0-%d) for file '%s'\n",
                       msg->sentence_index, sentence_slots - 1, msg->filename);
                response.msg_type = MSG_ERROR;
                response.error_code = ERR_SENTENCE_OUT_OF_RANGE;
                strcpy(response.data, "ERROR: Sentence index out of range.");
                send_message(client_sockfd, &response);
                return -1;
            }
            
            printf("[WRITE] Sentence index validation passed: %d is valid (0-%d)\n",
                   msg->sentence_index, sentence_slots - 1);
        } else {
            fclose(validation_fp);
        }
    }
    
    // Check file size for memory safety
    struct stat file_stat;
    if (stat(filepath, &file_stat) == 0) {
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
        response.error_code = lock_result;
        send_message(client_sockfd, &response);
        return -1;
    }
    
    // Get the file and record original word count for conflict detection
    LoadedFile* doc = get_file_from_cache(msg->filename);
    if (!doc) {
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        unlock_sentence_ll(msg->filename, msg->sentence_index, msg->username);
        send_message(client_sockfd, &response);
        return -1;
    }
    
    // Get target sentence and record original word count
    SentenceNode* target_sent = doc->sentences_head;
    int current_index = 0;
    while (target_sent != NULL && current_index < msg->sentence_index) {
        target_sent = target_sent->next;
        current_index++;
    }
    
    int original_word_count = 0;
    if (target_sent) {
        WordNode* word = target_sent->words_head;
        while (word) {
            original_word_count++;
            word = word->next;
        }
    }
    
    file_release(doc);  // Release reference
    
    // Send acknowledgment that write mode is active
    response.error_code = ERR_SUCCESS;
    strcpy(response.data, "WRITE_MODE");
    send_message(client_sockfd, &response);
    printf("[WRITE] Write mode activated, original word count: %d\n", original_word_count);
    
    // Save backup for undo before making changes
    save_undo_backup_ll(msg->filename);
    
    // Track edit operations during write session
    typedef struct {
        int word_index;
        char word[256];
    } EditOperation;
    
    int edit_capacity = 1000;
    EditOperation* edit_ops = malloc(edit_capacity * sizeof(EditOperation));
    int edit_count = 0;
    int delimiter_index = -1;  // Track first delimiter position
    
    if (!edit_ops) {
        unlock_sentence_ll(msg->filename, msg->sentence_index, msg->username);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Memory allocation error");
        send_message(client_sockfd, &response);
        return -1;
    }
    
    // Receive write operations until ETIRW
    int write_completed = 0;
    while (1) {
        Message write_cmd;
        if (receive_message(client_sockfd, &write_cmd) <= 0) {
            fprintf(stderr, "[WRITE] Failed to receive write command (client disconnected)\n");
            break;
        }
        
        // Check for end of write (ETIRW)
        if (strcmp(write_cmd.data, "ETIRW") == 0) {
            printf("[WRITE] Received ETIRW, beginning commit process\n");
            write_completed = 1;
            break;
        }
        
        // Parse and validate write command
        int base_word_idx = write_cmd.word_index;
        char* content = write_cmd.data;
        
        // Split content by spaces
        char content_copy[MAX_DATA_SIZE];
        strncpy(content_copy, content, MAX_DATA_SIZE - 1);
        content_copy[MAX_DATA_SIZE - 1] = '\0';
        
        // First pass: validate all words
        char* validation_copy = strdup(content_copy);
        char* token = strtok(validation_copy, " ");
        int word_offset = 0;
        int first_delimiter_in_batch = -1;
        
        while (token != NULL) {
            int word_idx = base_word_idx + word_offset;
            
            // Check if word has delimiter
            int word_len = strlen(token);
            if (word_len > 0) {
                char last_char = token[word_len - 1];
                if (last_char == '.' || last_char == '!' || last_char == '?') {
                    if (first_delimiter_in_batch < 0) {
                        first_delimiter_in_batch = word_idx;
                    }
                }
            }
            
            // Check delimiter boundary enforcement
            if (delimiter_index >= 0 && word_idx > delimiter_index) {
                Message err_response = {0};
                err_response.msg_type = MSG_ERROR;
                err_response.error_code = ERR_WORD_OUT_OF_RANGE;
                snprintf(err_response.data, MAX_DATA_SIZE, 
                        "Index %d is beyond sentence delimiter at position %d", 
                        word_idx, delimiter_index);
                send_message(client_sockfd, &err_response);
                free(validation_copy);
                goto next_command;
            }
            
            // CRITICAL FIX: Validate against sentence size (matching TinyOS logic EXACTLY)
            // In TinyOS: expected_size = sentence->word_count + word_offset
            // Since TinyOS updates sentence->word_count in real-time, we simulate this:
            // effective_word_count = original + all previously queued edits + current batch progress
            int effective_word_count = original_word_count + edit_count + word_offset;
            
            printf("[WRITE DEBUG] Validating word_idx=%d against effective_word_count=%d (original=%d + queued=%d + batch_progress=%d)\n",
                   word_idx, effective_word_count, original_word_count, edit_count, word_offset);
            
            if (word_idx < 0 || word_idx > effective_word_count) {
                Message err_response = {0};
                err_response.msg_type = MSG_ERROR;
                err_response.error_code = ERR_WORD_OUT_OF_RANGE;
                snprintf(err_response.data, MAX_DATA_SIZE, 
                        "Index %d out of range (valid: 0-%d)", 
                        word_idx, effective_word_count);
                send_message(client_sockfd, &err_response);
                printf("[WRITE] VALIDATION FAILED: word_idx=%d > effective_word_count=%d\n",
                       word_idx, effective_word_count);
                free(validation_copy);
                goto next_command;
            }
            
            printf("[WRITE DEBUG] Validation PASSED for word_idx=%d\n", word_idx);
            
            token = strtok(NULL, " ");
            word_offset++;
        }
        free(validation_copy);
        
        // Second pass: store edit operations
        token = strtok(content_copy, " ");
        word_offset = 0;
        
        while (token != NULL) {
            int word_idx = base_word_idx + word_offset;
            
            // Expand array if needed
            if (edit_count >= edit_capacity) {
                edit_capacity *= 2;
                EditOperation* new_ops = realloc(edit_ops, edit_capacity * sizeof(EditOperation));
                if (!new_ops) {
                    Message err_response = {0};
                    err_response.msg_type = MSG_ERROR;
                    err_response.error_code = ERR_SERVER_ERROR;
                    strcpy(err_response.data, "Memory allocation error");
                    send_message(client_sockfd, &err_response);
                    free(edit_ops);
                    unlock_sentence_ll(msg->filename, msg->sentence_index, msg->username);
                    return -1;
                }
                edit_ops = new_ops;
            }
            
            // Store the edit operation
            edit_ops[edit_count].word_index = word_idx;
            strncpy(edit_ops[edit_count].word, token, 255);
            edit_ops[edit_count].word[255] = '\0';
            edit_count++;
            
            // Update delimiter tracking
            if (delimiter_index >= 0 && word_idx <= delimiter_index) {
                delimiter_index++;
            }
            
            token = strtok(NULL, " ");
            word_offset++;
        }
        
        // Set delimiter index if found in this batch
        if (first_delimiter_in_batch >= 0 && delimiter_index < 0) {
            delimiter_index = first_delimiter_in_batch;
        }
        
        // Send acknowledgment
        Message ack = {0};
        ack.msg_type = MSG_ACK;
        ack.operation = OP_WRITE;
        ack.error_code = ERR_SUCCESS;
        snprintf(ack.data, MAX_DATA_SIZE, "OK (%d operations queued, delimiter at %d)", 
                edit_count, delimiter_index);
        send_message(client_sockfd, &ack);
        
next_command:
        continue;
    }
    
    // Always unlock the sentence
    unlock_sentence_ll(msg->filename, msg->sentence_index, msg->username);
    
    // If write didn't complete, rollback and return error
    if (!write_completed) {
        fprintf(stderr, "[WRITE] Write operation incomplete - rolling back changes\n");
        undo_file_change_ll(msg->filename);
        free(edit_ops);
        return -1;
    }
    
    // Acquire global commit lock for atomic operation
    lock_commit();
    
    // Re-read file for latest state before applying edits
    doc = get_file_from_cache(msg->filename);
    if (!doc) {
        free(edit_ops);
        unlock_commit();
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        send_message(client_sockfd, &response);
        return -1;
    }
    
    // Get fresh target sentence
    target_sent = doc->sentences_head;
    current_index = 0;
    while (target_sent != NULL && current_index < msg->sentence_index) {
        target_sent = target_sent->next;
        current_index++;
    }
    
    if (!target_sent && msg->sentence_index == doc->sentence_count) {
        // Create new sentence if appending
        target_sent = create_sentence_node('\0');
        if (target_sent) {
            if (doc->sentences_head == NULL) {
                doc->sentences_head = target_sent;
            } else {
                SentenceNode* last = doc->sentences_head;
                while (last->next) last = last->next;
                last->next = target_sent;
            }
            doc->sentence_count++;
        }
    }
    
    if (!target_sent) {
        free(edit_ops);
        file_release(doc);
        unlock_commit();
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SENTENCE_OUT_OF_RANGE;
        send_message(client_sockfd, &response);
        return -1;
    }
    
    // Calculate word offset for conflict resolution
    int current_word_count = 0;
    WordNode* word = target_sent->words_head;
    while (word) {
        current_word_count++;
        word = word->next;
    }
    
    int word_offset = current_word_count - original_word_count;
    printf("[WRITE] Applying %d edits with offset %d (original: %d, current: %d)\n", 
           edit_count, word_offset, original_word_count, current_word_count);
    
    // Apply edit operations with conflict resolution
    for (int i = 0; i < edit_count; i++) {
        int original_idx = edit_ops[i].word_index;
        int adjusted_idx = original_idx + word_offset;
        
        // Clamp to valid range
        int current_count = 0;
        word = target_sent->words_head;
        while (word) {
            current_count++;
            word = word->next;
        }
        
        if (adjusted_idx < 0) adjusted_idx = 0;
        if (adjusted_idx > current_count) adjusted_idx = current_count;
        
        // Insert word at adjusted position
        WordNode* new_word = create_word_node(edit_ops[i].word);
        if (new_word) {
            if (adjusted_idx == 0) {
                new_word->next = target_sent->words_head;
                target_sent->words_head = new_word;
            } else {
                WordNode* prev = target_sent->words_head;
                for (int j = 1; j < adjusted_idx && prev && prev->next; j++) {
                    prev = prev->next;
                }
                if (prev) {
                    new_word->next = prev->next;
                    prev->next = new_word;
                }
            }
        }
    }
    
    free(edit_ops);
    file_release(doc);
    
    // Sync to disk
    sync_file_to_disk(msg->filename);
    update_file_write_stats_ll(msg->filename);
    
    unlock_commit();
    
    // Send final success response
    Message final_response;
    memset(&final_response, 0, sizeof(Message));
    final_response.msg_type = MSG_ACK;
    final_response.operation = OP_WRITE;
    final_response.error_code = ERR_SUCCESS;
    strcpy(final_response.data, "Write completed successfully");
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
    char filepath[MAX_PATH];
    get_file_path(msg->filename, filepath, MAX_PATH);
    
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
