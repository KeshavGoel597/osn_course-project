#include "client.h"
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
    
    // CRITICAL FIX: Client doesn't listen on any ports (only makes outgoing connections)
    // Set to 0 to indicate "not listening" rather than misleading hardcoded values
    client_config.nm_port = 0;   // Not used - client initiates connections
    client_config.ss_port = 0;   // Not used - client initiates connections
    
    printf("\n=== Client Initialization ===\n");
    printf("Username: %s\n", client_config.username);
    printf("Client IP: %s\n", client_config.client_ip);
    
    return 0;
}

// Register with Name Server
int register_with_nm() {
    int sockfd = connect_to_server(client_config.nm_ip, NM_PORT);
    if (sockfd < 0) {
        fprintf(stderr, "Failed to connect to Name Server at %s:%d\n", client_config.nm_ip, NM_PORT);
        fprintf(stderr, "Make sure the Name Server is running.\n");
        return -1;
    }
    
    printf("Connected to Name Server at %s:%d\n", client_config.nm_ip, NM_PORT);
    
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
            
        case CMD_CREATEFOLDER:
            return send_createfolder_request(cmd->filename);
            
        case CMD_MOVE:
            return send_move_request(cmd->filename, cmd->target_path);
            
        case CMD_VIEWFOLDER:
            return send_viewfolder_request(cmd->filename);
            
        case CMD_CHECKPOINT:
            return send_checkpoint_request(cmd->filename, cmd->checkpoint_tag);
            
        case CMD_VIEWCHECKPOINT:
            return send_viewcheckpoint_request(cmd->filename, cmd->checkpoint_tag);
            
        case CMD_REVERT:
            return send_revert_request(cmd->filename, cmd->checkpoint_tag);
            
        case CMD_LISTCHECKPOINTS:
            return send_listcheckpoints_request(cmd->filename);
            
        case CMD_REQUESTACCESS:
            return send_requestaccess_request(cmd->filename, cmd->access_type);
            
        case CMD_VIEWREQUESTS:
            return send_viewrequests_request();
            
        case CMD_APPROVEREQUEST:
            return send_approverequest_request(cmd->request_id);
            
        case CMD_REJECTREQUEST:
            return send_rejectrequest_request(cmd->request_id);
            
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
    // Accept nm_ip and username
    char nm_ip[MAX_IP_LEN] = "127.0.0.1";  // Default for backward compatibility
    char username[MAX_USERNAME];
    
    // Usage: ./client [nm_ip] [username]
    if (argc >= 2) {
        strncpy(nm_ip, argv[1], MAX_IP_LEN - 1);
    }
    
    if (argc >= 3) {
        strncpy(username, argv[2], MAX_USERNAME - 1);
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
    
    // CRITICAL FIX: Store nm_ip AFTER init_client() to avoid memset zeroing it out
    strncpy(client_config.nm_ip, nm_ip, MAX_IP_LEN - 1);
    
    // Register with Name Server
    // CRITICAL FIX: Don't allow offline mode - NM is required for all operations
    if (register_with_nm() < 0) {
        fprintf(stderr, "Failed to register with Name Server\n");
        fprintf(stderr, "Name Server connection is required for NFS operations.\n");
        fprintf(stderr, "Please ensure the Name Server is running and try again.\n");
        return 1;  // Exit instead of continuing in offline mode
    }
    
    // Start interactive shell
    start_client_shell();
    
    // Cleanup
    cleanup_client();
    
    return 0;
}
