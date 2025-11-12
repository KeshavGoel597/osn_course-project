# Quick Testing Guide - Distributed File System

## 🚀 Quick Start (4 Terminals Required)

### Terminal 1: Name Server
```bash
cd /home/keshav-goel/Desktop/OSN/osn_course-project/name_server
./name_server
```
**Expected Output:**
```
=== Name Server (Control Plane) ===
Name Server initialized successfully
Listening on port 8080
Name Server is running...
```

---

### Terminal 2: Primary Storage Server (SS1)
```bash
cd /home/keshav-goel/Desktop/OSN/osn_course-project/storage_server
./storage_server 1 9001 9002 ./storage_data1
```
**Expected Output:**
```
Storage Server 1 starting...
Successfully registered with Name Server
=== Storage Server is running ===
```

---

### Terminal 3: Backup Storage Server (SS2)
```bash
cd /home/keshav-goel/Desktop/OSN/osn_course-project/storage_server
./storage_server 2 9003 9004 ./storage_data2
```
**Expected Output:**
```
Storage Server 2 starting...
Successfully registered with Name Server
Backup pairing established with SS1
=== Storage Server is running ===
```

---

### Terminal 4: Client (user1)
```bash
cd /home/keshav-goel/Desktop/OSN/osn_course-project/client
./client user1
```
**Expected Output:**
```
=== Client Initialization ===
Connected to Name Server
=== Distributed File System Client ===
User: user1
Type 'HELP' for available commands, 'EXIT' to quit.

user1>
```

---

## 📝 Test Sequence (Copy & Paste)

### Test 1: Create and Write to File
```
CREATE test.txt
WRITE test.txt 0
1 Hello world.
2 This is a test.
ETIRW
READ test.txt
```
**Expected Output:**
```
File Created Successfully!
Sentence 0 locked. Enter write commands:
Write Successful!
Hello world. This is a test.
```

---

### Test 2: View Files
```
VIEW
VIEW -l
```
**Expected Output for VIEW:**
```
--> test.txt
```

**Expected Output for VIEW -l:**
```
---------------------------------------------------------
| Filename     | Words   | Chars   | Last Access      | Owner   |
|--------------|---------|---------|------------------|---------|
| test.txt     |       6 |      28 | 2025-11-12 14:32 | user1   |
---------------------------------------------------------
```

---

### Test 3: File Info
```
INFO test.txt
```
**Expected Output:**
```
File: test.txt
Owner: user1
Created: 2025-11-12 14:30:00
Last Modified: 2025-11-12 14:31:00
Size: 28 bytes
Words: 6
Characters: 28
Access: user1 (RW)
Last Accessed: 2025-11-12 14:32 by user1
```

---

### Test 4: List Users (Open 2nd Client in Terminal 5)

**Terminal 5:**
```bash
cd /home/keshav-goel/Desktop/OSN/osn_course-project/client
./client user2
```

**Then in Terminal 4 (user1):**
```
LIST
```
**Expected Output:**
```
--> user1
--> user2
```

---

### Test 5: Access Control

**As user1:**
```
ADDACCESS -R test.txt user2
INFO test.txt
```
**Expected Output:**
```
Access granted successfully!
...
Access: user1 (RW), user2 (R)
...
```

**As user2 (Terminal 5):**
```
READ test.txt
```
**Expected Output:**
```
Hello world. This is a test.
```

**Try to write (should fail):**
```
WRITE test.txt 0
```
**Expected Output:**
```
Error: No write access to file 'test.txt'
```

**Grant write access (as user1 in Terminal 4):**
```
ADDACCESS -W test.txt user2
```

**Now write (as user2 in Terminal 5):**
```
WRITE test.txt 1
1 How are you?
ETIRW
READ test.txt
```
**Expected Output:**
```
Sentence 1 locked. Enter write commands:
Write Successful!
Hello world. This is a test. How are you?
```

---

### Test 6: Stream Mode
```
STREAM test.txt
```
**Expected Output:** (with 0.1s delay between words)
```
Hello world. This is a test. How are you?
--- End of stream ---
```

---

### Test 7: UNDO
```
UNDO test.txt
READ test.txt
```
**Expected Output:**
```
Undo Successful!
Hello world. This is a test.
```

---

### Test 8: EXEC Command

**Create executable file:**
```
CREATE commands.txt
WRITE commands.txt 0
1 echo "Running diagnostics..."
2 ls
3 echo "Done!"
ETIRW
EXEC commands.txt
```
**Expected Output:**
```
Running diagnostics...
test.txt
commands.txt
Done!
```

---

### Test 9: DELETE
```
DELETE commands.txt
VIEW
```
**Expected Output:**
```
File 'commands.txt' deleted successfully!
--> test.txt
```

---

### Test 10: Concurrent Editing

**Terminal 4 (user1):**
```
WRITE test.txt 0
(Don't send ETIRW yet)
```

**Terminal 5 (user2):**
```
WRITE test.txt 0
```
**Expected Output:**
```
Error: Sentence 0 is currently locked by another user
```

**Back to Terminal 4:**
```
1 Modified by user1.
ETIRW
```

**Terminal 5 can now write:**
```
WRITE test.txt 0
```

---

## 🎯 Success Criteria

✅ All commands execute without errors  
✅ Files are created and persisted  
✅ Access control is enforced  
✅ Concurrent locking works correctly  
✅ Backup replication happens (check storage_data2 folder)  
✅ Metadata timestamps update correctly  
✅ View/List/Info show proper formatting  

---

## 🐛 Common Issues & Solutions

### Issue: "Failed to connect to Name Server"
**Solution:** Make sure Name Server is running in Terminal 1

### Issue: "No available storage servers"
**Solution:** Start at least one storage server (Terminal 2)

### Issue: "File not found"
**Solution:** Use CREATE command before trying to read/write

### Issue: Command not recognized
**Solution:** Check spelling, commands are case-sensitive (use uppercase)

---

## 📊 Verify Backup Replication

After creating files in Terminal 4:

**Terminal 6:**
```bash
ls -la /home/keshav-goel/Desktop/OSN/osn_course-project/storage_server/storage_data1/files/
ls -la /home/keshav-goel/Desktop/OSN/osn_course-project/storage_server/storage_data2/files/
```

**Expected:** Both directories should contain the same files (test.txt)

---

## 🛑 Shutdown Sequence

1. Exit clients: Type `EXIT` in each client terminal
2. Stop storage servers: `Ctrl+C` in Terminals 2 & 3
3. Stop name server: `Ctrl+C` in Terminal 1

---

**Good luck with your testing! 🚀**
