# Fault Tolerance Testing Guide

## Overview

This guide provides step-by-step instructions for testing all fault tolerance features in the distributed file system.

## Prerequisites

1. Build all components:
```bash
# Build Name Server
cd name_server
make clean && make
cd ..

# Build Storage Servers
cd storage_server
make clean && make
cd ..

# Build Client
cd client
make clean && make
cd ..
```

## Test Environment Setup

### Terminal Layout
Open 6 terminals:
- Terminal 1: Name Server
- Terminal 2: Storage Server 1 (Primary)
- Terminal 3: Storage Server 2 (Backup)
- Terminal 4: Storage Server 3 (Primary) 
- Terminal 5: Storage Server 4 (Backup)
- Terminal 6: Client

## Test Scenarios

### Test 1: Basic Replication Setup

#### Step 1: Start Name Server
```bash
# Terminal 1
cd name_server
./name_server
```

Expected output:
```
=== Name Server Initialization ===
[Hash Table] Initialized with 10007 buckets
[Fault Tolerance] Initializing enhanced replication system
[Fault Tolerance] Enhanced replication system initialized successfully
Name Server initialized successfully
Listening on port 8080
```

#### Step 2: Start Primary Storage Server (SS1)
```bash
# Terminal 2
cd storage_server
./storage_server 8081 8082 storage_data1
```

Expected output:
```
[Backup Handler] Initialized (SS_ID=1, PRIMARY)
[Async Replication] Worker thread started
Connected to Name Server at 127.0.0.1:8080
Registration acknowledged by Name Server
```

#### Step 3: Start Backup Storage Server (SS2)
```bash
# Terminal 3
cd storage_server
./storage_server 8083 8084 storage_data2
```

Expected output:
```
[Backup Handler] Initialized (SS_ID=2, BACKUP)
Connected to Name Server at 127.0.0.1:8080
Registration acknowledged by Name Server
Received backup info from NM
Successfully configured backup replication
```

#### Verification
Check Name Server output for:
```
[Fault Tolerance] Creating replication pair: Primary SS1 <-> Backup SS2
[Backup Pairing] SS1 (primary) paired with SS2 (backup)
```

### Test 2: Asynchronous Replication

#### Step 1: Connect Client
```bash
# Terminal 6
cd client
./client
```

#### Step 2: Register User and Create File
```
Enter command: register testuser
Enter command: create test_replication.txt
```

#### Verification
Check both SS1 and SS2 terminals for:
- SS1: `[NM Handler] CREATE FILE request: test_replication.txt`
- SS1: `[Async Replication] Enqueued async CREATE replication`
- SS2: Should receive and process the replication

#### Step 3: Write to File
```
Enter command: write test_replication.txt 1 1 "This is replicated data"
```

#### Verification
- Both storage servers should contain the file
- Check file exists in both `storage_data1/files/` and `storage_data2/files/`

### Test 3: Primary Server Failure and Failover

#### Step 1: Create Test Data
```
Enter command: create failover_test.txt
Enter command: write failover_test.txt 1 1 "Test failover"
```

#### Step 2: Simulate Primary Server Failure
Kill SS1 (Terminal 2):
```bash
Ctrl+C
```

#### Verification
Check Name Server output:
```
[Heartbeat Monitor] SS1 heartbeat timeout
[Failover] SS1 marked as OFFLINE
[Failover] Handling failure of primary server SS1
[Failover] SS2 promoted to acting primary for failed SS1
```

#### Step 3: Test File Access During Failover
```
Enter command: read failover_test.txt 1 1
```

Should still work as SS2 is now acting primary.

### Test 4: Storage Server Recovery

#### Step 1: Restart Failed Primary Server
```bash
# Terminal 2 (restart SS1)
./storage_server 8081 8082 storage_data1
```

#### Verification
Check Name Server output:
```
[Heartbeat] SS1 back online
[Fault Tolerance] Handling reconnection of SS1
[Fault Tolerance] Starting recovery sync: SS1 <- SS2
```

Check SS1 terminal:
```
[NM Handler] BACKUP_INIT_SYNC request from NM
[Bulk Sync] Starting bulk synchronization to backup server
```

#### Step 2: Verify Data Synchronization
After sync completes, check that files exist in both servers:
```bash
ls storage_data1/files/
ls storage_data2/files/
```

Both should contain `failover_test.txt`

### Test 5: Concurrent Operations During Recovery

#### Step 1: During Recovery, Create New File
While SS1 is recovering:
```
Enter command: create recovery_test.txt
Enter command: write recovery_test.txt 1 1 "Created during recovery"
```

#### Verification
- Operations should complete successfully
- File should be replicated to both servers after recovery completes

### Test 6: Multiple Server Failures

#### Step 1: Test Multiple Servers
Start SS3 and SS4:
```bash
# Terminal 4
cd storage_server
./storage_server 8085 8086 storage_data3

# Terminal 5  
cd storage_server
./storage_server 8087 8088 storage_data4
```

#### Step 2: Create Files on Different Primaries
```
Enter command: create file_on_ss1.txt
Enter command: create file_on_ss3.txt
```

#### Step 3: Simulate Multiple Failures
Kill both SS1 and SS3.

#### Verification
- SS2 should handle requests for files originally on SS1
- SS4 should handle requests for files originally on SS3
- System should remain operational

### Test 7: Network Partition Simulation

#### Step 1: Block Network to Storage Server
Use firewall rules or disconnect network to simulate partition.

#### Step 2: Verify Failover Behavior
- Name Server should detect timeout
- Backup should be promoted
- Client operations should continue

### Test 8: Data Consistency Verification

#### Step 1: Create and Modify Files
```
Enter command: create consistency_test.txt
Enter command: write consistency_test.txt 1 1 "Original content"
Enter command: write consistency_test.txt 1 2 "Modified content"
```

#### Step 2: Force Failover and Recovery
1. Kill primary server
2. Modify file through backup
3. Restart primary server
4. Verify both servers have same content

#### Verification
Compare files:
```bash
diff storage_data1/files/consistency_test.txt storage_data2/files/consistency_test.txt
```

Should show no differences.

## Test Results Validation

### Success Criteria

1. **Replication**: All write operations replicated within 5 seconds
2. **Failure Detection**: Server failures detected within 30 seconds  
3. **Failover**: Backup promotion within 10 seconds of failure detection
4. **Recovery**: Complete data sync within 2 minutes for typical workload
5. **Consistency**: No data loss or corruption during any failure scenario

### Performance Metrics

Monitor these metrics during testing:

1. **Client Response Time**: Should not increase due to replication
2. **Replication Lag**: Maximum 5 seconds for async replication
3. **Recovery Time**: Complete sync within reasonable time
4. **System Availability**: >99.9% during single server failures

### Troubleshooting

#### Common Issues

1. **Replication Not Working**
   - Check storage server logs for replication queue messages
   - Verify backup server is connected and responding

2. **Recovery Hangs**
   - Check network connectivity between servers
   - Look for timeout messages in logs

3. **Data Inconsistency**
   - Check file permissions and storage directory setup
   - Verify metadata.txt synchronization

#### Debug Commands

```bash
# Check storage server processes
ps aux | grep storage_server

# Monitor replication logs
tail -f name_server/name_server.log

# Check file replication status
ls -la storage_data*/files/
```

## Advanced Testing

### Stress Testing

1. **High Load**: Create 1000+ files rapidly
2. **Concurrent Clients**: Multiple clients accessing files simultaneously
3. **Rapid Failures**: Quick succession of server failures and recoveries

### Edge Cases

1. **Disk Full**: Test behavior when storage is full
2. **Corrupted Data**: Test recovery from corrupted files
3. **Clock Skew**: Test with different server times

## Conclusion

This comprehensive testing guide ensures all fault tolerance features work correctly under various failure scenarios. Regular testing using these procedures will validate the system's reliability and help identify any issues before production deployment.

### Test Summary Checklist

- [ ] Basic replication setup works
- [ ] Asynchronous replication functions properly
- [ ] Primary server failures trigger correct failover
- [ ] Storage server recovery synchronizes data
- [ ] Concurrent operations work during recovery
- [ ] Multiple server failures handled gracefully
- [ ] Data consistency maintained throughout
- [ ] Performance remains acceptable during all operations

Completing all tests successfully ensures the fault tolerance implementation meets enterprise reliability standards.
