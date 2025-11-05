#include "storage_server.h"
#include "file_handler_ll.h"
#include "backup_handler.h"
#include "ss_nm_comm.h"
#include "ss_client_comm.h"
#include "../common/network_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>

// Global server configuration
SSConfig server_config;

// Initialize storage server
int init_storage_server(int nm_port, int client_port, const char *storage_dir) {
    server_config.nm_port = nm_port;
    server_config.client_port = client_port;
    strncpy(server_config.storage_dir, storage_dir, MAX_PATH - 1);
    
    // Get local IP address (simplified - using localhost for now)
    strncpy(server_config.ss_ip, "127.0.0.1", MAX_IP_LEN - 1);
    
    printf("=== Storage Server Initialization ===\n");
    printf("NM Port: %d\n", nm_port);
    printf("Client Port: %d\n", client_port);
    printf("Storage Directory: %s\n", storage_dir);
    printf("IP Address: %s\n", server_config.ss_ip);
    
    // Initialize file handler (linked list version)
    if (init_file_handler_ll(storage_dir) < 0) {
        fprintf(stderr, "Failed to initialize file handler\n");
        return -1;
    }
    
    // Load existing metadata
    if (load_metadata_ll() < 0) {
        fprintf(stderr, "Warning: Could not load metadata (might be first run)\n");
    }
    
    // Initialize backup handler
    if (init_backup_handler() < 0) {
        fprintf(stderr, "Warning: Could not initialize backup handler\n");
    }
    
    printf("Storage Server initialized successfully\n");
    return 0;
}

// Start listening for connections
int start_server() {
    // Create socket for Name Server connections
    server_config.nm_sockfd = create_socket();
    if (server_config.nm_sockfd < 0) {
        fprintf(stderr, "Failed to create NM socket\n");
        return -1;
    }
    
    if (bind_socket(server_config.nm_sockfd, server_config.nm_port) < 0) {
        fprintf(stderr, "Failed to bind NM socket to port %d\n", server_config.nm_port);
        return -1;
    }
    
    if (listen_socket(server_config.nm_sockfd, 5) < 0) {
        fprintf(stderr, "Failed to listen on NM socket\n");
        return -1;
    }
    
    printf("Listening for Name Server connections on port %d\n", server_config.nm_port);
    
    // Create socket for Client connections
    server_config.client_sockfd = create_socket();
    if (server_config.client_sockfd < 0) {
        fprintf(stderr, "Failed to create client socket\n");
        return -1;
    }
    
    if (bind_socket(server_config.client_sockfd, server_config.client_port) < 0) {
        fprintf(stderr, "Failed to bind client socket to port %d\n", server_config.client_port);
        return -1;
    }
    
    if (listen_socket(server_config.client_sockfd, 10) < 0) {
        fprintf(stderr, "Failed to listen on client socket\n");
        return -1;
    }
    
    printf("Listening for Client connections on port %d\n", server_config.client_port);
    
    // Register with Name Server
    printf("\nRegistering with Name Server...\n");
    if (register_with_nm(server_config.nm_port, server_config.client_port, server_config.ss_ip) < 0) {
        fprintf(stderr, "Failed to register with Name Server\n");
        return -1;
    }
    printf("Successfully registered with Name Server\n");
    
    // Start accepting connections in separate threads
    pthread_t nm_thread, client_thread;
    
    // Thread for handling NM connections
    if (pthread_create(&nm_thread, NULL, handle_nm_connections, NULL) != 0) {
        fprintf(stderr, "Failed to create NM handler thread\n");
        return -1;
    }
    
    // Thread for handling client connections
    if (pthread_create(&client_thread, NULL, handle_client_connections, NULL) != 0) {
        fprintf(stderr, "Failed to create client handler thread\n");
        return -1;
    }
    
    printf("\n=== Storage Server is running ===\n");
    printf("Press Ctrl+C to shutdown\n\n");
    
    // Wait for threads
    pthread_join(nm_thread, NULL);
    pthread_join(client_thread, NULL);
    
    return 0;
}

// Handle Name Server connections
void* handle_nm_connections(void *arg) {
    while (1) {
        char client_ip[MAX_IP_LEN];
        int nm_conn = accept_connection(server_config.nm_sockfd, client_ip);
        
        if (nm_conn < 0) {
            fprintf(stderr, "Failed to accept NM connection\n");
            continue;
        }
        
        printf("Accepted connection from Name Server (%s)\n", client_ip);
        
        // Create thread to handle this connection
        pthread_t thread;
        ThreadArg *arg = malloc(sizeof(ThreadArg));
        arg->sockfd = nm_conn;
        strncpy(arg->client_ip, client_ip, MAX_IP_LEN - 1);
        
        if (pthread_create(&thread, NULL, handle_nm_connection, arg) != 0) {
            fprintf(stderr, "Failed to create thread for NM connection\n");
            close_socket(nm_conn);
            free(arg);
            continue;
        }
        
        pthread_detach(thread);
    }
    return NULL;
}

// Handle Client connections
void* handle_client_connections(void *arg) {
    while (1) {
        char client_ip[MAX_IP_LEN];
        int client_conn = accept_connection(server_config.client_sockfd, client_ip);
        
        if (client_conn < 0) {
            fprintf(stderr, "Failed to accept client connection\n");
            continue;
        }
        
        printf("Accepted connection from Client (%s)\n", client_ip);
        
        // Create thread to handle this connection
        pthread_t thread;
        ThreadArg *arg = malloc(sizeof(ThreadArg));
        arg->sockfd = client_conn;
        strncpy(arg->client_ip, client_ip, MAX_IP_LEN - 1);
        
        if (pthread_create(&thread, NULL, handle_client_connection, arg) != 0) {
            fprintf(stderr, "Failed to create thread for client connection\n");
            close_socket(client_conn);
            free(arg);
            continue;
        }
        
        pthread_detach(thread);
    }
    return NULL;
}

// Cleanup and shutdown server
void shutdown_server() {
    printf("\n=== Shutting down Storage Server ===\n");
    
    // Save metadata before shutdown
    save_metadata_ll();
    
    // Close sockets
    if (server_config.nm_sockfd >= 0) {
        close_socket(server_config.nm_sockfd);
    }
    if (server_config.client_sockfd >= 0) {
        close_socket(server_config.client_sockfd);
    }
    
    // Cleanup backup handler
    cleanup_backup_handler();
    
    // Cleanup file handler (frees all in-memory structures)
    cleanup_file_handler_ll();
    
    printf("Storage Server shut down successfully\n");
}

// Signal handler for graceful shutdown
void signal_handler(int signum) {
    printf("\nReceived signal %d\n", signum);
    shutdown_server();
    exit(0);
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <ss_id> <nm_port> <client_port> <storage_dir>\n", argv[0]);
        fprintf(stderr, "Example: %s 1 9001 9002 ./storage_data1\n", argv[0]);
        fprintf(stderr, "         SS_ID: 1=primary, 2=backup for SS1, 3=primary, 4=backup for SS3, ...\n");
        return 1;
    }
    
    int ss_id = atoi(argv[1]);
    int nm_port = atoi(argv[2]);
    int client_port = atoi(argv[3]);
    const char *storage_dir = argv[4];
    
    // Determine if this is a primary or backup server
    server_config.ss_id = ss_id;
    server_config.is_primary = (ss_id % 2 == 1);  // Odd = primary, Even = backup
    server_config.is_acting_primary = 0;  // Initially false
    server_config.backup_sockfd = -1;     // Not connected yet
    server_config.bulk_sync_complete = 0; // No sync yet
    
    // Register signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize server
    if (init_storage_server(nm_port, client_port, storage_dir) < 0) {
        fprintf(stderr, "Failed to initialize storage server\n");
        return 1;
    }
    
    // Start server
    if (start_server() < 0) {
        fprintf(stderr, "Failed to start storage server\n");
        shutdown_server();
        return 1;
    }
    
    return 0;
}
