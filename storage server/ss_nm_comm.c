#include "ss_nm_comm.h"
#include "file_handler_ll.h"
#include "backup_handler.h"
#include "storage_server.h"
#include "../common/network_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

// Register storage server with Name Server
int register_with_nm(int nm_port, int client_port, const char *ss_ip) {
    // Connect to Name Server
    int nm_sockfd = connect_to_server(NM_IP, NM_PORT);
    if (nm_sockfd < 0) {
        fprintf(stderr, "Failed to connect to Name Server at %s:%d\n", NM_IP, NM_PORT);
        return -1;
    }
    
    printf("Connected to Name Server at %s:%d\n", NM_IP, NM_PORT);
    
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
    if (receive_message(nm_sockfd, &request) < 0) {
        fprintf(stderr, "[NM Handler] Failed to receive message\n");
        close_socket(nm_sockfd);
        return NULL;
    }
    
    printf("[NM Handler] Received operation: %d\n", request.operation);
    
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
                // Replicate to backup server if this is a primary server
                if (server_config.is_primary) {
                    if (replicate_create(request.filename, request.username) < 0) {
                        fprintf(stderr, "[NM Handler] Warning: Failed to replicate CREATE to backup\n");
                    }
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
                // Replicate to backup server if this is a primary server
                if (server_config.is_primary) {
                    if (replicate_delete(request.filename) < 0) {
                        fprintf(stderr, "[NM Handler] Warning: Failed to replicate DELETE to backup\n");
                    }
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
            
            // Read file content for execution
            char content[MAX_DATA_SIZE];
            if (read_file_ll(request.filename, content, MAX_DATA_SIZE) < 0) {
                response.error_code = ERR_FILE_NOT_FOUND;
            } else {
                // Send file content back to NM (NM will execute it)
                strncpy(response.data, content, MAX_DATA_SIZE - 1);
                response.msg_type = MSG_RESPONSE;
            }
            break;
        }
        
        case OP_SS_ADDACCESS: {
            printf("[NM Handler] ADDACCESS request: %s for user %s\n", 
                   request.filename, request.username);
            
            // Parse access type from data field
            int access_type = ACCESS_READ; // Default
            if (strstr(request.data, "RW") || strstr(request.data, "READWRITE")) {
                access_type = ACCESS_READ_WRITE;
            } else if (strstr(request.data, "W") || strstr(request.data, "WRITE")) {
                access_type = ACCESS_WRITE;
            }
            
            int result = add_user_access_ll(request.filename, request.username, access_type);
            if (result < 0) {
                response.error_code = ERR_SERVER_ERROR;
            } else {
                // Replicate metadata to backup
                if (server_config.is_primary) {
                    if (replicate_metadata() < 0) {
                        fprintf(stderr, "[NM Handler] Warning: Failed to replicate metadata\n");
                    }
                }
            }
            break;
        }
        
        case OP_SS_REMACCESS: {
            printf("[NM Handler] REMACCESS request: %s for user %s\n", 
                   request.filename, request.username);
            
            int result = remove_user_access_ll(request.filename, request.username);
            if (result < 0) {
                response.error_code = ERR_SERVER_ERROR;
            } else {
                // Replicate metadata to backup
                if (server_config.is_primary) {
                    if (replicate_metadata() < 0) {
                        fprintf(stderr, "[NM Handler] Warning: Failed to replicate metadata\n");
                    }
                }
            }
            break;
        }
        
        case OP_NM_BACKUP_INFO: {
            printf("[NM Handler] Received backup info from NM\n");
            
            // Extract backup server details
            char backup_ip[MAX_IP_LEN];
            int backup_port;
            strncpy(backup_ip, request.ip, MAX_IP_LEN - 1);
            backup_port = request.port1;
            
            printf("[NM Handler] Backup server: %s:%d\n", backup_ip, backup_port);
            
            // Handle backup info (connect and bulk sync)
            if (handle_nm_backup_info(backup_ip, backup_port) < 0) {
                response.error_code = ERR_CONNECTION_FAILED;
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
