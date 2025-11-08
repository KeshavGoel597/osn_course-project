#ifndef FILE_HANDLER_LL_H
#define FILE_HANDLER_LL_H

#include "../common/protocol.h"
#include <pthread.h>

// Word node - stores a single word
typedef struct WordNode {
    char word[256];              // The actual word
    struct WordNode *next;       // Next word in the sentence
} WordNode;

// Sentence node - stores a sentence (linked list of words)
typedef struct SentenceNode {
    WordNode *words_head;           // Head of word linked list
    char delimiter;                 // '.', '!', '?', or '\0' for last/incomplete sentence
    pthread_mutex_t sentence_lock;  // Lock for this specific sentence
    int is_locked;                  // Flag to indicate if locked
    char locked_by[MAX_USERNAME];   // Username who holds the lock
    struct SentenceNode *next;      // Next sentence
} SentenceNode;

// File structure - represents entire file in memory
typedef struct LoadedFile {
    char filename[MAX_FILENAME];
    SentenceNode *sentences_head;   // Head of sentence linked list
    int sentence_count;
    int is_loaded;                  // Flag: 1 if loaded, 0 if not
    pthread_rwlock_t file_rwlock;   // Reader-writer lock for READ vs WRITE
    struct LoadedFile *next;        // For hash table chaining or linked list
} LoadedFile;

// File metadata structure (same as before, but separate from in-memory structure)
typedef struct {
    char filename[MAX_FILENAME];
    char owner[MAX_USERNAME];
    char created_time[64];
    char modified_time[64];
    char accessed_time[64];
    long file_size;
    int word_count;
    int char_count;
    char access_list[MAX_DATA_SIZE];
} FileMetadata;

// Initialize file handler with linked list support
int init_file_handler_ll(const char *storage_dir);

// Load file from disk into linked list structure (lazy loading)
LoadedFile* load_file_into_memory(const char *filename);

// Unload file from memory (if needed to free memory)
int unload_file_from_memory(const char *filename);

// Get file from cache (loads if not present)
LoadedFile* get_file_from_cache(const char *filename);

// Create a new file
int create_file_ll(const char *filename, const char *owner);

// Delete a file
int delete_file_ll(const char *filename);

// Read entire file content (traverse linked list)
int read_file_ll(const char *filename, char *content, int max_size);

// Write to file at specific sentence and word index (linked list manipulation)
int write_to_file_ll(const char *filename, int sentence_index, int word_index, 
                     const char *content, const char *username);

// Lock a sentence (returns 0 on success, error code on failure)
int lock_sentence_ll(const char *filename, int sentence_index, const char *username);

// Unlock a sentence
int unlock_sentence_ll(const char *filename, int sentence_index, const char *username);

// Force unlock all sentences in a file (for cleanup/debugging)
int force_unlock_all_sentences_ll(const char *filename);

// Save in-memory linked list to disk (via swap file)
int sync_file_to_disk(const char *filename);

// Undo last change to a file
int undo_file_change_ll(const char *filename);

// Get file metadata
int get_file_metadata_ll(const char *filename, FileMetadata *metadata);

// Update file metadata
int update_file_metadata_ll(const char *filename, FileMetadata *metadata);

// Check if user has read access
int has_read_access_ll(const char *filename, const char *username);

// Check if user has write access
int has_write_access_ll(const char *filename, const char *username);

// Helper functions for path management (used by file_write_ll.c)
void get_file_path(const char *filename, char *path, size_t size);
void get_undo_path(const char *filename, char *path, size_t size);

// Add access to user
int add_user_access_ll(const char *filename, const char *username, int access_type);

// Remove user access
int remove_user_access_ll(const char *filename, const char *username);

// Get list of all files
int get_file_list_ll(char *file_list, int max_size);

// Save backup for undo
int save_undo_backup_ll(const char *filename);

// Load all metadata from disk
int load_metadata_ll();

// Save all metadata to disk
int save_metadata_ll();

// Cleanup - free all in-memory structures
void cleanup_file_handler_ll();

#endif // FILE_HANDLER_LL_H
