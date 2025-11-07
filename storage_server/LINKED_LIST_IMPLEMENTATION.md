# Linked List-Based File Handler Implementation

## Overview
This implementation uses linked lists to represent files in memory, supporting concurrent sentence-level editing for large files.

## Data Structures

### WordNode
- Stores a single word
- Points to next word in sentence
- Size: ~260 bytes per word

### SentenceNode  
- Contains linked list of WordNode
- Has own pthread_mutex for sentence-level locking
- Stores delimiter (., !, ?, or \0)
- Points to next sentence
- Size: ~48 bytes + word list

### LoadedFile
- Represents entire file in memory
- Contains head of sentence linked list
- Has pthread_rwlock for file-level read/write coordination
- Cached in global file cache
- Size: ~312 bytes + sentence/word lists

## Memory Management

### Lazy Loading Strategy (Option B)
1. Files loaded on first access
2. Kept in memory until explicitly unloaded
3. Global file_cache_mutex protects cache operations
4. Double-check pattern prevents race conditions

### Locking Hierarchy
```
file_cache_mutex (global)
  └─> file->file_rwlock (per file)
        └─> sentence->sentence_lock (per sentence)
```

## Key Features

### 1. Concurrent Access
- Multiple readers can read simultaneously (rwlock)
- Multiple writers can edit different sentences (per-sentence mutex)
- Writer locks specific sentence, others can access different sentences

### 2. Large File Support
- Dynamic memory allocation (no fixed buffer limits)
- Linked list grows as needed
- Can handle 100k+ lines

### 3. Atomic Writes
- Write to .tmp swap file
- Atomically rename() to replace original
- No partial writes visible

### 4. Delimiter Handling
- Automatically splits on '.', '!', '?' in inserted content
- Creates new sentences as needed
- Example: Inserting "AAD. Oh" creates two sentences

## Write Operation Flow

```
1. User: WRITE file.txt 5
2. Client connects to SS
3. SS: get_file_from_cache("file.txt")
   - Lock file_cache_mutex
   - Check cache
   - If not loaded: load_file_into_memory()
   - Unlock file_cache_mutex
   - Return LoadedFile*
   
4. SS: lock_sentence_ll(5)
   - Traverse to sentence 5
   - pthread_mutex_trylock(&sentence->lock)
   - If fails: return ERR_SENTENCE_LOCKED
   
5. SS: save_undo_backup_ll()
   - Write current in-memory state to undo file
   
6. User enters write commands:
   > 3 beautiful
   > 7 amazing
   > ETIRW
   
7. For each command:
   SS: write_to_file_ll(file, 5, word_idx, content)
   - Parse content into word groups (check for delimiters)
   - Traverse to sentence 5, word word_idx
   - Insert new WordNode(s) at position
   - If delimiters found: create new SentenceNode(s)
   - Update pointers
   
8. On ETIRW:
   SS: sync_file_to_disk()
   - Write linked list to file.txt.tmp
   - rename(file.txt.tmp, file.txt)
   
   SS: unlock_sentence_ll(5)
   - sentence->is_locked = 0
   - pthread_mutex_unlock(&sentence->lock)
```

## Example: Insert with Delimiter

```
Original: "I like PNS"
Insert "AAD. Oh no" at word 3

Step 1: Parse content
  Group 1: ["AAD."], delimiter='.'
  Group 2: ["Oh", "no"], delimiter='\0'

Step 2: Insert Group 1 at position 3
  Before: ["I", "like", "PNS"]
  After:  ["I", "like", "PNS", "AAD."]
  Sentence delimiter = '.'

Step 3: Create new sentence for Group 2
  New SentenceNode: ["Oh", "no"]
  Link after current sentence

Result: Two sentences
  Sentence 0: ["I", "like", "PNS", "AAD."] (delimiter='.')
  Sentence 1: ["Oh", "no"] (delimiter='\0')
```

## Memory Usage Estimate

For file with 100,000 lines, 50 words/line, 5 chars/word:

```
Words: 100,000 × 50 = 5,000,000 WordNodes
  Size: 5M × 260 bytes = 1.3 GB

Sentences: 100,000 SentenceNodes
  Size: 100K × 48 bytes = 4.8 MB

LoadedFile: 1 × 312 bytes = 312 bytes

Total: ~1.3 GB in memory
```

This is acceptable for modern systems and much better than loading entire 30MB text repeatedly.

## Files Created

1. **file_handler_ll.h** - Header with data structures and function declarations
2. **file_handler_ll.c** - Core file operations, cache management, access control
3. **file_write_ll.c** - Write operation implementation with delimiter handling

## Integration

To use this instead of the original file_handler:
1. Include file_handler_ll.h instead of file_handler.h
2. Call init_file_handler_ll() instead of init_file_handler()
3. Use _ll versions of all functions

## Status

✅ Data structures defined
✅ Lazy loading implemented
✅ Cache with mutex protection
✅ Sentence-level locking
✅ Read operation (traverse list)
✅ Write operation (insert with delimiter handling)
✅ Atomic disk sync (swap file)
✅ UNDO support
✅ Access control
✅ Metadata management

⚠️ Not yet integrated into storage_server.c (still uses old file_handler)
⚠️ Needs testing with actual workload
