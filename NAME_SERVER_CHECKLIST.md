# Name Server Integration Checklist

## For: Control Plane Developer (Name Server)
## From: Data Plane Developer (Storage Server + Client)

---

## Overview

The Storage Server and Client implementations are **complete and compiled**. The backup/replication functionality is also **fully implemented**. This checklist outlines what the Name Server needs to do for full system integration.

---

## 1. Storage Server Registration

### What Storage Server Sends
```c
Message reg_msg;
reg_msg.msg_type = MSG_REQUEST;
reg_msg.operation = OP_SS_REGISTER;
reg_msg.ss_id = server_config.ss_id;        // 1, 2, 3, 4, ... (NEW)
reg_msg.ip = "127.0.0.1";                   // SS IP
reg_msg.port1 = 9001;                       // NM connection port
reg_msg.port2 = 9002;                       // Client connection port
strcpy(reg_msg.data, "file1.txt,file2.txt"); // File list
```

### What Name Server Should Do
1. **Store SS information**:
   - SS_ID
   - IP address
   - NM port (for sending commands)
   - Client port (for redirecting clients)
   - File list
   - Is primary or backup? (ss_id % 2 == 1 ? PRIMARY : BACKUP)

2. **Determine backup pairing**:
   ```c
   if (ss_id % 2 == 1) {
       // This is a primary server (SS1, SS3, SS5, ...)
       primary_id = ss_id;
       backup_id = ss_id + 1;  // SS2, SS4, SS6, ...
   } else {
       // This is a backup server (SS2, SS4, SS6, ...)
       primary_id = ss_id - 1;  // SS1, SS3, SS5, ...
       backup_id = ss_id;
   }
   ```

3. **Notify primary of backup location** (NEW):
   ```c
   // When primary server registers (e.g., SS1)
   if (ss_id % 2 == 1) {
       // Look for backup server (ss_id + 1)
       BackupServer *backup = find_server_by_id(ss_id + 1);
       if (backup != NULL) {
           // Send backup info to primary
           Message msg;
           msg.operation = OP_BACKUP_INFO;  // You may need to define this
           strcpy(msg.backup_ip, backup->ip);
           msg.backup_port = backup->client_port;
           send_message(primary_sockfd, &msg);
           
           // Primary will call: connect_to_backup_server(backup_ip, backup_port)
       }
   }
   ```

4. **Send ACK**:
   ```c
   Message ack;
   ack.msg_type = MSG_ACK;
   ack.operation = OP_SS_REGISTER;
   ack.error_code = ERR_SUCCESS;
   send_message(ss_sockfd, &ack);
   ```

---

## 2. Client File Location Request

### What Client Sends
```c
Message req;
req.msg_type = MSG_REQUEST;
req.operation = OP_GET_SS_INFO;
strcpy(req.filename, "file.txt");
strcpy(req.username, "alice");
```

### What Name Server Should Do

#### Normal Case (Primary Available)
```c
// 1. Find which storage server has this file
int ss_id = find_ss_for_file(filename);

// 2. Get storage server info
StorageServer *ss = get_server_by_id(ss_id);

// 3. Check if primary server is alive
if (is_server_alive(ss_id)) {
    // 4. Send primary server info to client
    Message response;
    response.msg_type = MSG_RESPONSE;
    response.operation = OP_GET_SS_INFO;
    strcpy(response.ip, ss->ip);
    response.port1 = ss->client_port;  // Client should connect here
    response.error_code = ERR_SUCCESS;
    send_message(client_sockfd, &response);
}
```

#### Failover Case (Primary Down) - NEW
```c
// 1. Find which storage server has this file
int primary_id = find_ss_for_file(filename);

// 2. Check if primary server is alive
if (!is_server_alive(primary_id)) {
    // 3. Primary is down, redirect to backup
    int backup_id = primary_id + 1;  // SS1→SS2, SS3→SS4, etc.
    
    // 4. Get backup server info
    StorageServer *backup = get_server_by_id(backup_id);
    
    // 5. Check if backup is alive
    if (is_server_alive(backup_id)) {
        // 6. Send backup server info to client
        Message response;
        response.msg_type = MSG_RESPONSE;
        response.operation = OP_GET_SS_INFO;
        strcpy(response.ip, backup->ip);
        response.port1 = backup->client_port;
        response.error_code = ERR_SUCCESS;
        send_message(client_sockfd, &response);
        
        printf("[Failover] Redirected client to backup SS%d\n", backup_id);
    } else {
        // Both primary and backup down - error
        Message error;
        error.msg_type = MSG_ERROR;
        error.error_code = ERR_SERVER_ERROR;
        strcpy(error.data, "Both primary and backup servers unavailable");
        send_message(client_sockfd, &error);
    }
}
```

---

## 3. Health Monitoring (NEW)

### Heartbeat System
To detect when a primary server goes down:

```c
// Option 1: Passive monitoring (check during operations)
bool is_server_alive(int ss_id) {
    StorageServer *ss = get_server_by_id(ss_id);
    if (ss == NULL) return false;
    
    // Try to connect
    int sock = connect_to_server(ss->ip, ss->nm_port);
    if (sock < 0) {
        ss->is_alive = false;
        return false;
    }
    
    close(sock);
    ss->is_alive = true;
    return true;
}

// Option 2: Active heartbeat (recommended)
void* heartbeat_thread(void *arg) {
    while (running) {
        for (int i = 0; i < num_servers; i++) {
            StorageServer *ss = &servers[i];
            
            // Send heartbeat
            Message ping;
            ping.operation = OP_HEARTBEAT;  // You may need to define this
            
            int sock = connect_to_server(ss->ip, ss->nm_port);
            if (sock < 0) {
                ss->is_alive = false;
                printf("[Heartbeat] SS%d is DOWN\n", ss->ss_id);
            } else {
                if (send_message(sock, &ping) < 0) {
                    ss->is_alive = false;
                } else {
                    Message pong;
                    if (receive_message(sock, &pong) < 0) {
                        ss->is_alive = false;
                    } else {
                        ss->is_alive = true;
                    }
                }
                close(sock);
            }
        }
        
        sleep(5);  // Check every 5 seconds
    }
    return NULL;
}
```

---

## 4. File Operations (CREATE, DELETE)

### CREATE File

#### What Client Sends to NM
```c
Message req;
req.operation = OP_CREATE;
strcpy(req.filename, "newfile.txt");
strcpy(req.username, "alice");
```

#### What NM Should Do
```c
// 1. Choose a storage server (load balancing)
int ss_id = choose_storage_server();  // e.g., round-robin among primary servers

// 2. Get storage server info
StorageServer *ss = get_server_by_id(ss_id);

// 3. Send create command to storage server
int ss_sock = connect_to_server(ss->ip, ss->nm_port);

Message cmd;
cmd.msg_type = MSG_REQUEST;
cmd.operation = OP_SS_CREATE_FILE;
strcpy(cmd.filename, "newfile.txt");
strcpy(cmd.username, "alice");
send_message(ss_sock, &cmd);

// 4. Wait for response
Message response;
receive_message(ss_sock, &response);

// 5. Update file location mapping
add_file_to_ss(filename, ss_id);

// 6. Forward response to client
send_message(client_sockfd, &response);

close(ss_sock);

// Note: Storage server will automatically replicate to backup
```

### DELETE File

#### What Client Sends to NM
```c
Message req;
req.operation = OP_DELETE;
strcpy(req.filename, "oldfile.txt");
```

#### What NM Should Do
```c
// 1. Find which storage server has this file
int ss_id = find_ss_for_file(filename);

// 2. Get storage server info
StorageServer *ss = get_server_by_id(ss_id);

// 3. Send delete command to storage server
int ss_sock = connect_to_server(ss->ip, ss->nm_port);

Message cmd;
cmd.msg_type = MSG_REQUEST;
cmd.operation = OP_SS_DELETE_FILE;
strcpy(cmd.filename, "oldfile.txt");
send_message(ss_sock, &cmd);

// 4. Wait for response
Message response;
receive_message(ss_sock, &response);

// 5. Update file location mapping (remove)
remove_file_from_ss(filename, ss_id);

// 6. Forward response to client
send_message(client_sockfd, &response);

close(ss_sock);

// Note: Storage server will automatically replicate to backup
```

---

## 5. Data Structures Needed in NM

### Storage Server Information
```c
typedef struct {
    int ss_id;              // 1, 2, 3, 4, ...
    char ip[MAX_IP_LEN];    // IP address
    int nm_port;            // Port for NM connections
    int client_port;        // Port for client connections
    bool is_primary;        // true if ss_id is odd
    bool is_alive;          // Health status
    char files[MAX_FILES][MAX_FILENAME];  // List of files on this server
    int file_count;
    time_t last_heartbeat;  // Last successful heartbeat
} StorageServer;

StorageServer servers[MAX_SERVERS];
int num_servers = 0;
```

### File Location Mapping
```c
typedef struct {
    char filename[MAX_FILENAME];
    int primary_ss_id;      // Which primary server has this file
    int backup_ss_id;       // Corresponding backup server
} FileLocation;

FileLocation file_locations[MAX_FILES];
int num_files = 0;
```

---

## 6. Load Balancing

When creating a new file, choose storage server:

```c
int choose_storage_server() {
    // Option 1: Round-robin (simple)
    static int last_assigned = 1;  // Start with SS1
    int ss_id = last_assigned;
    
    // Find next available primary server
    while (ss_id <= num_servers) {
        if (ss_id % 2 == 1 && is_server_alive(ss_id)) {
            last_assigned = ss_id + 2;  // Skip to next odd number
            return ss_id;
        }
        ss_id += 2;
    }
    
    // If no primary available, error
    return -1;
    
    // Option 2: Least loaded (more complex)
    // Choose primary with fewest files
}
```

---

## 7. Protocol Operations Summary

### Operations Storage Server Handles
- `OP_SS_REGISTER` - Storage server registration
- `OP_SS_CREATE_FILE` - Create file (sent by NM)
- `OP_SS_DELETE_FILE` - Delete file (sent by NM)
- `OP_INFO` - Get file metadata
- `OP_EXEC` - Execute file
- `OP_READ` - Read file (sent by client directly)
- `OP_WRITE` - Write file (sent by client directly)
- `OP_STREAM` - Stream file (sent by client directly)
- `OP_UNDO` - Undo last change (sent by client directly)
- `OP_BACKUP_CREATE` - Backup operation (internal, primary→backup)
- `OP_BACKUP_DELETE` - Backup operation (internal, primary→backup)
- `OP_BACKUP_SYNC` - Backup operation (internal, primary→backup)

### Operations Name Server Handles
- `OP_SS_REGISTER` - From storage servers
- `OP_CLIENT_REGISTER` - From clients
- `OP_GET_SS_INFO` - From clients (get storage server location)
- `OP_CREATE` - From clients (create new file)
- `OP_DELETE` - From clients (delete file)
- `OP_LIST` - From clients (list all files)
- `OP_INFO` - From clients (get file info, forward to SS)

---

## 8. Testing Scenarios

### Test 1: Basic Replication
```bash
# Terminal 1: Start SS1 (primary)
./storage_server 1 9001 9002 ./storage_data1

# Terminal 2: Start SS2 (backup for SS1)
./storage_server 2 9003 9004 ./storage_data2

# Terminal 3: Start Name Server
./name_server

# Terminal 4: Start Client
./client alice
> CREATE test.txt

# Verify: File exists on both SS1 and SS2
```

### Test 2: Failover
```bash
# 1. Create file on SS1 (it replicates to SS2)
./client alice
> CREATE myfile.txt
> WRITE myfile.txt 0 0 Hello World!
> EXIT

# 2. Kill SS1
pkill -f "storage_server 1"

# 3. Try to read (should failover to SS2)
./client bob
> READ myfile.txt
# Should succeed, getting content from SS2
```

### Test 3: Load Balancing
```bash
# Start multiple primary servers
./storage_server 1 9001 9002 ./storage_data1
./storage_server 3 9005 9006 ./storage_data3
./storage_server 5 9009 9010 ./storage_data5

# Start corresponding backups
./storage_server 2 9003 9004 ./storage_data2
./storage_server 4 9007 9008 ./storage_data4
./storage_server 6 9011 9012 ./storage_data6

# Create multiple files - should distribute across SS1, SS3, SS5
./client alice
> CREATE file1.txt  # Should go to SS1
> CREATE file2.txt  # Should go to SS3
> CREATE file3.txt  # Should go to SS5
```

---

## 9. Error Cases to Handle

1. **No storage servers available**
   - Return ERR_SERVER_ERROR to client

2. **Primary down, backup also down**
   - Return ERR_SERVER_ERROR
   - Log error for admin

3. **File not found on any server**
   - Return ERR_FILE_NOT_FOUND

4. **Storage server registration with same SS_ID**
   - Update existing entry (server restart case)
   - Or reject if already alive (duplicate SS_ID)

---

## 10. Recommended Implementation Order

1. ✅ **Basic storage server registration** (no backup logic yet)
2. ✅ **File location tracking** (which SS has which file)
3. ✅ **Client GET_SS_INFO** (return primary server)
4. ✅ **CREATE/DELETE operations** (forward to SS)
5. ✅ **LIST operation** (aggregate from all servers)
6. **Health monitoring** (heartbeat or passive check)
7. **Failover logic** (redirect to backup if primary down)
8. **Backup pairing notification** (tell primary about backup)

---

## 11. Files You'll Need

```
name_server/
├── name_server.c         # Main NM logic
├── name_server.h         # NM data structures
├── nm_ss_comm.c          # Communication with storage servers
├── nm_client_comm.c      # Communication with clients
├── server_registry.c     # Track registered servers
├── file_registry.c       # Track file locations
├── load_balancer.c       # Choose server for new files
└── Makefile
```

---

## 12. Quick Reference: What's Already Done

✅ **Storage Server**
- File operations (CREATE, READ, WRITE, DELETE)
- Backup replication (automatic)
- Linked list file handler (scalable)
- Sentence-level locking
- UNDO support
- Access control

✅ **Client**
- Interactive shell
- All commands implemented
- Direct SS communication for READ/WRITE
- NM communication for file location

⏳ **Name Server (Your Work)**
- Storage server registry
- File location tracking
- Failover logic
- Load balancing
- Backup pairing notification

---

## Questions?

If you need clarification on any Storage Server functionality, check:
- `storage server/BACKUP_REPLICATION.md` - Detailed backup design
- `storage server/LINKED_LIST_IMPLEMENTATION.md` - File handler design
- `ARCHITECTURE_DIAGRAM.md` - Visual diagrams
- `common/protocol.h` - All message structures

Good luck with the Name Server implementation! 🚀
