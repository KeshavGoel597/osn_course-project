#include "lock_manager.h"
#include "../common/protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Global array of sentence locks
static SentenceLock *locks = NULL;
static int lock_count = 0;
static pthread_mutex_t lock_array_mutex = PTHREAD_MUTEX_INITIALIZER;

// Initialize the lock manager
int init_lock_manager() {
    locks = (SentenceLock*)calloc(MAX_LOCKED_FILES * MAX_SENTENCES_PER_FILE, sizeof(SentenceLock));
    if (locks == NULL) {
        fprintf(stderr, "Failed to allocate memory for lock manager\n");
        return -1;
    }
    lock_count = 0;
    printf("Lock manager initialized\n");
    return 0;
}

// Find or create a lock for a specific sentence
static SentenceLock* get_lock(const char *filename, int sentence_index) {
    pthread_mutex_lock(&lock_array_mutex);
    
    // Search for existing lock
    for (int i = 0; i < lock_count; i++) {
        if (strcmp(locks[i].filename, filename) == 0 && 
            locks[i].sentence_index == sentence_index) {
            pthread_mutex_unlock(&lock_array_mutex);
            return &locks[i];
        }
    }
    
    // Create new lock if not found
    if (lock_count >= MAX_LOCKED_FILES * MAX_SENTENCES_PER_FILE) {
        pthread_mutex_unlock(&lock_array_mutex);
        fprintf(stderr, "Maximum lock limit reached\n");
        return NULL;
    }
    
    SentenceLock *new_lock = &locks[lock_count];
    strncpy(new_lock->filename, filename, sizeof(new_lock->filename) - 1);
    new_lock->sentence_index = sentence_index;
    pthread_mutex_init(&new_lock->lock, NULL);
    new_lock->is_locked = 0;
    memset(new_lock->locked_by, 0, sizeof(new_lock->locked_by));
    lock_count++;
    
    pthread_mutex_unlock(&lock_array_mutex);
    return new_lock;
}

// Lock a specific sentence for writing
int lock_sentence(const char *filename, int sentence_index, const char *username) {
    SentenceLock *sent_lock = get_lock(filename, sentence_index);
    if (sent_lock == NULL) {
        return -1;
    }
    
    pthread_mutex_lock(&sent_lock->lock);
    
    if (sent_lock->is_locked) {
        pthread_mutex_unlock(&sent_lock->lock);
        fprintf(stderr, "Sentence %d in file '%s' is already locked by '%s'\n", 
                sentence_index, filename, sent_lock->locked_by);
        return ERR_SENTENCE_LOCKED;
    }
    
    sent_lock->is_locked = 1;
    strncpy(sent_lock->locked_by, username, sizeof(sent_lock->locked_by) - 1);
    pthread_mutex_unlock(&sent_lock->lock);
    
    printf("Sentence %d in file '%s' locked by user '%s'\n", 
           sentence_index, filename, username);
    return 0;
}

// Unlock a specific sentence
int unlock_sentence(const char *filename, int sentence_index, const char *username) {
    SentenceLock *sent_lock = get_lock(filename, sentence_index);
    if (sent_lock == NULL) {
        return -1;
    }
    
    pthread_mutex_lock(&sent_lock->lock);
    
    if (!sent_lock->is_locked) {
        pthread_mutex_unlock(&sent_lock->lock);
        fprintf(stderr, "Sentence %d in file '%s' is not locked\n", 
                sentence_index, filename);
        return -1;
    }
    
    // Check if the user unlocking is the same as the one who locked it
    if (strcmp(sent_lock->locked_by, username) != 0) {
        pthread_mutex_unlock(&sent_lock->lock);
        fprintf(stderr, "User '%s' cannot unlock sentence locked by '%s'\n", 
                username, sent_lock->locked_by);
        return -1;
    }
    
    sent_lock->is_locked = 0;
    memset(sent_lock->locked_by, 0, sizeof(sent_lock->locked_by));
    pthread_mutex_unlock(&sent_lock->lock);
    
    printf("Sentence %d in file '%s' unlocked by user '%s'\n", 
           sentence_index, filename, username);
    return 0;
}

// Check if a sentence is locked
int is_sentence_locked(const char *filename, int sentence_index) {
    pthread_mutex_lock(&lock_array_mutex);
    
    for (int i = 0; i < lock_count; i++) {
        if (strcmp(locks[i].filename, filename) == 0 && 
            locks[i].sentence_index == sentence_index) {
            int locked = locks[i].is_locked;
            pthread_mutex_unlock(&lock_array_mutex);
            return locked;
        }
    }
    
    pthread_mutex_unlock(&lock_array_mutex);
    return 0;  // Not locked if not found
}

// Cleanup lock manager
void cleanup_lock_manager() {
    if (locks != NULL) {
        for (int i = 0; i < lock_count; i++) {
            pthread_mutex_destroy(&locks[i].lock);
        }
        free(locks);
        locks = NULL;
        lock_count = 0;
    }
    pthread_mutex_destroy(&lock_array_mutex);
    printf("Lock manager cleaned up\n");
}
