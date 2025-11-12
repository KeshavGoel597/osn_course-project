# Critical Fixes - Testing Guide

**Test these scenarios to verify the critical fixes are working correctly.**

---

## ✅ **PRE-TEST SETUP**

### Step 1: Ensure everything compiles
```bash
cd /home/keshav-goel/Desktop/OSN/osn_course-project
make -C name_server clean && make -C name_server
make -C storage_server clean && make -C storage_server  
make -C client clean && make -C client
```

Expected: All build successfully with zero errors.

### Step 2: Clean old data
```bash
rm -rf name_server.log
rm -rf storage_data*
```

---

## 🧪 **TEST 1: Hash Table (O(1) File Lookup)**

### Objective
Verify that file lookups are fast regardless of number of files.

### Steps

**Terminal 1 - Start Name Server:**
```bash
cd name_server
./name_server
```

**Terminal 2 - Start Storage Server 1 (Primary):**
```bash
cd storage_server
./storage_server 1 8081 9091 storage_data1
```

**Terminal 3 - Start Client:**
```bash
cd client
./client alice
```

**In Client Terminal:**
```
# Create 10 files quickly
CREATE file1.txt
CREATE file2.txt
CREATE file3.txt
CREATE file4.txt
CREATE file5.txt
CREATE file6.txt
CREATE file7.txt
CREATE file8.txt
CREATE file9.txt
CREATE file10.txt

# Try INFO on different files - should be instant
INFO file1.txt
INFO file5.txt
INFO file10.txt

# View all files
VIEW
```

### Expected Results
- ✅ All CREATE operations succeed immediately
- ✅ INFO commands return instantly (no visible delay)
- ✅ Name Server logs show: `[Hash Table] Inserted file 'file1.txt' at bucket XXX`
- ✅ VIEW shows all 10 files

### What to Check in Name Server Terminal
Look for lines like:
```
[Hash Table] Initialized with 10007 buckets
[Hash Table] Inserted file 'file1.txt' at bucket 5432
[Hash Table] Inserted file 'file2.txt' at bucket 8901
...
```

---

## 🧪 **TEST 2: Asynchronous Replication**

### Objective
Verify that write operations return immediately without waiting for backup ACK.

### Steps

**Terminal 1 - Name Server** (already running from Test 1)

**Terminal 2 - Storage Server 1 (Primary)** (already running)

**Terminal 3 - Start Storage Server 2 (Backup):**
```bash
cd storage_server
./storage_server 2 8082 9092 storage_data2
```

Wait a few seconds for backup pairing to complete.

**Terminal 4 - Client:**
```bash
cd client
./client bob
```

**In Client Terminal:**
```
CREATE async_test.txt
WRITE async_test.txt 0
0 Hello this is asynchronous replication test!
ETIRW

READ async_test.txt
```

### Expected Results
- ✅ WRITE command completes immediately (sub-second response)
- ✅ Storage Server 1 logs show: `[Async Replication] Enqueued SYNC for file 'async_test.txt' (queue size: 1)`
- ✅ Storage Server 1 logs show: `[Async Replication] Processing task: SYNC for 'async_test.txt'`
- ✅ READ shows the written content

### What to Check in Storage Server 1 Terminal
Look for:
```
[Async Replication] Worker thread started
[Async Replication] Enqueued SYNC for file 'async_test.txt' (queue size: 1)
[WRITE] Write operation completed successfully
[Async Replication] Processing task: SYNC for 'async_test.txt'
[Backup Handler] Replicating SYNC for file: async_test.txt
```

**Key Observation:** "Write operation completed" appears BEFORE "Processing task", meaning client got response before backup sync started! ✅

### Verify Backup Got the File
**In Terminal 3 (Storage Server 2):**
Check for:
```
[Backup Handler] Received SYNC request for file: async_test.txt
```

Manually verify:
```bash
cd storage_data2
ls -la
# Should see async_test.txt
cat async_test.txt
# Should contain: "Hello this is asynchronous replication test!"
```

---

## 🧪 **TEST 3: Sentence Delimiter Parsing**

### Objective
Verify that every `.`, `!`, `?` creates a new sentence (even in "e.g.").

### Steps

**Using existing client from previous tests:**

```
CREATE delimiter_test.txt
WRITE delimiter_test.txt 0
0 This is e.g. a test! Really? Yes.
ETIRW

READ delimiter_test.txt
```

### Expected Results

The file should be split into **SEVEN** sentences:
```
1: "This is e"
2: "g"
3: " a test"
4: " Really"
5: " Yes"
```

Actually, let me recount based on the delimiter algorithm:
- "This is e.g." splits as:
  - "This is e." (sentence 1)
  - "g." (sentence 2)
- "a test!" → "a test!" (sentence 3)
- "Really?" → "Really?" (sentence 4)
- "Yes." → "Yes." (sentence 5)

So expected: **5 sentences**

### How to Verify
The READ output should show the content reconstructed, but internally it's stored as separate sentences. To truly verify, try:

```
CREATE test2.txt
WRITE test2.txt 0
0 e.g.
ETIRW
```

Then try to write to sentence 1 (should be "g.", not the whole "e.g."):
```
WRITE test2.txt 1
0 NEW_WORD
ETIRW

READ test2.txt
```

Expected: Should show "e. NEW_WORD" (sentence 0 was "e.", sentence 1 was "g." which got replaced)

---

## 🧪 **TEST 4: Primary Failover and Recovery**

### Objective
Test that backup takes over when primary fails, and primary can reconnect.

### Steps

**Ensure from Test 2:**
- Name Server running
- Storage Server 1 (Primary) running  
- Storage Server 2 (Backup) running
- A file exists on SS1

**Terminal 4 - Client:**
```
CREATE failover_test.txt
WRITE failover_test.txt 0
0 This file will survive failover!
ETIRW
```

**Simulate Primary Failure:**
Go to Terminal 2 (Storage Server 1) and press **Ctrl+C** to kill it.

**Wait 35 seconds** (heartbeat timeout = 30 seconds + buffer)

**Check Name Server Terminal:**
Should see:
```
[Heartbeat Monitor] SS1 heartbeat timeout
[Failover] SS1 marked as OFFLINE
[Failover] Handling failure of primary server SS1
[Failover] SS2 promoted to acting primary for failed SS1
```

**Try to read the file (should work from backup):**
```
READ failover_test.txt
```

Expected: ✅ File content displayed (served from SS2 acting as primary)

**Restart Primary SS1:**
Go to Terminal 2:
```bash
cd storage_server
./storage_server 1 8081 9091 storage_data1
```

**Check Name Server logs:**
Should see:
```
[SS Registration] Storage Server 1 reconnected
[SS Recovery] SS1 recovered, demoting backup SS2 from acting primary
```

**Verify client can still access files:**
```
READ failover_test.txt
INFO failover_test.txt
```

Expected: ✅ Operations succeed

---

## 🧪 **TEST 5: Concurrent Access & Locking**

### Objective
Verify sentence-level locking prevents simultaneous edits.

### Steps

**Terminal 4 - Client 1 (alice):**
```
CREATE concurrent.txt
WRITE concurrent.txt 0
0 First sentence.
ETIRW

WRITE concurrent.txt 0
# DON'T send ETIRW yet - keep lock held
0 Locked by alice...
```

**Terminal 5 - Client 2 (bob):**
```bash
cd client
./client bob
```

```
# Try to write to same sentence while alice holds lock
WRITE concurrent.txt 0
```

Expected: ❌ Error message: "Sentence is locked by another user"

**Back in Terminal 4 (alice):**
```
ETIRW
```

**Now in Terminal 5 (bob):**
```
# Try again
WRITE concurrent.txt 0
0 Now bob can edit!
ETIRW

READ concurrent.txt
```

Expected: ✅ Bob can now edit, file shows "Now bob can edit."

---

## 📊 **SUCCESS CRITERIA**

| Test | Feature | Pass Criteria |
|------|---------|---------------|
| 1 | Hash Table | Files found instantly, hash bucket logs visible |
| 2 | Async Replication | Write completes before backup sync starts |
| 3 | Delimiters | "e.g." becomes "e." and "g." as separate sentences |
| 4 | Failover | Backup serves requests when primary fails |
| 5 | Locking | Second user blocked while first holds lock |

If all 5 tests pass → **All critical fixes are working! ✅**

---

## 🐛 **TROUBLESHOOTING**

### Issue: "Connection refused" errors
**Solution:** Ensure Name Server is started first, then Storage Servers

### Issue: "File not found" after failover
**Solution:** Check that both SS1 and SS2 were connected before creating the file. Backup only gets files if replication was active.

### Issue: Async replication logs not appearing
**Solution:** Check SS is a primary server (odd ID). Backup servers don't replicate.

### Issue: Hash table logs not showing
**Solution:** Check Name Server terminal output from startup - should see "Hash Table] Initialized with 10007 buckets"

---

## 📈 **PERFORMANCE BENCHMARKING (Optional)**

### File Lookup Speed Test

Create a script to measure lookup time:

```bash
# In client, time how long this takes:
time INFO file1.txt
```

With hash table: Should be < 1ms  
Without hash table (old linear search): Would be O(N×M)

---

**END OF TESTING GUIDE**
