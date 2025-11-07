#ifndef STORAGE_SERVER_H
#define STORAGE_SERVER_H

#include "../common/protocol.h"
#include <pthread.h>

// Global configuration
typedef struct {
    int nm_port;           // Port for Name Server connections
    int client_port;       // Port for Client connections
    char storage_dir[MAX_PATH];  // Directory to store files
    char ss_ip[MAX_IP_LEN];      // Storage Server IP
    int nm_sockfd;         // Socket for NM connection
    int client_sockfd;     // Socket for client connections
    
    // Backup/Replication configuration
    int ss_id;             // Storage Server ID (1, 2, 3, ...)
    int is_primary;        // 1 if odd (primary), 0 if even (backup)
    int is_acting_primary; // 1 if backup is acting as primary (after failover)
    char backup_ip[MAX_IP_LEN];  // Backup server IP
    int backup_port;       // Backup server port for replication
    int backup_sockfd;     // Socket for backup connection (-1 if not connected)
    int bulk_sync_complete; // 1 if initial bulk sync is done
} SSConfig;

// Global server configuration
extern SSConfig server_config;

// Thread argument structure
typedef struct {
    int sockfd;
    char client_ip[MAX_IP_LEN];
} ThreadArg;

// Initialize storage server
int init_storage_server(int nm_port, int client_port, const char *storage_dir);

// Start listening for connections
int start_server();

// Handle Name Server connections (thread function)
void* handle_nm_connections(void *arg);

// Handle Client connections (thread function)
void* handle_client_connections(void *arg);

// Cleanup and shutdown server
void shutdown_server();

// Signal handler for graceful shutdown
void signal_handler(int signum);

#endif // STORAGE_SERVER_H
