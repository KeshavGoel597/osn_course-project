# OSN Course Project - Distributed File System

## Project Overview
A distributed file system with collaborative document viewing and editing capabilities.

## Architecture
- **Name Server (NM)**: Control plane - manages file locations and routing
- **Storage Servers (SS)**: Data plane - stores files and handles client requests
- **Clients**: User interface for file operations

## Key Features

### ✅ Implemented (Storage Server & Client)
1. **Scalable File Storage**: Linked list-based file representation (handles 100k+ lines)
2. **Concurrent Editing**: Sentence-level locking for true parallel editing
3. **Backup & Replication**: Each file stored on 2 servers (primary + backup)
4. **CRUD Operations**: Create, Read, Write, Delete files
5. **Access Control**: Read/Write permissions per user
6. **STREAM Mode**: Word-by-word file viewing
7. **UNDO Functionality**: Revert to previous file state
8. **Metadata Management**: File owner, size, timestamps, access lists

### Data Plane (Your Implementation)
- **Storage Server**: Handles file storage with backup replication
- **Client**: Interactive shell for file operations
- **Status**: ✅ Compiled and ready for testing

### Control Plane (Teammate's Implementation)
- **Name Server**: File location tracking, load balancing, failover
- **Status**: ⏳ In progress (teammate)

## Quick Start

### Build Storage Server
```bash
cd "storage server"
make clean && make
```

### Run Storage Server
```bash
# Primary Server (SS1)
./storage_server 1 9001 9002 ./storage_data1

# Backup Server (SS2) - in another terminal
./storage_server 2 9003 9004 ./storage_data2
```

### Build Client
```bash
cd client
make clean && make
```

### Run Client
```bash
./client <username>
```

## Documentation

### Storage Server
- **LINKED_LIST_IMPLEMENTATION.md**: Scalable file handler design
- **INTEGRATION_COMPLETE.md**: Integration of linked list into storage server
- **BACKUP_REPLICATION.md**: Backup and replication architecture
- **BACKUP_SUMMARY.md**: Quick summary of backup functionality

### Project Specifications
- **Course Project_short.pdf**: Original project requirements

## Backup & Replication

### How It Works
- **Odd servers (SS1, SS3, SS5)**: PRIMARY servers
- **Even servers (SS2, SS4, SS6)**: BACKUP servers
- **Pairing**: SS2 backs up SS1, SS4 backs up SS3, etc.
- **Synchronous replication**: All operations replicated to backup

### Commands
```bash
# Start primary server
make run

# Start backup server
make run-backup
```

## File Operations

### Supported Commands (Client)
```
CREATE <filename>           - Create new file
READ <filename>             - Read entire file
WRITE <filename> <sent> <word> - Write to specific location
DELETE <filename>           - Delete file
STREAM <filename>           - View file word-by-word
INFO <filename>             - Get file metadata
LIST                        - List all files
ADDACCESS <filename> <user> <R|W|RW> - Grant access
REMACCESS <filename> <user> - Revoke access
UNDO <filename>             - Undo last change
EXEC <filename>             - Execute file (if executable)
```

## Project Structure
```
osn_course-project/
├── common/                  # Shared protocol and utilities
│   ├── protocol.h          # Message structures and operations
│   └── network_utils.c/h   # Socket helper functions
├── storage server/          # Data plane (YOUR WORK)
│   ├── storage_server.c    # Main server with threading
│   ├── file_handler_ll.c   # Linked list file operations
│   ├── file_write_ll.c     # Complex write logic
│   ├── backup_handler.c    # Backup replication
│   ├── ss_nm_comm.c        # Name Server communication
│   ├── ss_client_comm.c    # Client request handling
│   └── Makefile
├── client/                  # User interface (YOUR WORK)
│   ├── client.c            # Main client with shell
│   ├── command_parser.c    # Command parsing
│   ├── client_nm_comm.c    # Name Server communication
│   ├── client_ss_comm.c    # Storage Server communication
│   └── Makefile
└── README.md               # This file
```

## Technical Highlights

### Scalability
- No fixed size limits (handles multi-GB files)
- Lazy loading with in-memory cache
- Efficient linked list traversal

### Concurrency
- Sentence-level pthread mutexes
- File-level reader-writer locks
- Thread-safe cache with double-check locking

### Reliability
- Atomic writes via swap file pattern
- Backup replication for redundancy
- Graceful error handling

### Delimiters
- Auto-detects sentence boundaries: `.`, `!`, `?`
- Splits sentences automatically during write

## Team Roles
- **Data Plane Developer (You)**: Storage Server + Client ✅
- **Control Plane Developer (Teammate)**: Name Server ⏳

## Next Steps
1. ✅ Implement backup replication - **DONE**
2. ⏳ Wait for Name Server implementation
3. ⏳ Integration testing (NM + SS + Client)
4. ⏳ Failover testing (primary down, backup serves)
5. ⏳ Performance benchmarking

## Notes
- All code is POSIX-compliant C
- Tested on Ubuntu Linux
- Uses TCP sockets for all communication
- pthread for threading and synchronization