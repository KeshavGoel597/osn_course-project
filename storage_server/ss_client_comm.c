#include "ss_client_comm.h"
#include "storage_server.h"
#include "file_handler_ll.h"
#include "backup_handler.h"
#include "../common/network_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    
    // Read file content
    if (read_file_ll(msg->filename, response.data, MAX_DATA_SIZE) < 0) {
        printf("[READ] File not found: %s\n", msg->filename);
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        send_message(client_sockfd, &response);
        return -1;
    }
    
    response.error_code = ERR_SUCCESS;
    send_message(client_sockfd, &response);
    printf("[READ] Successfully sent file content\n");
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
    
    // Try to lock the sentence
    int lock_result = lock_sentence_ll(msg->filename, msg->sentence_index, msg->username);
    if (lock_result != 0) {
        printf("[WRITE] Sentence is locked\n");
        response.msg_type = MSG_ERROR;
        response.error_code = ERR_SENTENCE_LOCKED;
        send_message(client_sockfd, &response);
        return -1;
    }
    
    // Send acknowledgment that lock is acquired
    response.error_code = ERR_SUCCESS;
    strcpy(response.data, "LOCKED");
    send_message(client_sockfd, &response);
    printf("[WRITE] Sentence locked, waiting for write commands\n");
    
    // Save backup for undo before making changes
    save_undo_backup_ll(msg->filename);
    
    // Receive write commands until ETIRW
    while (1) {
        Message write_cmd;
        if (receive_message(client_sockfd, &write_cmd) <= 0) {
            fprintf(stderr, "[WRITE] Failed to receive write command\n");
            unlock_sentence_ll(msg->filename, msg->sentence_index, msg->username);
            return -1;
        }
        
        // Check for end of write (ETIRW)
        if (strcmp(write_cmd.data, "ETIRW") == 0) {
            printf("[WRITE] Received ETIRW, completing write operation\n");
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
    
    // Unlock the sentence
    unlock_sentence_ll(msg->filename, msg->sentence_index, msg->username);
    
    // Replicate changes to backup server if this is a primary server
    if (server_config.is_primary) {
        if (replicate_sync(msg->filename) < 0) {
            fprintf(stderr, "[WRITE] Warning: Failed to replicate changes to backup\n");
        } else {
            printf("[WRITE] Successfully replicated changes to backup\n");
        }
    }
    
    // Send final success response
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
    
    // Read file content
    char content[MAX_DATA_SIZE];
    if (read_file_ll(msg->filename, content, MAX_DATA_SIZE) < 0) {
        printf("[STREAM] File not found: %s\n", msg->filename);
        Message error_msg;
        memset(&error_msg, 0, sizeof(Message));
        error_msg.msg_type = MSG_ERROR;
        error_msg.error_code = ERR_FILE_NOT_FOUND;
        send_message(client_sockfd, &error_msg);
        return -1;
    }
    
    printf("[STREAM] Starting to stream content word by word\n");
    
    // Parse content into words and send one by one
    char *token = strtok(content, " \t\n\r");
    while (token != NULL) {
        Message word_msg;
        memset(&word_msg, 0, sizeof(Message));
        word_msg.msg_type = MSG_RESPONSE;
        word_msg.operation = OP_STREAM_WORD;
        word_msg.error_code = ERR_SUCCESS;
        strncpy(word_msg.data, token, MAX_DATA_SIZE - 1);
        
        if (send_message(client_sockfd, &word_msg) < 0) {
            fprintf(stderr, "[STREAM] Failed to send word, client disconnected\n");
            return -1;
        }
        
        // Sleep for 0.1 seconds (100,000 microseconds)
        usleep(100000);
        
        token = strtok(NULL, " \t\n\r");
    }
    
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
