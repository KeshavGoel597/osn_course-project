#include "file_handler_ll.h"
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

// Helper function to get current timestamp
static void get_timestamp(char *buffer, size_t size) {
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
static WordNode* create_word_node(const char *word) {
    WordNode *node = (WordNode*)malloc(sizeof(WordNode));
    if (node == NULL) return NULL;
    
    strncpy(node->word, word, sizeof(node->word) - 1);
    node->word[sizeof(node->word) - 1] = '\0';
    node->next = NULL;
    return node;
}

// Create sentence node
static SentenceNode* create_sentence_node(char delimiter) {
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
    WordNode *current_word = NULL;
    
    char word_buffer[256];
    int word_idx = 0;
    
    const char *p = content;
    
    while (*p) {
        // Skip leading whitespace
        while (*p && isspace(*p)) p++;
        if (!*p) break;
        
        // Create new sentence if needed
        if (current_sentence == NULL) {
            current_sentence = create_sentence_node('\0');
            if (sentences_head == NULL) {
                sentences_head = current_sentence;
            }
        }
        
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
                
                // Create new sentence for next part
                SentenceNode *new_sentence = create_sentence_node('\0');
                current_sentence->next = new_sentence;
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
    
    // Remove trailing empty sentence if exists
    if (current_sentence != NULL && current_sentence->words_head == NULL) {
        if (sentences_head == current_sentence) {
            // Only sentence and it's empty
            free(current_sentence);
            return NULL;
        } else {
            // Find previous sentence and remove last one
            SentenceNode *prev = sentences_head;
            while (prev->next != current_sentence) {
                prev = prev->next;
            }
            prev->next = NULL;
            free(current_sentence);
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
    SentenceNode *sentences = parse_file_to_linked_list(content);
    free(content);
    
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

// Get file from cache (with lazy loading)
LoadedFile* get_file_from_cache(const char *filename) {
    pthread_mutex_lock(&file_cache_mutex);
    
    // Check if file is already in cache
    LoadedFile *current = file_cache_head;
    while (current != NULL) {
        if (strcmp(current->filename, filename) == 0 && current->is_loaded) {
            pthread_mutex_unlock(&file_cache_mutex);
            return current;
        }
        current = current->next;
    }
    
    // File not in cache, load it
    LoadedFile *loaded = load_file_into_memory(filename);
    if (loaded == NULL) {
        pthread_mutex_unlock(&file_cache_mutex);
        return NULL;
    }
    
    // Add to cache (at head)
    loaded->next = file_cache_head;
    file_cache_head = loaded;
    
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
    
    // Unload from memory if loaded
    unload_file_from_memory(filename);
    
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
    
    printf("File '%s' deleted\n", filename);
    return 0;
}

// Read entire file content by traversing linked list
int read_file_ll(const char *filename, char *content, int max_size) {
    LoadedFile *file = get_file_from_cache(filename);
    if (file == NULL) {
        return ERR_FILE_NOT_FOUND;
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
    
    return 0;
}

// Helper function to write linked list to file
static int write_linked_list_to_file(FILE *fp, SentenceNode *sentences_head) {
    SentenceNode *sent = sentences_head;
    int first_sentence = 1;
    
    while (sent != NULL) {
        // Add space between sentences
        if (!first_sentence) {
            fprintf(fp, " ");
        }
        first_sentence = 0;
        
        // Write words
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

// Lock a sentence
int lock_sentence_ll(const char *filename, int sentence_index, const char *username) {
    LoadedFile *file = get_file_from_cache(filename);
    if (file == NULL) {
        return ERR_FILE_NOT_FOUND;
    }
    
    // Traverse to target sentence
    SentenceNode *sent = file->sentences_head;
    int current_index = 0;
    
    while (sent != NULL && current_index < sentence_index) {
        sent = sent->next;
        current_index++;
    }
    
    if (sent == NULL) {
        return ERR_SENTENCE_OUT_OF_RANGE;
    }
    
    // Check if already locked by someone
    if (sent->is_locked) {
        // If locked by the same user, allow re-lock (idempotent)
        if (strcmp(sent->locked_by, username) == 0) {
            printf("Sentence %d in '%s' already locked by '%s' (re-lock allowed)\n", 
                   sentence_index, filename, username);
            return 0;
        }
        // Locked by different user - try to clean up stale locks
        printf("Sentence %d in '%s' appears locked by '%s', requested by '%s'\n", 
               sentence_index, filename, sent->locked_by, username);
        
        // Try to acquire the mutex to check if it's truly locked
        int trylock_result = pthread_mutex_trylock(&sent->sentence_lock);
        if (trylock_result == 0) {
            // We got the lock! Previous lock was stale (client disconnected)
            printf("[Lock Cleanup] Stale lock detected, cleaning up and granting to '%s'\n", username);
            sent->is_locked = 1;
            strncpy(sent->locked_by, username, MAX_USERNAME - 1);
            sent->locked_by[MAX_USERNAME - 1] = '\0';
            printf("Sentence %d in '%s' locked by '%s' (after cleanup)\n", sentence_index, filename, username);
            return 0;
        } else {
            // Truly locked by another active connection
            printf("[Lock] Sentence is actively locked, access denied\n");
            return ERR_SENTENCE_LOCKED;
        }
    }
    
    // Try to lock (non-blocking)
    if (pthread_mutex_trylock(&sent->sentence_lock) != 0) {
        // Someone else just acquired the lock
        return ERR_SENTENCE_LOCKED;
    }
    
    // Acquire lock
    sent->is_locked = 1;
    strncpy(sent->locked_by, username, MAX_USERNAME - 1);
    sent->locked_by[MAX_USERNAME - 1] = '\0';
    
    printf("Sentence %d in '%s' locked by '%s'\n", sentence_index, filename, username);
    return 0;
}

// Unlock a sentence
int unlock_sentence_ll(const char *filename, int sentence_index, const char *username) {
    LoadedFile *file = get_file_from_cache(filename);
    if (file == NULL) {
        return ERR_FILE_NOT_FOUND;
    }
    
    // Traverse to target sentence
    SentenceNode *sent = file->sentences_head;
    int current_index = 0;
    
    while (sent != NULL && current_index < sentence_index) {
        sent = sent->next;
        current_index++;
    }
    
    if (sent == NULL) {
        return -1;
    }
    
    // Check if locked by this user (or if it's a stale lock, allow cleanup)
    if (!sent->is_locked) {
        printf("Sentence %d in '%s' was not locked\n", sentence_index, filename);
        return 0;  // Already unlocked, return success
    }
    
    if (strcmp(sent->locked_by, username) != 0) {
        printf("Warning: Sentence %d in '%s' locked by '%s' but unlock requested by '%s'\n",
               sentence_index, filename, sent->locked_by, username);
        // Allow unlock anyway for cleanup purposes
    }
    
    // Release lock
    sent->is_locked = 0;
    memset(sent->locked_by, 0, sizeof(sent->locked_by));
    pthread_mutex_unlock(&sent->sentence_lock);
    
    printf("Sentence %d in '%s' unlocked by '%s'\n", sentence_index, filename, username);
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

// Write functions are implemented in file_write_ll.c

// Access control functions (same logic as before)
int has_read_access_ll(const char *filename, const char *username) {
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) return 0;
    
    if (strcmp(meta->owner, username) == 0) return 1;
    
    char search[128];
    snprintf(search, sizeof(search), "%s:R", username);
    if (strstr(meta->access_list, search) != NULL) return 1;
    
    snprintf(search, sizeof(search), "%s:RW", username);
    if (strstr(meta->access_list, search) != NULL) return 1;
    
    return 0;
}

int has_write_access_ll(const char *filename, const char *username) {
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) return 0;
    
    if (strcmp(meta->owner, username) == 0) return 1;
    
    char search[128];
    snprintf(search, sizeof(search), "%s:RW", username);
    if (strstr(meta->access_list, search) != NULL) return 1;
    
    return 0;
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

int add_user_access_ll(const char *filename, const char *username, int access_type) {
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) return ERR_FILE_NOT_FOUND;
    
    pthread_mutex_lock(&metadata_mutex);
    
    // Remove existing access first
    remove_user_access_ll(filename, username);
    
    char access_str[64];
    if (access_type == ACCESS_READ) {
        snprintf(access_str, sizeof(access_str), ",%s:R", username);
    } else {
        snprintf(access_str, sizeof(access_str), ",%s:RW", username);
    }
    
    strncat(meta->access_list, access_str, MAX_DATA_SIZE - strlen(meta->access_list) - 1);
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
        strncat(file_list, metadata_list[i].filename, max_size - strlen(file_list) - 1);
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
