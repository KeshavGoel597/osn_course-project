#include "storage_server_all.h"
#include "../common/network_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <netdb.h>

// Define constants if not already defined
#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif
#ifndef NI_NUMERICHOST
#define NI_NUMERICHOST 1
#endif

// Global server configuration
SSConfig server_config;

// Global replication queue for asynchronous backups
ReplicationQueue replication_queue;

// Global flag to control server running state
volatile sig_atomic_t server_running = 1;

// CRITICAL FIX: Get actual non-loopback IP address
int get_local_ip(char *ip_buffer, size_t buffer_size) {
    struct ifaddrs *ifaddr, *ifa;
    int family;
    char host[NI_MAXHOST];
    
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        // Fallback to localhost
        strncpy(ip_buffer, "127.0.0.1", buffer_size - 1);
        return -1;
    }
    
    // Iterate through interfaces to find first non-loopback IPv4 address
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        
        family = ifa->ifa_addr->sa_family;
        
        // We want IPv4 addresses only
        if (family == AF_INET) {
            int s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in),
                               host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            if (s != 0) {
                continue;
            }
            
            // Skip loopback
            if (strncmp(host, "127.", 4) != 0) {
                strncpy(ip_buffer, host, buffer_size - 1);
                ip_buffer[buffer_size - 1] = '\0';
                freeifaddrs(ifaddr);
                printf("[IP Detection] Found non-loopback IP: %s (%s)\n", host, ifa->ifa_name);
                return 0;
            }
        }
    }
    
    freeifaddrs(ifaddr);
    
    // No non-loopback address found, use localhost
    printf("[IP Detection] No non-loopback IP found, using localhost\n");
    strncpy(ip_buffer, "127.0.0.1", buffer_size - 1);
    return 0;
}

// Initialize storage server
int init_storage_server(int nm_port, int client_port, const char *storage_dir, const char *override_ip) {
    server_config.nm_port = nm_port;
    server_config.client_port = client_port;
    strncpy(server_config.storage_dir, storage_dir, MAX_PATH - 1);
    
    // CRITICAL FIX: Allow IP address override for cross-device setups
    if (override_ip != NULL && strlen(override_ip) > 0) {
        // Use provided IP address
        strncpy(server_config.ss_ip, override_ip, MAX_IP_LEN - 1);
        server_config.ss_ip[MAX_IP_LEN - 1] = '\0';
        printf("[IP Override] Using provided IP: %s\n", server_config.ss_ip);
    } else {
        // Auto-detect IP address
        if (get_local_ip(server_config.ss_ip, MAX_IP_LEN) < 0) {
            fprintf(stderr, "Warning: Could not detect IP address, using 127.0.0.1\n");
        }
    }
    
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
    
    // Initialize asynchronous replication (for primary servers)
    if (server_config.is_primary) {
        if (init_async_replication() < 0) {
            fprintf(stderr, "Warning: Could not initialize async replication\n");
        }
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
    pthread_detach(nm_thread);  // Detach so we don't need to join
    
    // Thread for handling client connections
    if (pthread_create(&client_thread, NULL, handle_client_connections, NULL) != 0) {
        fprintf(stderr, "Failed to create client handler thread\n");
        return -1;
    }
    pthread_detach(client_thread);  // Detach so we don't need to join
    
    printf("\n=== Storage Server is running ===\n");
    printf("Press Ctrl+C to shutdown\n\n");
    
    // Keep main thread alive until shutdown signal
    while (server_running) {
        sleep(1);
    }
    
    return 0;
}

// Handle Name Server connections
void* handle_nm_connections(void *arg) {
    (void)arg; // Suppress unused parameter warning
    while (server_running) {
        char client_ip[MAX_IP_LEN];
        int nm_conn = accept_connection(server_config.nm_sockfd, client_ip);
        
        if (nm_conn < 0) {
            if (!server_running) break;  // Exit gracefully if shutting down
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
    (void)arg; // Suppress unused parameter warning
    while (server_running) {
        char client_ip[MAX_IP_LEN];
        int client_conn = accept_connection(server_config.client_sockfd, client_ip);
        
        if (client_conn < 0) {
            if (!server_running) break;  // Exit gracefully if shutting down
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
    
    // Close sockets (if not already closed by signal handler)
    if (server_config.nm_sockfd >= 0) {
        close_socket(server_config.nm_sockfd);
        server_config.nm_sockfd = -1;
    }
    if (server_config.client_sockfd >= 0) {
        close_socket(server_config.client_sockfd);
        server_config.client_sockfd = -1;
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
    server_running = 0; // Set flag to stop server loops
    
    // Close listening sockets to unblock accept() calls
    if (server_config.nm_sockfd >= 0) {
        shutdown(server_config.nm_sockfd, SHUT_RDWR);
        close(server_config.nm_sockfd);
        server_config.nm_sockfd = -1;
    }
    if (server_config.client_sockfd >= 0) {
        shutdown(server_config.client_sockfd, SHUT_RDWR);
        close(server_config.client_sockfd);
        server_config.client_sockfd = -1;
    }
    
    // Shutdown will be handled by main thread after waking from sleep loop
}

int main(int argc, char *argv[]) {
    if (argc < 6 || argc > 7) {
        fprintf(stderr, "Usage: %s <ss_id> <nm_ip> <nm_port> <client_port> <storage_dir> [ss_ip]\n", argv[0]);
        fprintf(stderr, "Example: %s 1 127.0.0.1 9001 9002 ./storage_data1\n", argv[0]);
        fprintf(stderr, "         %s 1 192.168.1.100 9001 9002 ./storage_data1  # For remote NM\n", argv[0]);
        fprintf(stderr, "         %s 1 10.0.2.15 9001 9002 ./storage_data1 10.5.23.14  # Override SS IP\n", argv[0]);
        fprintf(stderr, "         SS_ID: 1=primary, 2=backup for SS1, 3=primary, 4=backup for SS3, ...\n");
        fprintf(stderr, "         NM_IP: IP address of Name Server (use 127.0.0.1 for localhost)\n");
        fprintf(stderr, "         SS_IP: (Optional) Override auto-detected IP for this storage server\n");
        return 1;
    }
    
    // CRITICAL FIX: Accept Name Server IP as command-line argument
    int ss_id = atoi(argv[1]);
    const char *nm_ip = argv[2];
    int nm_port = atoi(argv[3]);
    int client_port = atoi(argv[4]);
    const char *storage_dir = argv[5];
    const char *ss_ip_override = (argc == 7) ? argv[6] : NULL;  // Optional SS IP override
    
    // Validate ss_id
    if (ss_id <= 0 || ss_id > 100) {
        fprintf(stderr, "Error: SS_ID must be between 1 and 100\n");
        return 1;
    }
    
    // Validate NM IP address (basic check)
    if (strlen(nm_ip) == 0 || strlen(nm_ip) >= MAX_IP_LEN) {
        fprintf(stderr, "Error: Invalid Name Server IP address\n");
        return 1;
    }
    
    // Store NM IP in server config
    strncpy(server_config.nm_ip, nm_ip, MAX_IP_LEN - 1);
    server_config.nm_ip[MAX_IP_LEN - 1] = '\0';
    
    // Validate ports
    if (nm_port <= 1024 || nm_port > 65535) {
        fprintf(stderr, "Error: NM port must be between 1025 and 65535\n");
        return 1;
    }
    
    if (client_port <= 1024 || client_port > 65535) {
        fprintf(stderr, "Error: Client port must be between 1025 and 65535\n");
        return 1;
    }
    
    if (nm_port == client_port) {
        fprintf(stderr, "Error: NM port and Client port must be different\n");
        return 1;
    }
    
    // Validate storage directory path
    if (strlen(storage_dir) == 0 || strlen(storage_dir) >= MAX_PATH) {
        fprintf(stderr, "Error: Invalid storage directory path\n");
        return 1;
    }
    
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
    if (init_storage_server(nm_port, client_port, storage_dir, ss_ip_override) < 0) {
        fprintf(stderr, "Failed to initialize storage server\n");
        return 1;
    }
    
    // Start server
    if (start_server() < 0) {
        fprintf(stderr, "Failed to start storage server\n");
        shutdown_server();
        return 1;
    }
    
    // Server has stopped (signal received), perform cleanup
    shutdown_server();
    
    return 0;
}
