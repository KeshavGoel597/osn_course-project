# EXEC Command Implementation

## Overview
The EXEC command allows users with read access to execute file contents as shell commands. The execution happens on the **Name Server**, and the output is piped back to the client.

## Architecture Flow

```
Client -> NM: EXEC request with filename
NM -> SS: Request file content (OP_EXEC)
SS -> NM: Return file content
NM: Execute content as shell commands
NM -> Client: Return execution output
```

## Implementation Details

### 1. Client Side (`client/`)
- **Command Parser**: Parses `EXEC <filename>` command
- **Request Handler**: `send_exec_request()` in `client_nm_comm.c`
  - Sends OP_EXEC request to Name Server
  - Receives and displays execution output

### 2. Name Server Side (`name_server/`)
- **Request Handler**: `handle_exec()` in `client_handler.c`
  - Validates file exists
  - Checks user has read access (owner OR granted read access)
  - Requests file content from Storage Server
  - Creates temporary file with commands
  - Executes using `bash` and captures output
  - Returns output to client
  - Cleans up temporary file

### 3. Storage Server Side (`storage_server/`)
- **Content Provider**: Handles OP_EXEC in `ss_nm_comm.c`
  - Reads file content using linked list structure
  - Returns content to Name Server (does NOT execute)

## Security Features
1. **Access Control**: Only users with read access can execute files
   - File owner has automatic access
   - Users in access list with R or RW permissions can execute
2. **Execution Isolation**: Commands execute on Name Server in isolated process
3. **Output Capture**: All stdout and stderr captured and returned
4. **Cleanup**: Temporary execution files are automatically deleted

## Usage Examples

### Basic Execution
```bash
Client> EXEC script.txt
```

### Example Test File (test_commands.txt)
```bash
echo "Hello from executed file!"
date
whoami
ls -l /tmp
```

### Expected Output
```
Hello from executed file!
Mon Nov 10 15:30:45 UTC 2025
nm_user
total 12
-rw-r--r-- 1 root root  123 Nov 10 15:30 test.txt
```

## Error Handling

### File Not Found
```
Error: File 'nonexistent.txt' not found
```

### No Read Access
```
Error: No read access to file 'private.txt'
```

### Storage Server Unavailable
```
Error: Storage server not available
```

### Execution Failure
```
[Command execution failed with status 127]
```

## Code Locations

| Component | File | Function |
|-----------|------|----------|
| Client Request | `client/client_nm_comm.c` | `send_exec_request()` |
| Client Parser | `client/command_parser.c` | Parse `EXEC` command |
| NM Handler | `name_server/client_handler.c` | `handle_exec()` |
| NM Router | `name_server/name_server.c` | `case OP_EXEC` |
| SS Content Provider | `storage_server/ss_nm_comm.c` | `case OP_EXEC` |

## Testing Steps

1. **Start Name Server**
   ```bash
   cd name_server && ./name_server
   ```

2. **Start Storage Server**
   ```bash
   cd storage_server && ./storage_server storage_data1 9001 9002
   ```

3. **Start Client**
   ```bash
   cd client && ./client user1
   ```

4. **Create Test File**
   ```bash
   Client> CREATE test_script.txt
   Client> WRITE test_script.txt 0 0 echo "Test Execution"
   Client> WRITE test_script.txt 1 0 date
   Client> WRITE test_script.txt 2 0 echo "Done"
   ```

5. **Execute File**
   ```bash
   Client> EXEC test_script.txt
   Test Execution
   Mon Nov 10 15:30:45 UTC 2025
   Done
   ```

6. **Test Access Control**
   ```bash
   # As user2 (no access)
   Client> EXEC test_script.txt
   Error: No read access to file 'test_script.txt'
   
   # Grant access
   # As user1 (owner)
   Client> ADDACCESS test_script.txt user2 R
   
   # As user2 (with access)
   Client> EXEC test_script.txt
   Test Execution
   [output appears]
   ```

## Limitations

1. **Output Size**: Limited to MAX_DATA_SIZE (4096 bytes)
   - Longer outputs are truncated
2. **Execution Time**: Long-running commands may timeout
3. **Command Scope**: Executes in Name Server's environment
   - Has Name Server's permissions and PATH
4. **No Interactive Input**: Commands requiring user input will fail
5. **Shell**: Always uses `bash` for execution

## Future Enhancements

1. Support for execution timeouts
2. Async execution for long-running commands
3. Streaming output for large results
4. Execution history/audit log
5. Configurable shell interpreter
6. Resource limits (CPU, memory)

## Implementation Status

✅ Client command parsing  
✅ Client request handler  
✅ Name Server EXEC handler  
✅ Storage Server content provider  
✅ Access control validation  
✅ Command execution on NM  
✅ Output capture and return  
✅ Error handling  
✅ Temporary file cleanup  

## Notes

- The execution happens **synchronously** on the Name Server
- The Name Server creates a temporary file in `/tmp/` for execution
- All commands are executed with Name Server's user privileges
- The temporary file is cleaned up after execution completes
- Exit status of commands is captured but not prominently displayed
