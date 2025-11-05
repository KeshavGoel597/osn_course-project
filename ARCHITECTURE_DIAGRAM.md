# Backup & Replication Architecture Diagram

## System Overview

```
┌──────────────────────────────────────────────────────────────────┐
│                         Name Server (NM)                          │
│  • Tracks file locations                                         │
│  • Maintains SS_ID → IP/Port mapping                            │
│  • Provides backup server info to primaries                     │
│  • Implements failover (redirects to backup if primary down)    │
└───────┬──────────────────────────────────────────────────┬───────┘
        │                                                   │
        │ Registration (SS_ID, IP, Ports, File List)      │
        │                                                   │
┌───────▼─────────────────────┐                 ┌──────────▼────────────────────┐
│  Storage Server 1 (SS1)     │◄───────────────►│  Storage Server 2 (SS2)       │
│  • SS_ID = 1 (PRIMARY)      │  Replication    │  • SS_ID = 2 (BACKUP for SS1) │
│  • NM Port: 9001            │  Connection     │  • NM Port: 9003               │
│  • Client Port: 9002        │                 │  • Client Port: 9004           │
│  • Storage: ./storage_data1 │                 │  • Storage: ./storage_data2    │
└───────┬─────────────────────┘                 └──────────┬────────────────────┘
        │                                                   │
        │ Direct operations                                │ Backup requests
        │ (CREATE, READ, WRITE, DELETE)                   │ (OP_BACKUP_*)
        │                                                   │
┌───────▼────────────────────────────────────────────────────────────────────┐
│                              Clients                                        │
│  • Client 1 (Alice)    • Client 2 (Bob)    • Client 3 (Carol)            │
│  • Interactive shell with commands: CREATE, READ, WRITE, DELETE, etc.     │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Server Pairing Strategy

```
Primary Servers (Odd IDs)          Backup Servers (Even IDs)
─────────────────────────          ──────────────────────────

SS1 (ID=1) ◄──────────────────────► SS2 (ID=2)
  Port: 9001/9002                      Port: 9003/9004
  Storage: ./storage_data1             Storage: ./storage_data2

SS3 (ID=3) ◄──────────────────────► SS4 (ID=4)
  Port: 9005/9006                      Port: 9007/9008
  Storage: ./storage_data3             Storage: ./storage_data4

SS5 (ID=5) ◄──────────────────────► SS6 (ID=6)
  Port: 9009/9010                      Port: 9011/9012
  Storage: ./storage_data5             Storage: ./storage_data6

...                                  ...
```

## File Creation Flow

```
┌────────┐
│ Client │
└───┬────┘
    │ 1. CREATE file.txt
    ▼
┌───────────┐
│ Name      │
│ Server    │
└───┬───────┘
    │ 2. OP_SS_CREATE_FILE
    ▼
┌────────────────┐
│ SS1 (Primary)  │
│ ────────────── │
│ create_file_ll │
└────┬───────────┘
     │ 3. OP_BACKUP_CREATE
     │    + filename
     │    + owner
     ▼
┌────────────────┐
│ SS2 (Backup)   │
│ ────────────── │
│ create_file_ll │
└────┬───────────┘
     │ 4. ACK
     ▼
┌────────────────┐
│ SS1 (Primary)  │
│ ────────────── │
│ send_file_to_  │
│ backup()       │
└────┬───────────┘
     │ 5. File content
     │    (in chunks)
     ▼
┌────────────────┐
│ SS2 (Backup)   │
│ ────────────── │
│ receive_file_  │
│ from_primary() │
└────┬───────────┘
     │ 6. Final ACK
     ▼
┌────────────────┐
│ SS1 (Primary)  │
└────┬───────────┘
     │ 7. Success
     ▼
┌───────────┐
│ Name      │
│ Server    │
└───┬───────┘
    │ 8. File created
    ▼
┌────────┐
│ Client │
└────────┘
```

## File Write Flow

```
┌────────┐
│ Client │───────1. WRITE file.txt S1 W2 "Hello"────────►┌────────────────┐
└────────┘                                                │ SS1 (Primary)  │
                                                          │ ────────────── │
                                                          │ • Lock sentence│
                                                          │ • Write word   │
                                                          └────┬───────────┘
                                                               │
┌────────┐                                                     │
│ Client │◄───────2. LOCKED──────────────────────────────────┤
└───┬────┘                                                     │
    │ 3. Word insertions...                                   │
    │ 4. ETIRW (end write)                                    │
    └────────────────────────────────────────────────────────►│
                                                               │
                                      5. OP_BACKUP_SYNC        │
                                         + filename            │
                                                               ▼
                                                          ┌────────────────┐
                                                          │ SS2 (Backup)   │
                                                          │ ────────────── │
                                                          │ • Delete old   │
                                                          │ • Receive new  │
                                                          └────┬───────────┘
                                                               │
                                      6. File content          │
                                         (full file)           │
                                                               ▼
                                                          ┌────────────────┐
                                                          │ SS2 (Backup)   │
                                                          │ ────────────── │
                                                          │ • Save to disk │
                                                          └────┬───────────┘
                                                               │
                                      7. ACK                   │
                                                               ▼
┌────────┐                                                ┌────────────────┐
│ Client │◄────8. Write Successful!─────────────────────│ SS1 (Primary)  │
└────────┘                                                └────────────────┘
```

## Failover Scenario

### Normal Operation (Primary Available)
```
┌────────┐                           ┌────────────────┐
│ Client │─────READ file.txt────────►│ SS1 (Primary)  │
│        │◄────File content──────────│ ● ALIVE        │
└────────┘                           └────────────────┘

                                      ┌────────────────┐
                                      │ SS2 (Backup)   │
                                      │ ○ Idle         │
                                      └────────────────┘
```

### Failover (Primary Down)
```
┌────────┐      1. READ file.txt     ┌───────────┐
│ Client │──────────────────────────►│ Name      │
│        │                            │ Server    │
│        │◄───2. SS2 info─────────────│           │
└───┬────┘    (failover)             └───────────┘
    │
    │ 3. READ file.txt                ┌────────────────┐
    │                                 │ SS1 (Primary)  │
    │                                 │ ✗ DOWN         │
    │                                 └────────────────┘
    │
    └────────────────────────────────►┌────────────────┐
           4. READ file.txt           │ SS2 (Backup)   │
                                      │ ● SERVING      │
      ◄────5. File content────────────│                │
                                      └────────────────┘
```

## Data Structure (In-Memory Cache)

### Primary Server (SS1)
```
File Cache (Linked List)
│
├─► LoadedFile: "file1.txt"
│   ├── sentences_head ─► Sentence[0] ─► Sentence[1] ─► NULL
│   ├── sentence_count: 2
│   ├── is_loaded: 1
│   └── file_rwlock: <locked>
│
├─► LoadedFile: "file2.txt"
│   ├── sentences_head ─► Sentence[0] ─► Sentence[1] ─► Sentence[2] ─► NULL
│   ├── sentence_count: 3
│   ├── is_loaded: 1
│   └── file_rwlock: <unlocked>
│
└─► NULL
```

### Backup Server (SS2) - Same Structure
```
File Cache (Linked List)
│
├─► LoadedFile: "file1.txt"  ← REPLICATED from SS1
│   ├── sentences_head ─► Sentence[0] ─► Sentence[1] ─► NULL
│   ├── sentence_count: 2
│   ├── is_loaded: 1
│   └── file_rwlock: <locked>
│
├─► LoadedFile: "file2.txt"  ← REPLICATED from SS1
│   ├── sentences_head ─► Sentence[0] ─► Sentence[1] ─► Sentence[2] ─► NULL
│   ├── sentence_count: 3
│   ├── is_loaded: 1
│   └── file_rwlock: <unlocked>
│
└─► NULL
```

## Replication Operations

### OP_BACKUP_CREATE
```
┌──────────────┐    ┌──────────────────────────┐    ┌──────────────┐
│ Primary SS1  │───►│ Message                  │───►│ Backup SS2   │
│              │    │ msg_type: MSG_REQUEST    │    │              │
│              │    │ operation: OP_BACKUP_    │    │ Create file  │
│              │    │            CREATE         │    │ locally      │
│              │    │ filename: "file.txt"     │    │              │
│              │    │ username: "alice"        │    │              │
│              │    └──────────────────────────┘    │              │
│              │◄──── ACK ──────────────────────────│              │
│ Send file    │                                    │              │
│ content      │──── File chunks ───────────────────►│ Receive &   │
│              │                                    │ save         │
└──────────────┘                                    └──────────────┘
```

### OP_BACKUP_SYNC
```
┌──────────────┐    ┌──────────────────────────┐    ┌──────────────┐
│ Primary SS1  │───►│ Message                  │───►│ Backup SS2   │
│              │    │ msg_type: MSG_REQUEST    │    │              │
│ File was     │    │ operation: OP_BACKUP_    │    │ Delete old   │
│ modified     │    │            SYNC          │    │ version      │
│              │    │ filename: "file.txt"     │    │              │
│              │    └──────────────────────────┘    │              │
│              │◄──── ACK ──────────────────────────│              │
│ Send updated │                                    │              │
│ content      │──── Full file ─────────────────────►│ Save new    │
│              │                                    │ version      │
│              │◄──── Final ACK ────────────────────│              │
└──────────────┘                                    └──────────────┘
```

### OP_BACKUP_DELETE
```
┌──────────────┐    ┌──────────────────────────┐    ┌──────────────┐
│ Primary SS1  │───►│ Message                  │───►│ Backup SS2   │
│              │    │ msg_type: MSG_REQUEST    │    │              │
│ Delete file  │    │ operation: OP_BACKUP_    │    │ Delete file  │
│ locally      │    │            DELETE        │    │ locally      │
│              │    │ filename: "file.txt"     │    │              │
│              │    └──────────────────────────┘    │              │
│              │◄──── ACK ──────────────────────────│              │
└──────────────┘                                    └──────────────┘
```

## Thread Safety

### Backup Handler Mutex
```
pthread_mutex_t backup_mutex
│
├─► Protects: server_config.backup_sockfd
├─► Locked during: connect, replicate_create, replicate_delete, replicate_sync
└─► Ensures: Only one replication operation at a time
```

### File Cache Mutex (from file_handler_ll)
```
pthread_mutex_t file_cache_mutex
│
├─► Protects: file_cache linked list
├─► Locked during: get_file_from_cache, load_file_into_memory
└─► Ensures: Thread-safe cache operations
```

### Sentence Lock (embedded in SentenceNode)
```
pthread_mutex_t sentence_lock (per sentence)
│
├─► Protects: sentence content
├─► Locked during: WRITE operation
└─► Enables: Concurrent editing of different sentences
```

## Disk Layout

### Primary Server (SS1)
```
./storage_data1/
├── files/
│   ├── file1.txt    ← Original files
│   ├── file2.txt
│   └── file3.txt
├── undo/
│   ├── file1.txt    ← Backup for UNDO
│   └── file2.txt
└── metadata.txt     ← File metadata
```

### Backup Server (SS2)
```
./storage_data2/
├── files/
│   ├── file1.txt    ← Replicated from SS1
│   ├── file2.txt
│   └── file3.txt
├── undo/
│   ├── file1.txt    ← Replicated from SS1
│   └── file2.txt
└── metadata.txt     ← Replicated from SS1
```

## Summary

- ✅ **Primary servers** handle all client operations
- ✅ **Backup servers** replicate data from primary
- ✅ **Synchronous replication** ensures consistency
- ✅ **Failover support** via Name Server redirection
- ✅ **Thread-safe** with mutex protection
- ✅ **Scalable** with linked list file representation

---

This architecture ensures **data redundancy**, **high availability**, and **fault tolerance** in the distributed file system.
