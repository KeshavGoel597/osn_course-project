# Distributed File System Testing Guide

## Overview
This guide provides comprehensive testing instructions for the distributed file system with Name Server, Storage Servers, and Clients.

## System Architecture
```
┌─────────────┐    ┌──────────────┐    ┌─────────────────┐
│   Client    │────│ Name Server  │────│ Storage Server  │
│  (Port varies)   │  (Port 8080) │    │ (Ports 9001+)   │
└─────────────┘    └──────────────┘    └─────────────────┘
                          │                      │
                          └──────────────────────┘
                               Backup Pairing
```

## Prerequisites

### 1. Build All Components
```bash
# Build Name Server
cd name_server
make clean && make
cd ..

# Build Storage Server
cd "storage server"
make clean && make
cd ..

# Build Client
cd client
make clean && make
cd ..
```

### 2. Create Storage Directories
```bash
mkdir -p storage_data1 storage_data2 storage_data3 storage_data4
mkdir -p test_files
```

## Testing Scenarios

## 1. Basic System Setup Test

### Terminal 1: Start Name Server
```bash
cd name_server
./name_server
```
**Expected Output:**
```
Name Server starting on port 8080...
Waiting for connections...
```

### Terminal 2: Start Primary Storage Server (SS1)
```bash
cd "storage server"
./storage_server 1 9001 9002 ../storage_data1
```
**Expected Output:**
```
Storage Server 1 starting...
Client port: 9001, NM port: 9002
Registering with Name Server...
Registration successful
```

### Terminal 3: Start Backup Storage Server (SS2)
```bash
cd "storage server" 
./storage_server 2 9003 9004 ../storage_data2
```
**Expected Output:**
```
Storage Server 2 starting...
Client port: 9003, NM port: 9004
Registering with Name Server...
Registration successful
Backup pairing established with SS1
```

### Terminal 4: Start Client
```bash
cd client
./client
```
**Expected Output:**
```
Enter your username: testuser
Connected to Name Server
DFS> 
```

## 2. File Operations Testing

### Test File Creation
```bash
DFS> CREATE test.txt testuser
```
**Expected:** File created successfully on primary server (SS1) and replicated to backup (SS2)

### Test File Writing
```bash
DFS> WRITE test.txt
Enter content: Hello, this is a test file!
```
**Expected:** Content written to file

### Test File Reading
```bash
DFS> READ test.txt
```
**Expected:** Display file content: "Hello, this is a test file!"

### Test File Viewing
```bash
DFS> VIEW test.txt
```
**Expected:** Display file with metadata (size, permissions, etc.)

### Test File Information
```bash
DFS> INFO test.txt
```
**Expected:** Display detailed file information

## 3. Directory Operations Testing

### Test Directory Listing
```bash
DFS> LIST
```
**Expected:** List all accessible files

### Test Directory Creation
```bash
DFS> CREATE mydir/ testuser
```
**Expected:** Directory created successfully

## 4. Access Control Testing

### Add Read Access for Another User
```bash
DFS> ADDACCESS test.txt otheruser READ
```
**Expected:** Read access granted to otheruser

### Add Write Access for Another User
```bash
DFS> ADDACCESS test.txt otheruser WRITE
```
**Expected:** Write access granted to otheruser

### Remove Access
```bash
DFS> REMACCESS test.txt otheruser
```
**Expected:** Access removed for otheruser

## 5. Failover Testing

### Test Primary Server Failure
1. **Kill Primary Server (SS1):**
   ```bash
   # In SS1 terminal, press Ctrl+C
   ```

2. **Test Client Operations:**
   ```bash
   DFS> READ test.txt
   ```
   **Expected:** File served from backup server (SS2)

3. **Restart Primary Server:**
   ```bash
   cd "storage server"
   ./storage_server 1 9001 9002 ../storage_data1
   ```
   **Expected:** Primary server restored, backup pairing re-established

## 6. Load Balancing Testing

### Start Additional Servers
```bash
# Terminal 5: SS3 (Primary)
cd "storage server"
./storage_server 3 9005 9006 ../storage_data3

# Terminal 6: SS4 (Backup for SS3)
cd "storage server" 
./storage_server 4 9007 9008 ../storage_data4
```

### Create Multiple Files
```bash
DFS> CREATE file1.txt testuser
DFS> CREATE file2.txt testuser  
DFS> CREATE file3.txt testuser
DFS> CREATE file4.txt testuser
```
**Expected:** Files distributed across primary servers (SS1, SS3)

## 7. Stress Testing

### Multiple Clients Test
Start multiple client instances and perform concurrent operations:

```bash
# Terminal 7: Client 2
cd client
./client
# Username: user2

# Terminal 8: Client 3  
cd client
./client
# Username: user3
```

### Concurrent Operations
Perform simultaneous operations from different clients:
- File creation
- File reading/writing
- Access control changes

## 8. Error Handling Testing

### Test Invalid Commands
```bash
DFS> INVALID_COMMAND
```
**Expected:** "Unknown command" error message

### Test Non-existent File Operations
```bash
DFS> READ nonexistent.txt
```
**Expected:** "File not found" error

### Test Permission Violations
```bash
# As different user, try to access restricted file
DFS> READ restricted_file.txt
```
**Expected:** "Access denied" error

## 9. Advanced Features Testing

### Test File Streaming
```bash
DFS> STREAM test.txt
```
**Expected:** Stream file content in real-time

### Test Command Execution (if implemented)
```bash
DFS> EXEC ls -la
```
**Expected:** Execute command on storage server

### Test Undo Operations (if implemented)
```bash
DFS> UNDO
```
**Expected:** Undo last operation

## 10. System Monitoring

### Check Server Status
Monitor Name Server output for:
- Storage Server registrations
- Backup pairing notifications
- Client connection logs
- Failover events

### Check Storage Server Status
Monitor Storage Server output for:
- File operation logs
- Backup synchronization
- Client request handling

## Common Test Commands Summary

```bash
# Basic file operations
CREATE filename.txt username
WRITE filename.txt
READ filename.txt
VIEW filename.txt
DELETE filename.txt
INFO filename.txt

# Directory operations
LIST
CREATE dirname/ username

# Access control
ADDACCESS filename.txt username READ
ADDACCESS filename.txt username WRITE
REMACCESS filename.txt username

# System commands
HELP
EXIT
```

## Expected Behavior Summary

### ✅ Successful Operations Should Show:
- Clear success messages
- Proper file content display
- Correct access control enforcement
- Seamless failover handling
- Load balanced file distribution

### ❌ Error Cases Should Show:
- Informative error messages
- Graceful degradation
- No system crashes
- Proper cleanup

## Troubleshooting Tips

### Connection Issues
- Verify all servers are running
- Check port availability (8080, 9001-9008)
- Ensure firewall allows connections

### File Operation Issues
- Check file permissions
- Verify storage directories exist
- Ensure sufficient disk space

### Backup/Failover Issues
- Verify backup pairing messages
- Check server registration logs
- Test heartbeat mechanisms

## Performance Benchmarks

### Expected Response Times
- File creation: < 100ms
- File reading: < 50ms
- Failover: < 200ms
- Access control updates: < 100ms

### Scalability Targets
- Support 10+ concurrent clients
- Handle 100+ files per server
- Maintain < 1% data loss during failover

---

**Last Updated:** November 7, 2025  
**Status:** Ready for comprehensive testing
