# Name Server Implementation Tracking

## Overview
The Name Server acts as the coordinator/control plane for the distributed file system. It manages Storage Server registration, file location tracking, backup pairing, and failover handling.

## Core Responsibilities
- [x] Track Storage Servers and their status
- [x] Handle Storage Server registration 
- [x] Manage backup pairing (odd=primary, even=backup)
- [x] Route client requests to appropriate Storage Servers
- [x] Handle file creation/deletion coordination
- [x] Implement heartbeat monitoring for failover
- [x] Manage access control operations

## Implementation Status

### ✅ Completed Components

#### 1. Data Structures
- [x] `StorageServer` struct for tracking SS info
- [x] `FileLocation` struct for file-to-server mapping
- [x] Global arrays for managing servers and files

#### 2. Storage Server Management
- [x] `register_storage_server()` - Register new SS
- [x] `find_backup_partner()` - Pair primary with backup
- [x] `send_backup_info()` - Notify primary of backup details
- [x] Backup pairing logic (SS1↔SS2, SS3↔SS4, etc.)

#### 3. File Location Management
- [x] `add_file_location()` - Track where files are stored
- [x] `find_file_location()` - Lookup file location for clients
- [x] `remove_file_location()` - Remove file from tracking

#### 4. Request Handlers
- [x] `handle_ss_register()` - SS registration handler
- [x] `handle_get_ss_info()` - Client file location requests
- [x] `handle_create_file()` - Coordinate file creation
- [x] `handle_delete_file()` - Coordinate file deletion
- [x] `handle_addaccess()` - Add user access permissions
- [x] `handle_remaccess()` - Remove user access permissions

#### 5. Connection Management
- [x] Main server initialization
- [x] Connection acceptance loop
- [x] Message handling dispatch

#### 6. Failover Logic
- [x] `check_server_health()` - Heartbeat monitoring
- [x] Redirect clients to backup when primary fails
- [x] Acting primary designation

#### 7. Build System
- [x] Makefile for compilation
- [x] Successfully compiles with common utilities

## Key Features Implemented

### Backup Pairing Strategy
```
SS1 (ID=1) ↔ SS2 (ID=2) - Primary/Backup pair
SS3 (ID=3) ↔ SS4 (ID=4) - Primary/Backup pair
SS5 (ID=5) ↔ SS6 (ID=6) - Primary/Backup pair
```

### File Location Routing
- Clients request file location from Name Server
- Name Server returns Storage Server IP and port
- Clients connect directly to Storage Server
- Automatic failover to backup if primary is down

### Load Balancing
- Round-robin assignment of new files to primaries
- Distributes files across available primary servers
- Backup servers only serve during failover

## Testing Checklist

### Basic Functionality
- [ ] Start Name Server on port 8080
- [ ] Register Storage Server SS1 (primary)
- [ ] Register Storage Server SS2 (backup)
- [ ] Verify backup pairing notification sent to SS1
- [ ] Client requests file location (should get SS1)
- [ ] Create file through Name Server
- [ ] Delete file through Name Server

### Failover Testing
- [ ] Kill primary server (SS1)
- [ ] Client requests file location (should get SS2)
- [ ] Verify SS2 marked as acting primary
- [ ] Restart SS1, verify restored as primary

### Load Balancing
- [ ] Register multiple primary servers (SS1, SS3, SS5)
- [ ] Create multiple files
- [ ] Verify files distributed across primaries

### Access Control
- [ ] Add user access to file
- [ ] Remove user access from file
- [ ] Verify access propagated to Storage Servers

## Usage Instructions

### Starting Name Server
```bash
cd name_server
make clean && make
./name_server
```
Name Server listens on port 8080 by default.

### Integration with Storage Servers
Storage Servers should register with Name Server on startup:
```bash
# Primary server
./storage_server 1 9001 9002 ./storage_data1

# Backup server  
./storage_server 2 9003 9004 ./storage_data2
```

### Client Integration
Clients should connect to Name Server at 127.0.0.1:8080 for file location requests.

## Architecture Notes

### Message Flow Examples

#### Storage Server Registration
```
SS1 → NM: OP_SS_REGISTER (ss_id=1, ip, ports, file_list)
NM: Register SS1 as primary
NM: Look for backup partner (SS2)
SS2 → NM: OP_SS_REGISTER (ss_id=2, ip, ports, file_list)  
NM: Register SS2 as backup for SS1
NM → SS1: OP_NM_BACKUP_INFO (backup_ip, backup_port)
```

#### Client File Access
```
Client → NM: OP_GET_SS_INFO (filename="test.txt")
NM: Lookup test.txt location (found on SS1)
NM: Check SS1 health (alive)
NM → Client: RESPONSE (ip=SS1_IP, port=SS1_CLIENT_PORT)
Client → SS1: Direct connection for file operations
```

#### File Creation
```
Client → NM: OP_CREATE (filename, owner)
NM: Select primary server (SS1) via load balancing
NM → SS1: OP_SS_CREATE_FILE (filename, owner)
SS1: Create file locally and replicate to backup
SS1 → NM: ACK (success)
NM: Add file location to tracking
NM → Client: ACK (success)
```

## Future Enhancements
- [ ] Persistent storage of server/file mappings
- [ ] Advanced load balancing algorithms
- [ ] Server health metrics and monitoring
- [ ] Automatic server discovery
- [ ] Replication factor configuration
- [ ] Geographic distribution support

## Debugging Tips
- Check server logs for registration messages
- Verify backup pairing with correct IDs
- Monitor heartbeat messages for failover testing
- Use network tools to verify port connectivity
- Check file location mappings after operations

---
**Status**: ✅ IMPLEMENTATION COMPLETE  
**Last Updated**: November 7, 2025  
**Next Step**: Integration testing with Storage Servers and Clients
