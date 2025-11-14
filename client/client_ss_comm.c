#include "client.h"
#include "../common/network_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Send READ request directly to Storage Server
int send_read_request(const char *filename) {
    // Get SS info from Name Server
    char ss_ip[MAX_IP_LEN];
    int ss_port;
    
    if (get_ss_info(filename, ss_ip, &ss_port) < 0) {
        return -1;
    }
    
    printf("Connecting to Storage Server at %s:%d\n", ss_ip, ss_port);
    
    // Connect to Storage Server
    int sockfd = connect_to_server(ss_ip, ss_port);
    if (sockfd < 0) {
        fprintf(stderr, "Failed to connect to Storage Server\n");
        return -1;
    }
    
    // Send READ request
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_READ;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    strncpy(request.filename, filename, MAX_FILENAME - 1);
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send READ request\n");
        close_socket(sockfd);
        return -1;
    }
    
    // CRITICAL FIX: Receive chunked response for large files
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive READ response\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (response.msg_type == MSG_ERROR) {
        if (response.error_code == ERR_NO_READ_ACCESS) {
            fprintf(stderr, "Error: No read access to file '%s'\n", filename);
        } else if (response.error_code == ERR_FILE_NOT_FOUND) {
            fprintf(stderr, "Error: File '%s' not found\n", filename);
        } else {
            fprintf(stderr, "Error: %d\n", response.error_code);
        }
        close_socket(sockfd);
        return -1;
    }
    
    // Check if this is a chunked transfer (file size in response)
    long file_size = response.sentence_index;
    
    if (file_size <= MAX_DATA_SIZE - 100) {
        // Small file - data is in first response
        printf("%s\n", response.data);
    } else {
        // Large file - receive chunks
        printf("[Receiving large file: %ld bytes]\n", file_size);
        
        size_t total_received = 0;
        int chunk_num = 0;
        
        while (1) {
            Message chunk_msg;
            if (receive_message(sockfd, &chunk_msg) < 0) {
                fprintf(stderr, "Failed to receive chunk\n");
                break;
            }
            
            if (chunk_msg.operation == OP_STOP) {
                printf("\n[File transfer complete: %zu bytes received]\n", total_received);
                break;
            }
            
            if (chunk_msg.operation == OP_READ_CHUNK) {
                size_t chunk_size = chunk_msg.sentence_index;
                
                // Write chunk data directly to stdout
                fwrite(chunk_msg.data, 1, chunk_size, stdout);
                fflush(stdout);
                
                total_received += chunk_size;
                chunk_num++;
                
                if (chunk_num % 10 == 0) {
                    fprintf(stderr, "\r[Received: %zu/%ld bytes (%d%%)]", 
                           total_received, file_size, 
                           (int)((total_received * 100) / file_size));
                }
            }
        }
        printf("\n");
    }
    
    close_socket(sockfd);
    return 0;
}

// Send WRITE request directly to Storage Server
int send_write_request(const char *filename, int sentence_index) {
    // Get SS info from Name Server
    char ss_ip[MAX_IP_LEN];
    int ss_port;
    
    if (get_ss_info(filename, ss_ip, &ss_port) < 0) {
        return -1;
    }
    
    printf("Connecting to Storage Server at %s:%d\n", ss_ip, ss_port);
    
    // Connect to Storage Server
    int sockfd = connect_to_server(ss_ip, ss_port);
    if (sockfd < 0) {
        fprintf(stderr, "Failed to connect to Storage Server\n");
        return -1;
    }
    
    // Send WRITE request to lock the sentence
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_WRITE;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    strncpy(request.filename, filename, MAX_FILENAME - 1);
    request.sentence_index = sentence_index;
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send WRITE request\n");
        close_socket(sockfd);
        return -1;
    }
    
    // Receive lock acknowledgment
    Message lock_response;
    if (receive_message(sockfd, &lock_response) < 0) {
        fprintf(stderr, "Failed to receive lock response\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (lock_response.msg_type == MSG_ERROR) {
        if (lock_response.error_code == ERR_NO_WRITE_ACCESS) {
            fprintf(stderr, "Error: No write access to file '%s'\n", filename);
        } else if (lock_response.error_code == ERR_SENTENCE_LOCKED) {
            fprintf(stderr, "Error: Sentence %d is currently locked by another user\n", sentence_index);
        } else if (lock_response.error_code == ERR_SENTENCE_OUT_OF_RANGE) {
            fprintf(stderr, "Error: Sentence index %d out of range\n", sentence_index);
        } else {
            fprintf(stderr, "Error: %d\n", lock_response.error_code);
        }
        close_socket(sockfd);
        return -1;
    }
    
    printf("Sentence %d locked. Enter write commands:\n", sentence_index);
    printf("Format: <word_index> <content>\n");
    printf("Enter 'ETIRW' to finish writing\n\n");
    
    // Interactive write loop
    char input[MAX_DATA_SIZE];
    while (1) {
        printf("> ");
        fflush(stdout);
        
        if (fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }
        
        // Remove newline
        input[strcspn(input, "\n")] = 0;
        
        // Check for ETIRW
        if (strcmp(input, "ETIRW") == 0) {
            Message end_msg;
            memset(&end_msg, 0, sizeof(Message));
            end_msg.msg_type = MSG_REQUEST;
            end_msg.operation = OP_WRITE;
            strcpy(end_msg.data, "ETIRW");
            
            if (send_message(sockfd, &end_msg) < 0) {
                fprintf(stderr, "Failed to send ETIRW\n");
                close_socket(sockfd);
                return -1;
            }
            
            // Receive final acknowledgment
            Message final_response;
            if (receive_message(sockfd, &final_response) < 0) {
                fprintf(stderr, "Failed to receive final response\n");
                close_socket(sockfd);
                return -1;
            }
            
            printf("%s\n", final_response.data);
            break;
        }
        
        // Parse word_index and content
        int word_index;
        char content[MAX_DATA_SIZE];
        
        if (sscanf(input, "%d %[^\n]", &word_index, content) != 2) {
            fprintf(stderr, "Invalid format. Use: <word_index> <content>\n");
            continue;
        }
        
        // Send write command
        Message write_cmd;
        memset(&write_cmd, 0, sizeof(Message));
        write_cmd.msg_type = MSG_REQUEST;
        write_cmd.operation = OP_WRITE;
        write_cmd.word_index = word_index;
        strncpy(write_cmd.data, content, MAX_DATA_SIZE - 1);
        
        if (send_message(sockfd, &write_cmd) < 0) {
            fprintf(stderr, "Failed to send write command\n");
            close_socket(sockfd);
            return -1;
        }
        
        // Receive acknowledgment
        Message write_response;
        if (receive_message(sockfd, &write_response) < 0) {
            fprintf(stderr, "Failed to receive write response\n");
            close_socket(sockfd);
            return -1;
        }
        
        if (write_response.msg_type == MSG_ERROR) {
            if (write_response.error_code == ERR_WORD_OUT_OF_RANGE) {
                fprintf(stderr, "Error: Word index out of range\n");
            } else {
                fprintf(stderr, "Error: %d\n", write_response.error_code);
            }
        }
    }
    
    close_socket(sockfd);
    return 0;
}

// Send STREAM request directly to Storage Server
int send_stream_request(const char *filename) {
    // Get SS info from Name Server
    char ss_ip[MAX_IP_LEN];
    int ss_port;
    
    if (get_ss_info(filename, ss_ip, &ss_port) < 0) {
        return -1;
    }
    
    printf("Connecting to Storage Server at %s:%d\n", ss_ip, ss_port);
    
    // Connect to Storage Server
    int sockfd = connect_to_server(ss_ip, ss_port);
    if (sockfd < 0) {
        fprintf(stderr, "Failed to connect to Storage Server\n");
        return -1;
    }
    
    // Send STREAM request
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_STREAM;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    strncpy(request.filename, filename, MAX_FILENAME - 1);
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send STREAM request\n");
        close_socket(sockfd);
        return -1;
    }
    
    printf("\n--- Streaming '%s' ---\n", filename);
    
    // Receive words one by one
    while (1) {
        Message word_msg;
        if (receive_message(sockfd, &word_msg) < 0) {
            fprintf(stderr, "\nError: Storage Server disconnected\n");
            close_socket(sockfd);
            return -1;
        }
        
        if (word_msg.msg_type == MSG_ERROR) {
            if (word_msg.error_code == ERR_NO_READ_ACCESS) {
                fprintf(stderr, "\nError: No read access to file '%s'\n", filename);
            } else if (word_msg.error_code == ERR_FILE_NOT_FOUND) {
                fprintf(stderr, "\nError: File '%s' not found\n", filename);
            } else {
                fprintf(stderr, "\nError: %d\n", word_msg.error_code);
            }
            close_socket(sockfd);
            return -1;
        }
        
        // Check for STOP message
        if (word_msg.operation == OP_STOP) {
            printf("\n--- End of stream ---\n");
            break;
        }
        
        // Display the word
        printf("%s ", word_msg.data);
        fflush(stdout);
    }
    
    close_socket(sockfd);
    return 0;
}

// Send UNDO request (through Name Server, but may involve SS)
int send_undo_request(const char *filename) {
    // Get SS info from Name Server
    char ss_ip[MAX_IP_LEN];
    int ss_port;
    
    if (get_ss_info(filename, ss_ip, &ss_port) < 0) {
        return -1;
    }
    
    printf("Connecting to Storage Server at %s:%d\n", ss_ip, ss_port);
    
    // Connect to Storage Server
    int sockfd = connect_to_server(ss_ip, ss_port);
    if (sockfd < 0) {
        fprintf(stderr, "Failed to connect to Storage Server\n");
        return -1;
    }
    
    // Send UNDO request
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_UNDO;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    strncpy(request.filename, filename, MAX_FILENAME - 1);
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send UNDO request\n");
        close_socket(sockfd);
        return -1;
    }
    
    // Receive response
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive UNDO response\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (response.msg_type == MSG_ERROR) {
        if (response.error_code == ERR_FILE_NOT_FOUND) {
            fprintf(stderr, "Error: File '%s' not found\n", filename);
        } else {
            fprintf(stderr, "Error: No undo history available\n");
        }
        close_socket(sockfd);
        return -1;
    }
    
    printf("Undo Successful!\n");
    
    close_socket(sockfd);
    return 0;
}
