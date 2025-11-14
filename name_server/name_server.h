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
#define MAX_ACCESS_REQUESTS 1000
#define HEARTBEAT_INTERVAL 10  // seconds
#define HEARTBEAT_TIMEOUT 30   // seconds
#define FILE_HASH_TABLE_SIZE 10007  // Prime number for better distribution

// Storage Server Status
#define SS_STATUS_ONLINE 1
#define SS_STATUS_OFFLINE 0
#define SS_STATUS_ACTING_PRIMARY 2
#define SS_STATUS_RECOVERING 3    // New status for servers coming back online
#define SS_STATUS_SYNCING 4       // New status for servers being synchronized

// Enhanced replication states
#define REPLICATION_STATUS_SYNCED 0
#define REPLICATION_STATUS_OUT_OF_SYNC 1
#define REPLICATION_STATUS_FAILED 2

// File Information Structure
typedef struct {
    char filename[MAX_FILENAME];
    char owner[MAX_USERNAME];
    int primary_ss_id;
    int backup_ss_id;
    time_t created_time;
    time_t modified_time;
} FileInfo;

// Access Request Structure
typedef struct {
    char request_id[MAX_FILENAME];  // Format: "filename:username:timestamp"
    char filename[MAX_FILENAME];
    char requester[MAX_USERNAME];
    char owner[MAX_USERNAME];
    int access_type;  // ACCESS_READ or ACCESS_WRITE
    time_t request_time;
    int status;  // 0=pending, 1=approved, 2=rejected
} AccessRequest;

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

// Recovery and synchronization tracking
typedef struct {
    int ss_id;
    time_t failure_time;
    time_t recovery_start_time;
    int sync_progress;        // Percentage of files synchronized
    int total_files_to_sync;
    int files_synced;
    int sync_complete;
} SSRecoveryInfo;

// Enhanced replication tracking per SS pair
typedef struct {
    int primary_ss_id;
    int backup_ss_id;
    int replication_status;   // REPLICATION_STATUS_*
    time_t last_sync_time;
    int failed_replications;  // Count of recent failures
    int auto_failover_enabled;
} ReplicationPairInfo;

// Cache Entry for Recent File Searches (LRU Cache)
#define CACHE_SIZE 100
#define CACHE_TTL 60  // Cache entries expire after 60 seconds

typedef struct CacheEntry {
    char filename[MAX_FILENAME];
    int primary_ss_id;
    int backup_ss_id;
    time_t timestamp;
    int valid;  // 1 if entry is valid, 0 if empty
} CacheEntry;

typedef struct {
    CacheEntry entries[CACHE_SIZE];
    int next_evict_index;  // Simple round-robin eviction
    pthread_mutex_t cache_mutex;
} FileLocationCache;

// Global Name Server State
typedef struct {
    StorageServerInfo storage_servers[MAX_STORAGE_SERVERS];
    int ss_count;
    pthread_mutex_t ss_list_mutex;
    
    ClientInfo clients[MAX_CLIENTS];
    int client_count;
    pthread_mutex_t client_list_mutex;
    
    FileHashTable file_index;  // Hash table for O(1) file lookups
    
    FileLocationCache search_cache;  // LRU cache for recent file searches
    
    AccessRequest access_requests[MAX_ACCESS_REQUESTS];
    int request_count;
    pthread_mutex_t request_mutex;
    
    int next_primary_ss;  // Round-robin for file assignment
    pthread_mutex_t assignment_mutex;
    
    // Enhanced fault tolerance tracking
    ReplicationPairInfo replication_pairs[MAX_STORAGE_SERVERS];
    int replication_pair_count;
    pthread_mutex_t replication_mutex;
    
    SSRecoveryInfo recovery_sessions[MAX_STORAGE_SERVERS];
    int active_recoveries;
    pthread_mutex_t recovery_mutex;
    
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
void handle_recovery_sync_complete(int ss_id);  // NEW: Called when SS completes recovery sync

// Hash Table Functions for Efficient File Search (O(1))
void init_file_hash_table();
unsigned int hash_filename(const char *filename);
void hash_insert_file(FileInfo *file);
void hash_remove_file(const char *filename);
FileInfo* hash_find_file(const char *filename);

// Cache Functions for Recent File Searches
void init_file_cache();
void cache_insert(const char *filename, int primary_ss_id, int backup_ss_id);
int cache_lookup(const char *filename, int *primary_ss_id, int *backup_ss_id);
void cache_invalidate(const char *filename);
void cache_clear();

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
void handle_createfolder(int socket, Message *msg);
void handle_move_file(int socket, Message *msg);
void handle_viewfolder(int socket, Message *msg);
void handle_checkpoint(int socket, Message *msg);
void handle_viewcheckpoint(int socket, Message *msg);
void handle_revert(int socket, Message *msg);
void handle_listcheckpoints(int socket, Message *msg);
void handle_requestaccess(int socket, Message *msg);
void handle_viewrequests(int socket, Message *msg);
void handle_approverequest(int socket, Message *msg);
void handle_rejectrequest(int socket, Message *msg);

// Heartbeat and monitoring
void* heartbeat_monitor(void* arg);
void send_heartbeat_to_ss(int ss_id);
int is_storage_server_alive(int ss_id);

// Enhanced fault tolerance functions
int initialize_replication_system();
int create_replication_pair(int primary_ss_id, int backup_ss_id);
int handle_ss_reconnection(int ss_id);
int start_ss_recovery_sync(int recovering_ss_id, int partner_ss_id);
int monitor_replication_health();
int perform_emergency_failover(int failed_ss_id);
int synchronize_ss_after_recovery(int recovering_ss_id);
int replicate_all_writes_async(const char *filename, const char *operation, const char *data);
int validate_data_consistency(int primary_ss_id, int backup_ss_id);
void update_replication_status(int primary_ss_id, int backup_ss_id, int status);
void* replication_monitor_thread(void* arg);

// Recovery and synchronization functions
void* ss_recovery_manager(void* arg);
int sync_metadata_between_ss(int source_ss_id, int target_ss_id);
int sync_files_between_ss(int source_ss_id, int target_ss_id);
int verify_sync_completion(int ss_id);

// Utility functions
void print_server_status();
void log_operation(const char *operation, const char *details);

// Global state
extern NameServerState *nm_state;

#endif // NAME_SERVER_H
