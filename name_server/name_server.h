#ifndef NAME_SERVER_H
#define NAME_SERVER_H

#include "../common/protocol.h"
#include "../common/network_utils.h"
#include <pthread.h>
#include <time.h>

// Maximum limits
#define MAX_STORAGE_SERVERS 100
#define MAX_FILES_PER_SERVER 1000
#define MAX_CLIENTS 100
#define HEARTBEAT_INTERVAL 10  // seconds
#define HEARTBEAT_TIMEOUT 30   // seconds
#define FILE_HASH_TABLE_SIZE 10007  // Prime number for better distribution

// Storage Server Status
#define SS_STATUS_ONLINE 1
#define SS_STATUS_OFFLINE 0
#define SS_STATUS_ACTING_PRIMARY 2

// File Information Structure
typedef struct {
    char filename[MAX_FILENAME];
    char owner[MAX_USERNAME];
    int primary_ss_id;
    int backup_ss_id;
    time_t created_time;
    time_t modified_time;
} FileInfo;

// Storage Server Information Structure
typedef struct {
    int ss_id;
    char ip[MAX_IP_LEN];
    int nm_port;        // Port for Name Server connections
    int client_port;    // Port for Client connections
    int status;         // SS_STATUS_ONLINE, SS_STATUS_OFFLINE, SS_STATUS_ACTING_PRIMARY
    time_t last_heartbeat;
    int backup_ss_id;   // Which SS this backs up (0 if none)
    int is_primary;     // 1 if primary, 0 if backup
    FileInfo files[MAX_FILES_PER_SERVER];
    int file_count;
    pthread_mutex_t ss_mutex;  // Protects this SS's data
} StorageServerInfo;

// Client Information Structure
typedef struct {
    char username[MAX_USERNAME];
    char ip[MAX_IP_LEN];
    int port;
    time_t last_activity;
    int is_connected;
} ClientInfo;

// Hash Table Entry for Fast File Lookup (O(1))
typedef struct FileHashNode {
    char filename[MAX_FILENAME];
    FileInfo *file_ptr;  // Pointer to the actual FileInfo in storage_servers array
    struct FileHashNode *next;  // For collision chaining
} FileHashNode;

// Hash Table for Efficient File Search
typedef struct {
    FileHashNode *buckets[FILE_HASH_TABLE_SIZE];
    pthread_mutex_t hash_mutex;  // Protects hash table operations
} FileHashTable;

// Global Name Server State
typedef struct {
    StorageServerInfo storage_servers[MAX_STORAGE_SERVERS];
    int ss_count;
    pthread_mutex_t ss_list_mutex;
    
    ClientInfo clients[MAX_CLIENTS];
    int client_count;
    pthread_mutex_t client_list_mutex;
    
    FileHashTable file_index;  // Hash table for O(1) file lookups
    
    int next_primary_ss;  // Round-robin for file assignment
    pthread_mutex_t assignment_mutex;
    
    int server_socket;
    int running;
} NameServerState;

// Function declarations
// Main server functions
int init_name_server();
void start_name_server();
void stop_name_server();
void cleanup_name_server();

// Storage Server management
int register_storage_server(Message *msg);
int find_storage_server(int ss_id);
int find_primary_for_file(const char *filename);
int find_backup_for_primary(int primary_ss_id);
void setup_backup_pairing(int primary_ss_id);
void handle_storage_server_failure(int ss_id);
void promote_backup_to_primary(int backup_ss_id, int failed_primary_ss_id);

// Hash Table Functions for Efficient File Search (O(1))
void init_file_hash_table();
unsigned int hash_filename(const char *filename);
void hash_insert_file(FileInfo *file);
void hash_remove_file(const char *filename);
FileInfo* hash_find_file(const char *filename);

// Client management
int register_client(Message *msg);
int find_client(const char *username);

// File management
int add_file_to_server(int ss_id, const char *filename, const char *owner);
int remove_file_from_server(int ss_id, const char *filename);
FileInfo* find_file(const char *filename);
int get_available_primary_server();

// Request handlers
void handle_connection(void *arg);
void handle_client_request(int client_socket);
void handle_storage_server_request(int server_socket);
void handle_storage_server_registration(int socket, Message *msg);
void handle_client_registration(int socket, Message *msg);
void handle_get_ss_info(int client_socket, Message *msg);
void handle_create_file(int client_socket, Message *msg);
void handle_delete_file(int client_socket, Message *msg);
void handle_info_request(int socket, Message *msg);
void handle_addaccess(int client_socket, Message *msg);
void handle_remaccess(int client_socket, Message *msg);
void handle_list_files(int client_socket, Message *msg);
void handle_view_files(int client_socket, Message *msg);
void handle_exec(int client_socket, Message *msg);

// Heartbeat and monitoring
void* heartbeat_monitor(void* arg);
void send_heartbeat_to_ss(int ss_id);
int is_storage_server_alive(int ss_id);

// Utility functions
void print_server_status();
void log_operation(const char *operation, const char *details);

// Global state
extern NameServerState *nm_state;

#endif // NAME_SERVER_H
