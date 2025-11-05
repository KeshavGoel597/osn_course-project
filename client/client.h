#ifndef CLIENT_H
#define CLIENT_H

#include "../common/protocol.h"
#include <pthread.h>

// Client configuration
typedef struct {
    char username[MAX_USERNAME];
    char client_ip[MAX_IP_LEN];
    int nm_port;           // Port for Name Server connection
    int ss_port;           // Port for Storage Server connection
    int nm_sockfd;         // Socket for NM connection (may be used multiple times)
} ClientConfig;

// Global client configuration
extern ClientConfig client_config;

// Initialize client
int init_client(const char *username);

// Register with Name Server
int register_with_nm();

// Start client interactive shell
void start_client_shell();

// Cleanup client
void cleanup_client();

#endif // CLIENT_H
