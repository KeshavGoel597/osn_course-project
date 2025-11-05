#include "client.h"
#include "command_parser.h"
#include "client_nm_comm.h"
#include "client_ss_comm.h"
#include "../common/network_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

// Global client configuration
ClientConfig client_config;

// Initialize client
int init_client(const char *username) {
    memset(&client_config, 0, sizeof(ClientConfig));
    
    strncpy(client_config.username, username, MAX_USERNAME - 1);
    strncpy(client_config.client_ip, "127.0.0.1", MAX_IP_LEN - 1);
    client_config.nm_port = 7001;   // Example port for NM connection
    client_config.ss_port = 7002;   // Example port for SS connection
    
    printf("\n=== Client Initialization ===\n");
    printf("Username: %s\n", client_config.username);
    printf("Client IP: %s\n", client_config.client_ip);
    
    return 0;
}

// Register with Name Server
int register_with_nm() {
    int sockfd = connect_to_server(NM_IP, NM_PORT);
    if (sockfd < 0) {
        fprintf(stderr, "Failed to connect to Name Server at %s:%d\n", NM_IP, NM_PORT);
        fprintf(stderr, "Make sure the Name Server is running.\n");
        return -1;
    }
    
    printf("Connected to Name Server at %s:%d\n", NM_IP, NM_PORT);
    
    // Prepare registration message
    Message reg_msg;
    memset(&reg_msg, 0, sizeof(Message));
    
    reg_msg.msg_type = MSG_REQUEST;
    reg_msg.operation = OP_CLIENT_REGISTER;
    strncpy(reg_msg.username, client_config.username, MAX_USERNAME - 1);
    strncpy(reg_msg.ip, client_config.client_ip, MAX_IP_LEN - 1);
    reg_msg.port1 = client_config.nm_port;   // Port for NM connection
    reg_msg.port2 = client_config.ss_port;   // Port for SS connection
    
    // Send registration message
    if (send_message(sockfd, &reg_msg) < 0) {
        fprintf(stderr, "Failed to send registration message\n");
        close_socket(sockfd);
        return -1;
    }
    
    printf("Sent registration message to Name Server\n");
    
    // Wait for acknowledgment
    Message ack_msg;
    if (receive_message(sockfd, &ack_msg) < 0) {
        fprintf(stderr, "Failed to receive acknowledgment from Name Server\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (ack_msg.msg_type == MSG_ACK && ack_msg.error_code == ERR_SUCCESS) {
        printf("Registration acknowledged by Name Server\n");
        close_socket(sockfd);
        return 0;
    } else {
        fprintf(stderr, "Registration failed with error code: %d\n", ack_msg.error_code);
        close_socket(sockfd);
        return -1;
    }
}

// Handle commands
static int handle_command(ParsedCommand *cmd) {
    switch (cmd->type) {
        case CMD_VIEW:
            return send_view_request(cmd->view_flags);
            
        case CMD_READ:
            return send_read_request(cmd->filename);
            
        case CMD_CREATE:
            return send_create_request(cmd->filename);
            
        case CMD_WRITE:
            return send_write_request(cmd->filename, cmd->sentence_index);
            
        case CMD_DELETE:
            return send_delete_request(cmd->filename);
            
        case CMD_INFO:
            return send_info_request(cmd->filename);
            
        case CMD_STREAM:
            return send_stream_request(cmd->filename);
            
        case CMD_LIST:
            return send_list_request();
            
        case CMD_ADDACCESS:
            return send_addaccess_request(cmd->filename, cmd->target_user, cmd->access_type);
            
        case CMD_REMACCESS:
            return send_remaccess_request(cmd->filename, cmd->target_user);
            
        case CMD_EXEC:
            return send_exec_request(cmd->filename);
            
        case CMD_UNDO:
            return send_undo_request(cmd->filename);
            
        case CMD_HELP:
            display_help();
            return 0;
            
        case CMD_EXIT:
            return -1;  // Signal to exit
            
        default:
            fprintf(stderr, "Unknown or invalid command. Type 'HELP' for available commands.\n");
            return 0;
    }
}

// Start client interactive shell
void start_client_shell() {
    printf("\n=== Distributed File System Client ===\n");
    printf("User: %s\n", client_config.username);
    printf("Type 'HELP' for available commands, 'EXIT' to quit.\n\n");
    
    char input[1024];
    ParsedCommand cmd;
    
    while (1) {
        printf("%s> ", client_config.username);
        fflush(stdout);
        
        if (fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }
        
        // Remove newline
        input[strcspn(input, "\n")] = 0;
        
        // Skip empty lines
        if (strlen(input) == 0) {
            continue;
        }
        
        // Parse command
        CommandType type = parse_command(input, &cmd);
        
        if (type == CMD_EXIT) {
            printf("Exiting client...\n");
            break;
        }
        
        if (type == CMD_UNKNOWN) {
            fprintf(stderr, "Unknown or invalid command. Type 'HELP' for available commands.\n");
            continue;
        }
        
        // Handle command
        handle_command(&cmd);
        printf("\n");
    }
}

// Cleanup client
void cleanup_client() {
    printf("Client cleanup complete\n");
}

// Signal handler for graceful shutdown
void signal_handler(int signum) {
    printf("\nReceived signal %d\n", signum);
    cleanup_client();
    exit(0);
}

int main(int argc, char *argv[]) {
    // Prompt for username if not provided
    char username[MAX_USERNAME];
    
    if (argc >= 2) {
        strncpy(username, argv[1], MAX_USERNAME - 1);
    } else {
        printf("Enter username: ");
        fflush(stdout);
        if (fgets(username, sizeof(username), stdin) == NULL) {
            fprintf(stderr, "Failed to read username\n");
            return 1;
        }
        username[strcspn(username, "\n")] = 0;
    }
    
    if (strlen(username) == 0) {
        fprintf(stderr, "Username cannot be empty\n");
        return 1;
    }
    
    // Register signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize client
    if (init_client(username) < 0) {
        fprintf(stderr, "Failed to initialize client\n");
        return 1;
    }
    
    // Register with Name Server
    if (register_with_nm() < 0) {
        fprintf(stderr, "Failed to register with Name Server\n");
        fprintf(stderr, "Continuing in offline mode (limited functionality)\n");
    }
    
    // Start interactive shell
    start_client_shell();
    
    // Cleanup
    cleanup_client();
    
    return 0;
}
