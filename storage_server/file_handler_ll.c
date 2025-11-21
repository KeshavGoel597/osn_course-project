#include "storage_server_all.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>

// Global storage directory
static char storage_dir[MAX_PATH];
static char files_dir[MAX_PATH];
static char undo_dir[MAX_PATH];
static char metadata_file[MAX_PATH];

// In-memory file cache (simple linked list for now)
static LoadedFile *file_cache_head = NULL;
static pthread_mutex_t file_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

// In-memory metadata
#define MAX_FILES 1000
static FileMetadata metadata_list[MAX_FILES];
static int metadata_count = 0;
static pthread_mutex_t metadata_mutex = PTHREAD_MUTEX_INITIALIZER;

// Access control cache for O(1) lookups
// Hash table: key = "filename:username", value = access_type (R=1, W=2, RW=3)
#define ACCESS_CACHE_SIZE 10007  // Prime number for better distribution
typedef struct AccessCacheEntry {
    char key[MAX_FILENAME + MAX_USERNAME + 2];  // "filename:username"
    int access_type;  // 1=READ, 2=WRITE, 3=READ_WRITE
    int valid;
    struct AccessCacheEntry *next;  // For chaining
} AccessCacheEntry;

static AccessCacheEntry access_cache[ACCESS_CACHE_SIZE];
static pthread_mutex_t access_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

// Hash function for access cache
static unsigned int hash_access_key(const char *key) {
    unsigned int hash = 5381;
    int c;
    while ((c = *key++))
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    return hash % ACCESS_CACHE_SIZE;
}

// Build access cache for a file (parse access_list and populate hash table)
static void build_access_cache_for_file(const char *filename, const char *access_list) {
    pthread_mutex_lock(&access_cache_mutex);
    
    // Parse access_list: format is "user1:R,user2:RW,user3:R,..."
    char list_copy[MAX_DATA_SIZE];
    strncpy(list_copy, access_list, MAX_DATA_SIZE - 1);
    list_copy[MAX_DATA_SIZE - 1] = '\0';
    
    char *entry = strtok(list_copy, ",");
    while (entry != NULL) {
        // Parse "username:R" or "username:RW"
        char *colon = strchr(entry, ':');
        if (colon != NULL) {
            *colon = '\0';
            char *username = entry;
            char *access_str = colon + 1;
            
            // Create cache key
            char cache_key[MAX_FILENAME + MAX_USERNAME + 2];
            snprintf(cache_key, sizeof(cache_key), "%s:%s", filename, username);
            
            // Determine access type
            int access_type = 0;
            if (strcmp(access_str, "R") == 0) {
                access_type = 1;
            } else if (strcmp(access_str, "RW") == 0) {
                access_type = 3;
            }
            
            if (access_type > 0) {
                // Insert into hash table
                unsigned int hash = hash_access_key(cache_key);
                AccessCacheEntry *entry_ptr = &access_cache[hash];
                
                // Check if entry already exists
                int found = 0;
                if (entry_ptr->valid && strcmp(entry_ptr->key, cache_key) == 0) {
                    entry_ptr->access_type = access_type;
                    found = 1;
                } else {
                    // Linear probing for collision resolution
                    for (int i = 1; i < ACCESS_CACHE_SIZE && !found; i++) {
                        unsigned int probe_hash = (hash + i) % ACCESS_CACHE_SIZE;
                        entry_ptr = &access_cache[probe_hash];
                        if (!entry_ptr->valid) {
                            strncpy(entry_ptr->key, cache_key, sizeof(entry_ptr->key) - 1);
                            entry_ptr->access_type = access_type;
                            entry_ptr->valid = 1;
                            found = 1;
                            break;
                        } else if (strcmp(entry_ptr->key, cache_key) == 0) {
                            entry_ptr->access_type = access_type;
                            found = 1;
                            break;
                        }
                    }
                }
            }
        }
        entry = strtok(NULL, ",");
    }
    
    pthread_mutex_unlock(&access_cache_mutex);
}

// Invalidate access cache for a file
static void invalidate_access_cache_for_file(const char *filename) {
    pthread_mutex_lock(&access_cache_mutex);
    
    for (int i = 0; i < ACCESS_CACHE_SIZE; i++) {
        if (access_cache[i].valid) {
            // Check if key starts with filename:
            char prefix[MAX_FILENAME + 2];
            snprintf(prefix, sizeof(prefix), "%s:", filename);
            if (strncmp(access_cache[i].key, prefix, strlen(prefix)) == 0) {
                access_cache[i].valid = 0;
            }
        }
    }
    
    pthread_mutex_unlock(&access_cache_mutex);
}

// Lookup access in cache - O(1) average case
static int lookup_access_cache(const char *filename, const char *username, int required_access) {
    char cache_key[MAX_FILENAME + MAX_USERNAME + 2];
    snprintf(cache_key, sizeof(cache_key), "%s:%s", filename, username);
    
    pthread_mutex_lock(&access_cache_mutex);
    
    unsigned int hash = hash_access_key(cache_key);
    AccessCacheEntry *entry_ptr = &access_cache[hash];
    
    // Check direct hash location
    if (entry_ptr->valid && strcmp(entry_ptr->key, cache_key) == 0) {
        int has_access = (entry_ptr->access_type & required_access) == required_access;
        pthread_mutex_unlock(&access_cache_mutex);
        return has_access;
    }
    
    // Linear probing
    for (int i = 1; i < ACCESS_CACHE_SIZE; i++) {
        unsigned int probe_hash = (hash + i) % ACCESS_CACHE_SIZE;
        entry_ptr = &access_cache[probe_hash];
        
        if (!entry_ptr->valid) {
            // Not found in cache
            break;
        }
        
        if (strcmp(entry_ptr->key, cache_key) == 0) {
            int has_access = (entry_ptr->access_type & required_access) == required_access;
            pthread_mutex_unlock(&access_cache_mutex);
            return has_access;
        }
    }
    
    pthread_mutex_unlock(&access_cache_mutex);
    return 0;  // Not found in cache = no access
}

// Helper function to get current timestamp
void get_timestamp(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", t);
}

// Helper function to get file path
void get_file_path(const char *filename, char *path, size_t size) {
    snprintf(path, size, "%s/%s", files_dir, filename);
}

// Helper function to get undo file path
void get_undo_path(const char *filename, char *path, size_t size) {
    snprintf(path, size, "%s/%s.undo", undo_dir, filename);
}

// Initialize file handler
int init_file_handler_ll(const char *storage_directory) {
    strncpy(storage_dir, storage_directory, MAX_PATH - 1);
    snprintf(files_dir, MAX_PATH, "%s/files", storage_dir);
    snprintf(undo_dir, MAX_PATH, "%s/undo", storage_dir);
    snprintf(metadata_file, MAX_PATH, "%s/metadata.txt", storage_dir);
    
    // Create directories
    mkdir(storage_dir, 0755);
    mkdir(files_dir, 0755);
    mkdir(undo_dir, 0755);
    
    // CRITICAL FIX: Initialize access control cache
    memset(access_cache, 0, sizeof(access_cache));
    pthread_mutex_init(&access_cache_mutex, NULL);
    printf("Access control cache initialized (%d entries)\n", ACCESS_CACHE_SIZE);
    
    printf("File handler (linked list) initialized\n");
    printf("  Files directory: %s\n", files_dir);
    printf("  Undo directory: %s\n", undo_dir);
    printf("  Metadata file: %s\n", metadata_file);
    
    return 0;
}

// Find metadata for a file
static FileMetadata* find_metadata(const char *filename) {
    pthread_mutex_lock(&metadata_mutex);
    for (int i = 0; i < metadata_count; i++) {
        if (strcmp(metadata_list[i].filename, filename) == 0) {
            pthread_mutex_unlock(&metadata_mutex);
            return &metadata_list[i];
        }
    }
    pthread_mutex_unlock(&metadata_mutex);
    return NULL;
}

// Create word node
WordNode* create_word_node(const char *word) {
    WordNode *node = (WordNode*)malloc(sizeof(WordNode));
    if (node == NULL) return NULL;
    
    strncpy(node->word, word, sizeof(node->word) - 1);
    node->word[sizeof(node->word) - 1] = '\0';
    node->next = NULL;
    return node;
}

// Create sentence node
SentenceNode* create_sentence_node(char delimiter) {
    SentenceNode *node = (SentenceNode*)malloc(sizeof(SentenceNode));
    if (node == NULL) return NULL;
    
    node->words_head = NULL;
    node->delimiter = delimiter;
    pthread_mutex_init(&node->sentence_lock, NULL);
    node->is_locked = 0;
    memset(node->locked_by, 0, sizeof(node->locked_by));
    node->next = NULL;
    return node;
}

// Free word list
static void free_word_list(WordNode *head) {
    while (head != NULL) {
        WordNode *temp = head;
        head = head->next;
        free(temp);
    }
}

// Free sentence list
static void free_sentence_list(SentenceNode *head) {
    while (head != NULL) {
        SentenceNode *temp = head;
        head = head->next;
        free_word_list(temp->words_head);
        pthread_mutex_destroy(&temp->sentence_lock);
        free(temp);
    }
}

// Check if character is a sentence delimiter
static int is_delimiter(char c) {
    return (c == '.' || c == '!' || c == '?');
}

// Parse file content into linked list structure
static SentenceNode* parse_file_to_linked_list(const char *content) {
    SentenceNode *sentences_head = NULL;
    SentenceNode *current_sentence = NULL;
    SentenceNode *last_sentence = NULL;
    WordNode *current_word = NULL;
    
    char word_buffer[256];
    int word_idx = 0;
    
    const char *p = content;
    
    // For empty content, don't create any sentences
    if (*p == '\0') {
        return NULL;
    }
    
    // Create first sentence only if content exists
    current_sentence = create_sentence_node('\0');
    if (current_sentence == NULL) return NULL;
    sentences_head = current_sentence;
    last_sentence = current_sentence;
    
    while (*p) {
        // Skip leading whitespace
        while (*p && isspace(*p)) p++;
        if (!*p) break;
        
        // Read word
        word_idx = 0;
        while (*p && !isspace(*p)) {
            word_buffer[word_idx++] = *p++;
            if (word_idx >= 255) break;
        }
        word_buffer[word_idx] = '\0';
        
        if (word_idx == 0) continue;
        
        // Process word character by character to handle multiple delimiters
        // Each delimiter creates a new sentence, even in "e.g." -> "e." and "g."
        int start_pos = 0;
        for (int i = 0; i < word_idx; i++) {
            if (is_delimiter(word_buffer[i])) {
                // Found delimiter - create word up to and including delimiter
                char part[256];
                int len = i - start_pos + 1;
                strncpy(part, word_buffer + start_pos, len);
                part[len] = '\0';
                
                WordNode *new_word = create_word_node(part);
                if (new_word == NULL) {
                    free_sentence_list(sentences_head);
                    return NULL;
                }
                
                if (current_sentence->words_head == NULL) {
                    current_sentence->words_head = new_word;
                    current_word = new_word;
                } else {
                    current_word->next = new_word;
                    current_word = new_word;
                }
                
                // End this sentence
                current_sentence->delimiter = word_buffer[i];
                
                // Create new sentence for next part (always, even if empty)
                SentenceNode *new_sentence = create_sentence_node('\0');
                if (new_sentence == NULL) {
                    free_sentence_list(sentences_head);
                    return NULL;
                }
                last_sentence->next = new_sentence;
                last_sentence = new_sentence;
                current_sentence = new_sentence;
                current_word = NULL;
                
                start_pos = i + 1;
            }
        }
        
        // Add any remaining part after last delimiter
        if (start_pos < word_idx) {
            char remaining[256];
            strcpy(remaining, word_buffer + start_pos);
            
            WordNode *new_word = create_word_node(remaining);
            if (new_word == NULL) {
                free_sentence_list(sentences_head);
                return NULL;
            }
            
            if (current_sentence->words_head == NULL) {
                current_sentence->words_head = new_word;
                current_word = new_word;
            } else {
                current_word->next = new_word;
                current_word = new_word;
            }
        }
    }
    
    // CRITICAL FIX: Remove trailing empty sentences ONLY if they have no delimiter
    // This ensures proper indexing: "hi . HI" has 2 sentences, "hi . HI ." has 3 sentences
    SentenceNode *prev = NULL;
    SentenceNode *curr = sentences_head;
    
    // Find the last sentence
    while (curr != NULL && curr->next != NULL) {
        prev = curr;
        curr = curr->next;
    }
    
    // Remove last sentence only if it's empty AND has no delimiter
    if (curr != NULL && curr->words_head == NULL && curr->delimiter == '\0') {
        if (prev != NULL) {
            prev->next = NULL;
            free(curr);
        } else {
            // Only sentence is empty with no delimiter - this means empty file
            free_sentence_list(sentences_head);
            return NULL;
        }
    }
    
    return sentences_head;
}

// Load file from disk into linked list structure
LoadedFile* load_file_into_memory(const char *filename) {
    char filepath[MAX_PATH];
    get_file_path(filename, filepath, MAX_PATH);
    
    // Read file content
    FILE *fp = fopen(filepath, "r");
    if (fp == NULL) {
        fprintf(stderr, "Failed to open file: %s\n", filepath);
        return NULL;
    }
    
    // Get file size
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    // Allocate buffer
    char *content = (char*)malloc(file_size + 1);
    if (content == NULL) {
        fclose(fp);
        return NULL;
    }
    
    // Read entire file
    size_t bytes_read = fread(content, 1, file_size, fp);
    content[bytes_read] = '\0';
    fclose(fp);
    
    // Parse into linked list
    SentenceNode *sentences = NULL;
    if (file_size > 0) {
        sentences = parse_file_to_linked_list(content);
    }
    free(content);
    
    // CRITICAL FIX: For empty files, don't create any sentences
    // For non-empty files, the parser will create appropriate sentences including empty ones
    // This allows proper indexing as per TinyOS specification
    if (sentences == NULL) {
        printf("Loaded empty file '%s' - no sentences created\n", filename);
    }
    
    // Create LoadedFile structure
    LoadedFile *loaded = (LoadedFile*)malloc(sizeof(LoadedFile));
    if (loaded == NULL) {
        free_sentence_list(sentences);
        return NULL;
    }
    
    strncpy(loaded->filename, filename, MAX_FILENAME - 1);
    loaded->sentences_head = sentences;
    loaded->is_loaded = 1;
    pthread_rwlock_init(&loaded->file_rwlock, NULL);
    loaded->next = NULL;
    
    // CRITICAL FIX: Initialize reference counting
    loaded->refcount = 0;
    pthread_mutex_init(&loaded->refcount_lock, NULL);
    loaded->marked_for_deletion = 0;
    
    // Count sentences
    loaded->sentence_count = 0;
    SentenceNode *s = sentences;
    while (s != NULL) {
        loaded->sentence_count++;
        s = s->next;
    }
    
    printf("Loaded file '%s' into memory (%d sentences)\n", filename, loaded->sentence_count);
    
    return loaded;
}

// CRITICAL FIX: Reference counting functions to prevent memory leak on delete
void file_addref(LoadedFile *file) {
    if (!file) return;
    pthread_mutex_lock(&file->refcount_lock);
    file->refcount++;
    pthread_mutex_unlock(&file->refcount_lock);
}

void file_release(LoadedFile *file) {
    if (!file) return;
    
    pthread_mutex_lock(&file->refcount_lock);
    file->refcount--;
    int refs = file->refcount;
    int marked = file->marked_for_deletion;
    pthread_mutex_unlock(&file->refcount_lock);
    
    // If refcount reaches 0 AND file is marked for deletion, free it
    if (refs == 0 && marked) {
        printf("[RefCount] File '%s' refcount=0 and marked for deletion, freeing memory\n", 
               file->filename);
        
        // Remove from cache
        pthread_mutex_lock(&file_cache_mutex);
        LoadedFile *current = file_cache_head;
        LoadedFile *prev = NULL;
        
        while (current != NULL) {
            if (current == file) {
                if (prev == NULL) {
                    file_cache_head = current->next;
                } else {
                    prev->next = current->next;
                }
                break;
            }
            prev = current;
            current = current->next;
        }
        pthread_mutex_unlock(&file_cache_mutex);
        
        // Free memory
        if (file->sentences_head) {
            free_sentence_list(file->sentences_head);
        }
        pthread_rwlock_destroy(&file->file_rwlock);
        pthread_mutex_destroy(&file->refcount_lock);
        free(file);
    }
}

// Get file from cache (with lazy loading)
LoadedFile* get_file_from_cache(const char *filename) {
    pthread_mutex_lock(&file_cache_mutex);
    
    // Check if file is already in cache
    LoadedFile *current = file_cache_head;
    while (current != NULL) {
        if (strcmp(current->filename, filename) == 0 && current->is_loaded) {
            // CRITICAL FIX: Increment reference count when returning cached file
            file_addref(current);
            pthread_mutex_unlock(&file_cache_mutex);
            return current;
        }
        current = current->next;
    }
    
    // CRITICAL FIX: Keep mutex locked while loading to prevent race condition
    // File not in cache, load it while holding the lock
    LoadedFile *loaded = load_file_into_memory(filename);
    if (loaded == NULL) {
        pthread_mutex_unlock(&file_cache_mutex);
        return NULL;
    }
    
    // Add to cache (at head) - still holding mutex
    loaded->next = file_cache_head;
    file_cache_head = loaded;
    
    // CRITICAL FIX: Increment reference count for newly loaded file
    file_addref(loaded);
    
    pthread_mutex_unlock(&file_cache_mutex);
    return loaded;
}

// Unload file from memory
int unload_file_from_memory(const char *filename) {
    pthread_mutex_lock(&file_cache_mutex);
    
    LoadedFile *current = file_cache_head;
    LoadedFile *prev = NULL;
    
    while (current != NULL) {
        if (strcmp(current->filename, filename) == 0) {
            // Remove from list
            if (prev == NULL) {
                file_cache_head = current->next;
            } else {
                prev->next = current->next;
            }
            
            // Free resources
            free_sentence_list(current->sentences_head);
            pthread_rwlock_destroy(&current->file_rwlock);
            free(current);
            
            pthread_mutex_unlock(&file_cache_mutex);
            printf("Unloaded file '%s' from memory\n", filename);
            return 0;
        }
        prev = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&file_cache_mutex);
    return -1;
}

// Reload file from disk (for UNDO operation)
// This safely replaces the cached version with fresh content from disk
int reload_file_from_disk(const char *filename) {
    pthread_mutex_lock(&file_cache_mutex);
    
    // Find and remove old cached version
    LoadedFile *current = file_cache_head;
    LoadedFile *prev = NULL;
    
    while (current != NULL) {
        if (strcmp(current->filename, filename) == 0) {
            // Found it - remove from cache
            if (prev == NULL) {
                file_cache_head = current->next;
            } else {
                prev->next = current->next;
            }
            
            // Free the old version
            free_sentence_list(current->sentences_head);
            pthread_rwlock_destroy(&current->file_rwlock);
            free(current);
            
            printf("Removed old cached version of '%s'\n", filename);
            break;
        }
        prev = current;
        current = current->next;
    }
    
    // Reload from disk
    LoadedFile *reloaded = load_file_into_memory(filename);
    if (reloaded != NULL) {
        // Add to cache
        reloaded->next = file_cache_head;
        file_cache_head = reloaded;
        printf("Reloaded file '%s' from disk\n", filename);
        pthread_mutex_unlock(&file_cache_mutex);
        return 0;
    } else {
        pthread_mutex_unlock(&file_cache_mutex);
        return -1;
    }
}

// Create a new file
int create_file_ll(const char *filename, const char *owner) {
    // Check if file already exists
    if (find_metadata(filename) != NULL) {
        fprintf(stderr, "File '%s' already exists\n", filename);
        return ERR_FILE_EXISTS;
    }
    
    pthread_mutex_lock(&metadata_mutex);
    if (metadata_count >= MAX_FILES) {
        pthread_mutex_unlock(&metadata_mutex);
        fprintf(stderr, "Maximum file limit reached\n");
        return -1;
    }
    
    // Create the file on disk
    char filepath[MAX_PATH];
    get_file_path(filename, filepath, MAX_PATH);
    
    FILE *fp = fopen(filepath, "w");
    if (fp == NULL) {
        pthread_mutex_unlock(&metadata_mutex);
        perror("Failed to create file");
        return -1;
    }
    fclose(fp);
    
    // Create metadata
    FileMetadata *meta = &metadata_list[metadata_count];
    memset(meta, 0, sizeof(FileMetadata));
    
    strncpy(meta->filename, filename, MAX_FILENAME - 1);
    strncpy(meta->owner, owner, MAX_USERNAME - 1);
    get_timestamp(meta->created_time, sizeof(meta->created_time));
    get_timestamp(meta->modified_time, sizeof(meta->modified_time));
    get_timestamp(meta->accessed_time, sizeof(meta->accessed_time));
    meta->file_size = 0;
    meta->word_count = 0;
    meta->char_count = 0;
    snprintf(meta->access_list, MAX_DATA_SIZE, "%s:RW", owner);
    
    metadata_count++;
    pthread_mutex_unlock(&metadata_mutex);
    
    // Save metadata
    save_metadata_ll();
    
    printf("File '%s' created by owner '%s'\n", filename, owner);
    return 0;
}

// Delete a file
int delete_file_ll(const char *filename) {
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) {
        fprintf(stderr, "File '%s' not found\n", filename);
        return ERR_FILE_NOT_FOUND;
    }
    
    // CRITICAL FIX: Use reference counting to safely delete files
    // Mark file for deletion and only free memory when refcount reaches 0
    pthread_mutex_lock(&file_cache_mutex);
    LoadedFile *current = file_cache_head;
    while (current != NULL) {
        if (strcmp(current->filename, filename) == 0) {
            pthread_mutex_lock(&current->refcount_lock);
            current->marked_for_deletion = 1;
            int refs = current->refcount;
            pthread_mutex_unlock(&current->refcount_lock);
            
            printf("[Delete] File '%s' marked for deletion (refcount=%d)\n", filename, refs);
            
            // If refcount is 0, we can free immediately
            if (refs == 0) {
                // Remove from cache list
                LoadedFile *prev = NULL;
                LoadedFile *temp = file_cache_head;
                while (temp != NULL) {
                    if (temp == current) {
                        if (prev == NULL) {
                            file_cache_head = temp->next;
                        } else {
                            prev->next = temp->next;
                        }
                        break;
                    }
                    prev = temp;
                    temp = temp->next;
                }
                
                pthread_mutex_unlock(&file_cache_mutex);
                
                // Free memory
                if (current->sentences_head) {
                    free_sentence_list(current->sentences_head);
                }
                pthread_rwlock_destroy(&current->file_rwlock);
                pthread_mutex_destroy(&current->refcount_lock);
                free(current);
                
                printf("[Delete] File '%s' freed from memory immediately (refcount was 0)\n", filename);
            } else {
                pthread_mutex_unlock(&file_cache_mutex);
                printf("[Delete] File '%s' will be freed when refcount reaches 0\n", filename);
            }
            
            break;
        }
        current = current->next;
    }
    
    if (current == NULL) {
        pthread_mutex_unlock(&file_cache_mutex);
    }
    
    // Delete the file from disk
    char filepath[MAX_PATH];
    get_file_path(filename, filepath, MAX_PATH);
    
    if (unlink(filepath) < 0) {
        perror("Failed to delete file");
        return -1;
    }
    
    // Delete undo backup if exists
    char undo_path[MAX_PATH];
    get_undo_path(filename, undo_path, MAX_PATH);
    unlink(undo_path);  // Ignore errors
    
    // Remove from metadata list
    pthread_mutex_lock(&metadata_mutex);
    int index = meta - metadata_list;
    for (int i = index; i < metadata_count - 1; i++) {
        metadata_list[i] = metadata_list[i + 1];
    }
    metadata_count--;
    pthread_mutex_unlock(&metadata_mutex);
    
    save_metadata_ll();
    
    printf("File '%s' deleted from disk\n", filename);
    return 0;
}

// Read entire file content by traversing linked list
int read_file_ll(const char *filename, char *content, int max_size) {
    LoadedFile *file = get_file_from_cache(filename);
    if (file == NULL) {
        return ERR_FILE_NOT_FOUND;
    }
    
    // If any sentence is locked (write in progress), read from disk instead
    if (file_has_locked_sentences(filename)) {
        printf("[READ] File has locked sentences, reading from disk for isolation\n");
        file_release(file);  // CRITICAL FIX: Release reference before returning
        return read_file_from_disk_ll(filename, content, max_size);
    }
    
    pthread_rwlock_rdlock(&file->file_rwlock);
    
    content[0] = '\0';
    int remaining = max_size - 1;
    
    SentenceNode *sent = file->sentences_head;
    int first_sentence = 1;
    
    while (sent != NULL && remaining > 0) {
        // Add space between sentences
        if (!first_sentence) {
            strncat(content, " ", remaining);
            remaining--;
        }
        first_sentence = 0;
        
        // Traverse words
        WordNode *word = sent->words_head;
        int first_word = 1;
        while (word != NULL && remaining > 0) {
            if (!first_word) {
                strncat(content, " ", remaining);
                remaining--;
            }
            first_word = 0;
            
            strncat(content, word->word, remaining);
            remaining -= strlen(word->word);
            
            word = word->next;
        }
        
        sent = sent->next;
    }
    
    pthread_rwlock_unlock(&file->file_rwlock);
    
    // Update access time
    FileMetadata *meta = find_metadata(filename);
    if (meta != NULL) {
        pthread_mutex_lock(&metadata_mutex);
        get_timestamp(meta->accessed_time, sizeof(meta->accessed_time));
        pthread_mutex_unlock(&metadata_mutex);
    }
    
    file_release(file);  // CRITICAL FIX: Release reference before returning
    return 0;
}

// Check if file has any locked sentences
int file_has_locked_sentences(const char *filename) {
    LoadedFile *file = get_file_from_cache(filename);
    if (file == NULL) {
        return 0; // File not loaded, no locks
    }
    
    pthread_rwlock_rdlock(&file->file_rwlock);
    
    SentenceNode *sent = file->sentences_head;
    while (sent != NULL) {
        if (sent->is_locked) {
            pthread_rwlock_unlock(&file->file_rwlock);
            return 1; // Found a locked sentence
        }
        sent = sent->next;
    }
    
    pthread_rwlock_unlock(&file->file_rwlock);
    return 0; // No locked sentences
}

// Read file directly from disk (bypassing cache)
int read_file_from_disk_ll(const char *filename, char *content, int max_size) {
    char filepath[MAX_PATH];
    get_file_path(filename, filepath, MAX_PATH);
    
    FILE *fp = fopen(filepath, "r");
    if (fp == NULL) {
        return ERR_FILE_NOT_FOUND;
    }
    
    content[0] = '\0';
    int remaining = max_size - 1;
    int bytes_read = 0;
    
    char ch;
    while ((ch = fgetc(fp)) != EOF && remaining > 0) {
        content[bytes_read++] = ch;
        remaining--;
    }
    content[bytes_read] = '\0';
    
    fclose(fp);
    
    // Update access time
    FileMetadata *meta = find_metadata(filename);
    if (meta != NULL) {
        pthread_mutex_lock(&metadata_mutex);
        get_timestamp(meta->accessed_time, sizeof(meta->accessed_time));
        pthread_mutex_unlock(&metadata_mutex);
    }
    
    return 0;
}

// Helper function to write linked list to file
static int write_linked_list_to_file(FILE *fp, SentenceNode *sentences_head) {
    SentenceNode *sent = sentences_head;
    
    while (sent != NULL) {
        // Write words in this sentence
        WordNode *word = sent->words_head;
        int first_word = 1;
        while (word != NULL) {
            if (!first_word) {
                fprintf(fp, " ");
            }
            first_word = 0;
            fprintf(fp, "%s", word->word);
            word = word->next;
        }
        
        // CRITICAL FIX: Write the sentence delimiter if present
        // Without this, delimiters are lost when syncing to disk
        if (sent->delimiter != '\0') {
            fprintf(fp, "%c", sent->delimiter);
        }
        
        // Add space after this sentence if there's a next sentence
        // This ensures sentences are separated when read back
        if (sent->next != NULL) {
            fprintf(fp, " ");
        }
        
        sent = sent->next;
    }
    
    return 0;
}

// Save in-memory linked list to disk (via swap file)
int sync_file_to_disk(const char *filename) {
    LoadedFile *file = get_file_from_cache(filename);
    if (file == NULL) {
        return ERR_FILE_NOT_FOUND;
    }
    
    char filepath[MAX_PATH];
    char swappath[MAX_PATH];
    get_file_path(filename, filepath, MAX_PATH);
    snprintf(swappath, MAX_PATH, "%s.tmp", filepath);
    
    pthread_rwlock_rdlock(&file->file_rwlock);
    
    // Write to swap file
    FILE *fp = fopen(swappath, "w");
    if (fp == NULL) {
        pthread_rwlock_unlock(&file->file_rwlock);
        perror("Failed to create swap file");
        return -1;
    }
    
    write_linked_list_to_file(fp, file->sentences_head);
    fclose(fp);
    
    pthread_rwlock_unlock(&file->file_rwlock);
    
    // Atomically replace original with swap file
    if (rename(swappath, filepath) < 0) {
        perror("Failed to rename swap file");
        unlink(swappath);
        return -1;
    }
    
    printf("Synced file '%s' to disk\n", filename);
    return 0;
}

// Lock a sentence (NEW VERSION)
int lock_sentence_ll(const char *filename, int sentence_index, const char *username) {
    LoadedFile *file = get_file_from_cache(filename);
    if (file == NULL) {
        return ERR_FILE_NOT_FOUND;
    }
    
    // Validate sentence_index (must be non-negative and within valid range for creation)
    if (sentence_index < 0) {
        printf("Lock FAILED: Negative sentence index %d for file '%s'\n", sentence_index, filename);
        file_release(file);
        return ERR_SENTENCE_OUT_OF_RANGE;
    }
    
    // CRITICAL FIX: Allow locking next sentence index (for creating new sentences)
    // This matches TinyOS behavior: sentence_index <= sentence_count is valid
    if (sentence_index > file->sentence_count) {
        printf("Lock FAILED: Sentence index %d out of range (0-%d) for file '%s'\n", 
               sentence_index, file->sentence_count, filename);
        file_release(file);
        return ERR_SENTENCE_OUT_OF_RANGE;
    }
    
    // Traverse to target sentence
    SentenceNode *sent = file->sentences_head;
    int current_index = 0;
    
    while (sent != NULL && current_index < sentence_index) {
        sent = sent->next;
        current_index++;
    }
    
    // sent will be NULL for new sentences (sentence_index == sentence_count)
    // This is OK - we'll handle it differently
    if (sent == NULL) {
        // This is a new sentence (sentence_index == sentence_count)
        // No actual locking needed yet - the write handler will create and lock it
        printf("Allowing lock for new sentence %d in file '%s' for user '%s'\n", 
               sentence_index, filename, username);
        file_release(file);
        return 0; // Success
    }

    int result = ERR_SENTENCE_LOCKED; // Default to locked

    // Lock the mutex to make our check atomic
    pthread_mutex_lock(&sent->sentence_lock);

    if (!sent->is_locked) {
        // --- Success case ---
        // Sentence is free, so we'll lock it.
        sent->is_locked = 1;
        strncpy(sent->locked_by, username, MAX_USERNAME - 1);
        sent->locked_by[MAX_USERNAME - 1] = '\0';
        result = 0; // Success
        printf("Lock SUCCESS: Sentence %d in '%s' locked by '%s'\n", 
               sentence_index, filename, username);
    } else {
        // --- Failure case ---
        // Sentence is already locked by someone (maybe even us, in a stale state)
        // We deny the lock.
        printf("Lock FAILED: Sentence %d in '%s' is already locked by '%s'. Request from '%s' denied.\n", 
               sentence_index, filename, sent->locked_by, username);
        result = ERR_SENTENCE_LOCKED;
    }

    // Unlock the mutex
    pthread_mutex_unlock(&sent->sentence_lock);
    
    file_release(file);
    return result;
}
// Unlock a sentence (NEW VERSION)
int unlock_sentence_ll(const char *filename, int sentence_index, const char *username) {
    LoadedFile *file = get_file_from_cache(filename);
    if (file == NULL) {
        return ERR_FILE_NOT_FOUND;
    }
    
    // Validate sentence_index (must be non-negative)
    if (sentence_index < 0) {
        printf("Unlock FAILED: Negative sentence index %d for file '%s'\n", sentence_index, filename);
        return -1;
    }
    
    // Traverse to target sentence
    SentenceNode *sent = file->sentences_head;
    int current_index = 0;
    
    while (sent != NULL && current_index < sentence_index) {
        sent = sent->next;
        current_index++;
    }
    
    if (sent == NULL) {
        return -1; // Sentence not found
    }

    // Lock the mutex to make our check atomic
    pthread_mutex_lock(&sent->sentence_lock);

    if (!sent->is_locked) {
        // Sentence is already unlocked. Do nothing.
        printf("Unlock INFO: Sentence %d in '%s' was already unlocked.\n", 
               sentence_index, filename);
    } else if (strcmp(sent->locked_by, username) != 0) {
        // Wrong user is trying to unlock. Do not allow.
        printf("Unlock FAILED: Sentence %d in '%s' is locked by '%s', cannot be unlocked by '%s'.\n", 
               sentence_index, filename, sent->locked_by, username);
    } else {
        // --- Success case ---
        // Sentence is locked AND the username matches. Unlock it.
        sent->is_locked = 0;
        memset(sent->locked_by, 0, sizeof(sent->locked_by));
        printf("Unlock SUCCESS: Sentence %d in '%s' unlocked by '%s'.\n", 
               sentence_index, filename, username);
    }

    // Unlock the mutex
    pthread_mutex_unlock(&sent->sentence_lock);
    
    return 0;
}

// Force unlock all sentences in a file (for cleanup/debugging)
int force_unlock_all_sentences_ll(const char *filename) {
    LoadedFile *file = get_file_from_cache(filename);
    if (file == NULL) {
        return ERR_FILE_NOT_FOUND;
    }
    
    SentenceNode *sent = file->sentences_head;
    int unlocked_count = 0;
    
    while (sent != NULL) {
        if (sent->is_locked) {
            sent->is_locked = 0;
            memset(sent->locked_by, 0, sizeof(sent->locked_by));
            // Try to unlock the mutex - may fail if not locked, ignore error
            pthread_mutex_trylock(&sent->sentence_lock);  // Acquire if not held
            pthread_mutex_unlock(&sent->sentence_lock);   // Release it
            unlocked_count++;
        }
        sent = sent->next;
    }
    
    printf("Force unlocked %d sentences in '%s'\n", unlocked_count, filename);
    return 0;
}

// Update file statistics (word count, character count, file size) after write operations
int update_file_statistics_ll(const char *filename) {
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) return ERR_FILE_NOT_FOUND;
    
    char filepath[MAX_PATH];
    get_file_path(filename, filepath, MAX_PATH);
    
    FILE *fp = fopen(filepath, "r");
    if (fp == NULL) {
        fprintf(stderr, "Failed to open file for statistics update: %s\n", filename);
        return ERR_FILE_NOT_FOUND;
    }
    
    // Calculate file size
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    // Count words and characters
    int word_count = 0;
    int char_count = 0;
    int in_word = 0;
    int c;
    
    while ((c = fgetc(fp)) != EOF) {
        char_count++;
        
        if (isspace(c) || c == '.' || c == '!' || c == '?') {
            if (in_word) {
                word_count++;
                in_word = 0;
            }
        } else {
            in_word = 1;
        }
    }
    
    // Handle last word if file doesn't end with whitespace
    if (in_word) {
        word_count++;
    }
    
    fclose(fp);
    
    // Update metadata
    pthread_mutex_lock(&metadata_mutex);
    meta->file_size = file_size;
    meta->word_count = word_count;
    meta->char_count = char_count;
    pthread_mutex_unlock(&metadata_mutex);
    
    // Save updated metadata to disk
    save_metadata_ll();
    
    printf("[Stats] Updated statistics for '%s': %d words, %d chars, %ld bytes\n", 
           filename, word_count, char_count, file_size);
    
    return 0;
}

// Write functions are implemented in file_write_ll.c

// Access control functions (same logic as before)
int has_read_access_ll(const char *filename, const char *username) {
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) return 0;
    
    // Owner always has access
    if (strcmp(meta->owner, username) == 0) return 1;
    
    // OPTIMIZED: Try cache first - O(1) lookup
    int cache_result = lookup_access_cache(filename, username, 1);  // 1 = READ access
    if (cache_result) {
        return 1;
    }
    
    // Cache miss - parse access_list and rebuild cache
    // This happens only when cache is cold or invalidated
    build_access_cache_for_file(filename, meta->access_list);
    
    // Try cache again after rebuild
    return lookup_access_cache(filename, username, 1);
}

int has_write_access_ll(const char *filename, const char *username) {
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) return 0;
    
    // Owner always has access
    if (strcmp(meta->owner, username) == 0) return 1;
    
    // OPTIMIZED: Try cache first - O(1) lookup
    // Write requires bit 2 set (RW=3 has bit 2 set)
    int cache_result = lookup_access_cache(filename, username, 2);  // 2 = WRITE access
    if (cache_result) {
        return 1;
    }
    
    // Cache miss - parse access_list and rebuild cache
    build_access_cache_for_file(filename, meta->access_list);
    
    // Try cache again after rebuild
    return lookup_access_cache(filename, username, 2);
}

int get_file_metadata_ll(const char *filename, FileMetadata *metadata) {
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) return ERR_FILE_NOT_FOUND;
    
    pthread_mutex_lock(&metadata_mutex);
    memcpy(metadata, meta, sizeof(FileMetadata));
    pthread_mutex_unlock(&metadata_mutex);
    return 0;
}

int update_file_metadata_ll(const char *filename, FileMetadata *metadata) {
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) return ERR_FILE_NOT_FOUND;
    
    pthread_mutex_lock(&metadata_mutex);
    memcpy(meta, metadata, sizeof(FileMetadata));
    pthread_mutex_unlock(&metadata_mutex);
    save_metadata_ll();
    return 0;
}

int update_file_modified_time_ll(const char *filename) {
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) return ERR_FILE_NOT_FOUND;
    
    pthread_mutex_lock(&metadata_mutex);
    get_timestamp(meta->modified_time, sizeof(meta->modified_time));
    pthread_mutex_unlock(&metadata_mutex);
    save_metadata_ll();
    return 0;
}

// CRITICAL FIX: Consolidated metadata update to prevent race conditions
// Updates modified time, file size, and word count in a SINGLE atomic operation
// Prevents lost updates from multiple sequential locks
int update_file_write_stats_ll(const char *filename) {
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) return ERR_FILE_NOT_FOUND;
    
    // Get file statistics from disk
    char filepath[MAX_PATH];
    get_file_path(filename, filepath, MAX_PATH);
    
    struct stat file_stat;
    if (stat(filepath, &file_stat) != 0) {
        return -1;
    }
    
    // Count words by reading file
    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        return -1;
    }
    
    int word_count = 0;
    int char_count = 0;
    int in_word = 0;
    int ch;
    
    while ((ch = fgetc(fp)) != EOF) {
        char_count++;
        if (ch == ' ' || ch == '\n' || ch == '\t') {
            if (in_word) {
                word_count++;
                in_word = 0;
            }
        } else {
            in_word = 1;
        }
    }
    if (in_word) word_count++;
    fclose(fp);
    
    // CRITICAL: Single mutex lock for all updates - prevents race conditions
    pthread_mutex_lock(&metadata_mutex);
    
    // Update all statistics atomically
    get_timestamp(meta->modified_time, sizeof(meta->modified_time));
    meta->file_size = file_stat.st_size;
    meta->word_count = word_count;
    meta->char_count = char_count;
    
    pthread_mutex_unlock(&metadata_mutex);
    
    // Single disk write for all changes
    save_metadata_ll();
    
    printf("[Metadata] Updated write stats for '%s': size=%ld, words=%d, chars=%d\n",
           filename, meta->file_size, meta->word_count, meta->char_count);
    
    return 0;
}

int update_file_accessed_time_ll(const char *filename, const char *username) {
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) return ERR_FILE_NOT_FOUND;
    
    pthread_mutex_lock(&metadata_mutex);
    // Format: "2025-10-10 14:32 by user1"
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));
    snprintf(meta->accessed_time, sizeof(meta->accessed_time), "%s by %s", timestamp, username);
    pthread_mutex_unlock(&metadata_mutex);
    save_metadata_ll();
    return 0;
}

int add_user_access_ll(const char *filename, const char *username, int access_type) {
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) return ERR_FILE_NOT_FOUND;
    
    pthread_mutex_lock(&metadata_mutex);
    
    // Remove existing access first (inline to avoid deadlock)
    char search_rw[128], search_r[128];
    snprintf(search_rw, sizeof(search_rw), "%s:RW", username);
    snprintf(search_r, sizeof(search_r), "%s:R", username);
    
    char *pos = strstr(meta->access_list, search_rw);
    if (pos == NULL) pos = strstr(meta->access_list, search_r);
    
    if (pos != NULL) {
        char *comma_before = (pos > meta->access_list && *(pos - 1) == ',') ? pos - 1 : NULL;
        char *comma_after = strchr(pos, ',');
        
        if (comma_before) {
            if (comma_after) {
                memmove(comma_before, comma_after, strlen(comma_after) + 1);
            } else {
                *comma_before = '\0';
            }
        } else if (comma_after) {
            memmove(pos, comma_after + 1, strlen(comma_after + 1) + 1);
        } else {
            *pos = '\0';
        }
    }
    
    // Add new access
    char access_str[64];
    if (access_type == ACCESS_READ) {
        snprintf(access_str, sizeof(access_str), ",%s:R", username);
        printf("[Access Control] Granting READ-ONLY access to %s for file %s\n", username, filename);
    } else {
        snprintf(access_str, sizeof(access_str), ",%s:RW", username);
        printf("[Access Control] Granting READ-WRITE access to %s for file %s\n", username, filename);
    }
    
    strncat(meta->access_list, access_str, MAX_DATA_SIZE - strlen(meta->access_list) - 1);
    printf("[Access Control] Updated access list: %s\n", meta->access_list);
    
    // CRITICAL: Invalidate access cache after permission change
    invalidate_access_cache_for_file(filename);
    
    pthread_mutex_unlock(&metadata_mutex);
    
    save_metadata_ll();
    return 0;
}

int remove_user_access_ll(const char *filename, const char *username) {
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) return ERR_FILE_NOT_FOUND;
    
    pthread_mutex_lock(&metadata_mutex);
    
    char search_rw[128], search_r[128];
    snprintf(search_rw, sizeof(search_rw), "%s:RW", username);
    snprintf(search_r, sizeof(search_r), "%s:R", username);
    
    char *pos = strstr(meta->access_list, search_rw);
    if (pos == NULL) pos = strstr(meta->access_list, search_r);
    
    if (pos != NULL) {
        char *comma_before = (pos > meta->access_list && *(pos - 1) == ',') ? pos - 1 : NULL;
        char *comma_after = strchr(pos, ',');
        
        if (comma_before) {
            if (comma_after) {
                memmove(comma_before, comma_after, strlen(comma_after) + 1);
            } else {
                *comma_before = '\0';
            }
        } else if (comma_after) {
            memmove(pos, comma_after + 1, strlen(comma_after + 1) + 1);
        } else {
            *pos = '\0';
        }
    }
    
    // CRITICAL: Invalidate access cache after permission change
    invalidate_access_cache_for_file(filename);
    
    pthread_mutex_unlock(&metadata_mutex);
    save_metadata_ll();
    return 0;
}

int get_file_list_ll(char *file_list, int max_size) {
    file_list[0] = '\0';
    
    pthread_mutex_lock(&metadata_mutex);
    for (int i = 0; i < metadata_count; i++) {
        if (i > 0) {
            strncat(file_list, ",", max_size - strlen(file_list) - 1);
        }
        // Format: filename:owner
        strncat(file_list, metadata_list[i].filename, max_size - strlen(file_list) - 1);
        strncat(file_list, ":", max_size - strlen(file_list) - 1);
        strncat(file_list, metadata_list[i].owner, max_size - strlen(file_list) - 1);
    }
    pthread_mutex_unlock(&metadata_mutex);
    
    return 0;
}

int load_metadata_ll() {
    FILE *fp = fopen(metadata_file, "r");
    if (fp == NULL) {
        fprintf(stderr, "No existing metadata file\n");
        return -1;
    }
    
    pthread_mutex_lock(&metadata_mutex);
    metadata_count = 0;
    while (metadata_count < MAX_FILES && 
           fread(&metadata_list[metadata_count], sizeof(FileMetadata), 1, fp) == 1) {
        metadata_count++;
    }
    pthread_mutex_unlock(&metadata_mutex);
    
    fclose(fp);
    printf("Loaded %d file metadata entries\n", metadata_count);
    return 0;
}

int save_metadata_ll() {
    FILE *fp = fopen(metadata_file, "w");
    if (fp == NULL) {
        perror("Failed to save metadata");
        return -1;
    }
    
    pthread_mutex_lock(&metadata_mutex);
    for (int i = 0; i < metadata_count; i++) {
        fwrite(&metadata_list[i], sizeof(FileMetadata), 1, fp);
    }
    pthread_mutex_unlock(&metadata_mutex);
    
    fclose(fp);
    return 0;
}

// Add metadata entry (used for folders and other entries)
int add_metadata_ll(FileMetadata *metadata) {
    pthread_mutex_lock(&metadata_mutex);
    
    // Check if already exists
    for (int i = 0; i < metadata_count; i++) {
        if (strcmp(metadata_list[i].filename, metadata->filename) == 0) {
            pthread_mutex_unlock(&metadata_mutex);
            fprintf(stderr, "Metadata for '%s' already exists\n", metadata->filename);
            return ERR_FILE_EXISTS;
        }
    }
    
    // Check capacity
    if (metadata_count >= MAX_FILES) {
        pthread_mutex_unlock(&metadata_mutex);
        fprintf(stderr, "Maximum file limit reached\n");
        return -1;
    }
    
    // Add to list
    memcpy(&metadata_list[metadata_count], metadata, sizeof(FileMetadata));
    metadata_count++;
    pthread_mutex_unlock(&metadata_mutex);
    
    // Save to disk
    save_metadata_ll();
    
    printf("Added metadata for '%s'\n", metadata->filename);
    return 0;
}

// Ensure a sentence has a delimiter (add newline if missing)
int ensure_sentence_delimiter_ll(const char *filename, int sentence_index) {
    LoadedFile *file = get_file_from_cache(filename);
    if (file == NULL) {
        return ERR_FILE_NOT_FOUND;
    }
    
    pthread_rwlock_wrlock(&file->file_rwlock);
    
    // Traverse to target sentence
    SentenceNode *target_sent = file->sentences_head;
    int current_index = 0;
    
    while (target_sent != NULL && current_index < sentence_index) {
        target_sent = target_sent->next;
        current_index++;
    }
    
    if (target_sent == NULL) {
        pthread_rwlock_unlock(&file->file_rwlock);
        return ERR_SENTENCE_OUT_OF_RANGE;
    }
    
    // CRITICAL FIX: If sentence doesn't have a delimiter, add period (not newline!)
    // Valid delimiters are: '.' '!' '?'
    if (target_sent->delimiter == '\0') {
        target_sent->delimiter = '.';
        printf("[File Handler] Added period delimiter to sentence %d in file %s\n", 
               sentence_index, filename);
    }
    
    pthread_rwlock_unlock(&file->file_rwlock);
    
    // Note: No need to sync to disk here - will be synced after ETIRW
    return 0;
}

void cleanup_file_handler_ll() {
    pthread_mutex_lock(&file_cache_mutex);
    
    LoadedFile *current = file_cache_head;
    while (current != NULL) {
        LoadedFile *temp = current;
        current = current->next;
        
        free_sentence_list(temp->sentences_head);
        pthread_rwlock_destroy(&temp->file_rwlock);
        free(temp);
    }
    file_cache_head = NULL;
    
    pthread_mutex_unlock(&file_cache_mutex);
    
    printf("File handler cleaned up\n");
}
