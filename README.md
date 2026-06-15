# Distributed File System (DFS)

[![C99](https://img.shields.io/badge/Language-C99-blue.svg)](https://en.wikipedia.org/wiki/C99)
[![Build Status](https://img.shields.io/badge/Build-Passing-brightgreen.svg)]()
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20macOS-lightgrey.svg)]()

A robust, multi-threaded Network File System (NFS) written in C. This project implements a decoupled architecture separating metadata management from actual data storage, allowing for scalable, concurrent, and secure file operations across multiple distributed storage servers.

> **Note:** All bonus features have been successfully implemented with the exception of fault tolerance.

## 🌟 Unique Feature

The client features a highly intuitive, interactive `HELP` command interface that maps out all available options and functionalities, significantly improving the developer and user experience.

---

## 📑 Table of Contents

* [System Architecture](#-system-architecture)
* [Key Features](#-key-features)
* [Prerequisites](#-prerequisites)
* [Build Instructions](#-build-instructions)
* [Getting Started](#-getting-started)
* [Command Reference](#-command-reference)
* [Bonus Implementations](#-bonus-implementations)
* [Troubleshooting](#-troubleshooting)
* [Detailed Documentation](#-detailed-documentation)

---

# 🏗 System Architecture

The system is built on three primary components:

### 1. Name Server (Central Coordinator)

* Manages metadata
* Handles client and storage server registrations
* Performs load balancing using round-robin scheduling
* Uses O(1) hash tables and LRU caches for efficient lookups

### 2. Storage Servers (Data Nodes)

* Store actual file data on disk
* Manage direct data transfers between clients and storage servers
* Handle fine-grained concurrency using sentence-level locking

### 3. Clients

* Interactive shell interface
* Execute file operations
* Route requests through the Name Server
* Connect directly to Storage Servers for heavy I/O transfers

For an in-depth technical breakdown of the network protocols, data structures, and threading models, refer to **ARCHITECTURE.md**.

---

# ✨ Key Features

## Decoupled Data & Metadata

Clients retrieve connection details from the Name Server and communicate directly with Storage Servers for data transfer, eliminating central bottlenecks.

## High Concurrency & Fine-Grained Locking

* POSIX reader-writer locks for files
* Mutexes for individual sentences
* Multiple users can edit different portions of the same file simultaneously

## Word-by-Word Streaming

Supports live streaming of file contents word-by-word over custom TCP protocols.

## Robust Access Control

File owners can:

* Grant access
* Revoke access
* Manage READ permissions
* Manage WRITE permissions

## Version Control & Checkpointing

Provides:

* File checkpoints
* Historical version viewing
* Reverting changes
* Dedicated `UNDO` command

---

# 🛠 Prerequisites

To compile and run this system, you will need:

* GCC Compiler (C99 compatible)
* POSIX Threads (`pthread`)
* GNU Make
* Linux, macOS, or WSL

---

# ⚙️ Build Instructions

The project uses a unified Makefile.

## Build Entire System

```bash
make all
```

## Build Individual Components

```bash
make nm       # Builds the Name Server
make ss       # Builds the Storage Server
make client   # Builds the Client
```

## Clean Build Artifacts

```bash
make clean
```

---

# 🚀 Getting Started

The components must be started in the following order:

1. Name Server
2. Storage Server(s)
3. Client(s)

## 1. Start the Name Server

Default port: `8080`

```bash
make run-nm
```

Or:

```bash
cd name_server
./name_server
```

---

## 2. Start Storage Servers

Example:

```bash
make run-ss1
make run-ss2
```

These start:

* SS1 on ports 9001 / 9002
* SS2 on ports 9003 / 9004

Manual syntax:

```bash
./storage_server <SS_ID> <NM_IP> <NM_PORT> <CLIENT_PORT> <STORAGE_DIR>
```

---

## 3. Start the Client Shell

```bash
make run-client
```

Or:

```bash
cd client
./client 127.0.0.1
```

When prompted, enter a username to begin your session.

---

# 💻 Command Reference

You can type `HELP` at any time inside the client shell.

## File & Directory Operations

| Command      | Arguments            | Description                  |
| ------------ | -------------------- | ---------------------------- |
| VIEW         | `[-a] [-l] [-al]`    | List accessible files        |
| READ         | `<filename>`         | Read file contents           |
| CREATE       | `<filename>`         | Create a file                |
| WRITE        | `<filename> <sent#>` | Write to sentence            |
| DELETE       | `<filename>`         | Delete file                  |
| INFO         | `<filename>`         | Display metadata             |
| STREAM       | `<filename>`         | Stream contents word-by-word |
| UNDO         | `<filename>`         | Undo last write              |
| CREATEFOLDER | `<foldername>`       | Create directory             |
| VIEWFOLDER   | `<foldername>`       | View directory contents      |
| MOVE         | `<filename> <path>`  | Move file                    |
| EXEC         | `<filename>`         | Execute script               |

---

## Access Control

| Command        | Arguments                 | Description           |
| -------------- | ------------------------- | --------------------- |
| ADDACCESS      | `<file> <user> -R/-W/-RW` | Grant permissions     |
| REMACCESS      | `<file> <user>`           | Revoke permissions    |
| REQUESTACCESS  | `<file> -R/-W/-RW`        | Request permissions   |
| VIEWREQUESTS   | None                      | View pending requests |
| APPROVEREQUEST | `<request_id>`            | Approve request       |
| REJECTREQUEST  | `<request_id>`            | Reject request        |

---

## Versioning (Checkpoints)

| Command         | Arguments          | Description        |
| --------------- | ------------------ | ------------------ |
| CHECKPOINT      | `<filename> <tag>` | Save checkpoint    |
| VIEWCHECKPOINT  | `<filename> <tag>` | View checkpoint    |
| LISTCHECKPOINTS | `<filename>`       | List checkpoints   |
| REVERT          | `<filename> <tag>` | Restore checkpoint |

---

# 🏆 Bonus Implementations

### Interactive Help Command

Custom HELP interface providing guided command discovery.

### Access Control Workflow

Complete request → approval → permission grant pipeline.

Includes:

* `OP_REQUESTACCESS`
* `OP_APPROVEREQUEST`
* Permission enforcement

### Advanced Versioning

Supports:

* Checkpoints
* Tagged snapshots
* Revert operations
* Standard UNDO functionality

### Directory Support

Includes:

* CREATEFOLDER
* VIEWFOLDER
* MOVE

### Remote Execution (EXEC)

Secure chunked remote script execution on storage servers.

### Not Implemented

* Fault tolerance
* Data replication

---

# ⚠️ Troubleshooting

## Address Already in Use

The Name Server port may still be occupied.

```bash
lsof -ti:8080 | xargs kill -9
```

---

## Connection Refused

Verify:

* Name Server is running
* Correct IP address is used
* Storage Servers are registered

For local execution:

```text
127.0.0.1
```

---

## Sentence Locked

Another client is currently writing to the same sentence index.

Wait until the lock is released.

---

## Cross-Architecture Compatibility

The system uses:

```c
htonl()
ntohl()
```

for network byte-order normalization, ensuring compatibility across architectures such as x86 and ARM.

---

# 📚 Detailed Documentation

For a complete explanation of:

* Data structures
* Networking protocol
* Threading model
* Request/response lifecycle
* Internal algorithms

please refer to:

```text
ARCHITECTURE.md
```
