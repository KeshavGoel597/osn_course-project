#ifndef LOCK_MANAGER_H
#define LOCK_MANAGER_H

#include <pthread.h>

// Maximum number of files that can be locked simultaneously
#define MAX_LOCKED_FILES 1000
#define MAX_SENTENCES_PER_FILE 10000

// Sentence lock structure
typedef struct {
    char filename[256];
    int sentence_index;
    pthread_mutex_t lock;
    int is_locked;
    char locked_by[64];  // Username who holds the lock
} SentenceLock;

// Initialize the lock manager
int init_lock_manager();

// Lock a specific sentence for writing
int lock_sentence(const char *filename, int sentence_index, const char *username);

// Unlock a specific sentence
int unlock_sentence(const char *filename, int sentence_index, const char *username);

// Check if a sentence is locked
int is_sentence_locked(const char *filename, int sentence_index);

// Cleanup lock manager
void cleanup_lock_manager();

#endif // LOCK_MANAGER_H
