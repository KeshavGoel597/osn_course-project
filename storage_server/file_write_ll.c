#include "storage_server_all.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

// Forward declarations
extern LoadedFile* get_file_from_cache(const char *filename);
extern int sync_file_to_disk(const char *filename);
extern int reload_file_from_disk(const char *filename);

// Helper: Check if character is a sentence delimiter
static int is_delimiter(char c) {
    return (c == '.' || c == '!' || c == '?');
}

// Helper: Count words in a sentence
static int count_words_in_sentence(SentenceNode *sent) {
    int count = 0;
    WordNode *word = sent->words_head;
    while (word != NULL) {
        count++;
        word = word->next;
    }
    return count;
}

// Helper: Get word at specific index
static WordNode* get_word_at_index(SentenceNode *sent, int index, WordNode **prev) {
    *prev = NULL;
    WordNode *word = sent->words_head;
    int current = 0;
    
    while (word != NULL && current < index) {
        *prev = word;
        word = word->next;
        current++;
    }
    
    return word;
}

// Helper: Split content by delimiters, returns array of word groups
// Each group becomes a separate sentence
typedef struct {
    char words[100][256];  // Words in this group
    int word_count;
    char delimiter;        // Delimiter at end of this group
} WordGroup;

static int split_content_into_groups(const char *content, WordGroup *groups, int max_groups) {
    int group_count = 0;
    groups[group_count].word_count = 0;
    groups[group_count].delimiter = '\0';
    
    char word_buffer[256];
    int word_idx = 0;
    const char *p = content;
    
    while (*p && group_count < max_groups) {
        // Skip whitespace
        while (*p && isspace(*p)) p++;
        if (!*p) break;
        
        // Read word
        word_idx = 0;
        while (*p && !isspace(*p) && word_idx < 255) {
            word_buffer[word_idx++] = *p++;
        }
        word_buffer[word_idx] = '\0';
        
        if (word_idx == 0) continue;
        
        // Process word character by character to handle multiple delimiters
        // Each delimiter creates a new sentence, even in "e.g." -> "e." and "g."
        int start_pos = 0;
        for (int i = 0; i < word_idx; i++) {
            if (is_delimiter(word_buffer[i])) {
                // Found delimiter - add text up to and including delimiter
                int len = i - start_pos + 1;
                char part[256];
                strncpy(part, word_buffer + start_pos, len);
                part[len] = '\0';
                
                strcpy(groups[group_count].words[groups[group_count].word_count], part);
                groups[group_count].word_count++;
                groups[group_count].delimiter = word_buffer[i];
                
                // Start new group for next sentence
                group_count++;
                if (group_count >= max_groups) break;
                groups[group_count].word_count = 0;
                groups[group_count].delimiter = '\0';
                
                start_pos = i + 1;
            }
        }
        
        // Add any remaining part after last delimiter (or whole word if no delimiter)
        if (start_pos < word_idx && group_count < max_groups) {
            char remaining[256];
            strcpy(remaining, word_buffer + start_pos);
            strcpy(groups[group_count].words[groups[group_count].word_count], remaining);
            groups[group_count].word_count++;
        }
    }
    
    // If last group has words, count it
    if (groups[group_count].word_count > 0) {
        group_count++;
    }
    
    return group_count;
}

// Main write function - insert content at specific sentence and word index
int write_to_file_ll(const char *filename, int sentence_index, int word_index, 
                     const char *content, const char *username) {
    (void)username;  // Not used in current implementation
    
    LoadedFile *file = get_file_from_cache(filename);
    if (file == NULL) {
        return ERR_FILE_NOT_FOUND;
    }
    
    pthread_rwlock_wrlock(&file->file_rwlock);
    
    // Traverse to target sentence
    SentenceNode *target_sent = file->sentences_head;
    SentenceNode *prev_sent = NULL;
    int current_index = 0;
    
    while (target_sent != NULL && current_index < sentence_index) {
        prev_sent = target_sent;
        target_sent = target_sent->next;
        current_index++;
    }
    
    // Check if sentence index is valid (can be sentence_count for appending)
    printf("[DEBUG] Write request: sentence_index=%d, file->sentence_count=%d\n", 
           sentence_index, file->sentence_count);
    if (sentence_index < 0 || sentence_index > file->sentence_count) {
        printf("[DEBUG] Sentence index out of range: %d > %d\n", sentence_index, file->sentence_count);
        pthread_rwlock_unlock(&file->file_rwlock);
        file_release(file);  // CRITICAL FIX: Release reference
        return ERR_SENTENCE_OUT_OF_RANGE;
    }
    
    // CRITICAL FIX: For new sentences, word_index must be 0 (can't write to middle of non-existent sentence)
    // Check this BEFORE creating the sentence to avoid leaving empty sentences on error
    if (target_sent == NULL && word_index != 0) {
        fprintf(stderr, "[DEBUG] Cannot write at word_index %d in new sentence (must be 0)\n", word_index);
        pthread_rwlock_unlock(&file->file_rwlock);
        file_release(file);
        return ERR_WORD_OUT_OF_RANGE;
    }
    
    // If appending new sentence
    if (target_sent == NULL) {
        target_sent = create_sentence_node('\0');
        // CRITICAL FIX: Check malloc success
        if (target_sent == NULL) {
            fprintf(stderr, "[Write LL] Failed to allocate new sentence node\n");
            pthread_rwlock_unlock(&file->file_rwlock);
            file_release(file);  // CRITICAL FIX: Release reference
            return -1;
        }
        if (prev_sent != NULL) {
            prev_sent->next = target_sent;
        } else {
            file->sentences_head = target_sent;
        }
        file->sentence_count++;
    }
    
    // Count words in target sentence
    int word_count = count_words_in_sentence(target_sent);
    
    // Check if word index is valid (for existing sentences)
    if (word_index < 0 || word_index > word_count) {
        pthread_rwlock_unlock(&file->file_rwlock);
        file_release(file);  // CRITICAL FIX: Release reference
        return ERR_WORD_OUT_OF_RANGE;
    }
    
    // Parse content into word groups (may contain multiple sentences)
    WordGroup groups[100];
    int group_count = split_content_into_groups(content, groups, 100);
    
    if (group_count == 0) {
        pthread_rwlock_unlock(&file->file_rwlock);
        file_release(file);  // CRITICAL FIX: Release reference
        return 0;  // Nothing to insert
    }
    
    // Insert first group into current sentence at word_index
    WordNode *prev_word = NULL;
    WordNode *insert_point = get_word_at_index(target_sent, word_index, &prev_word);
    (void)insert_point; // Suppress unused variable warning
    
    // Insert words from first group
    WordNode *last_inserted = prev_word;
    for (int i = 0; i < groups[0].word_count; i++) {
        WordNode *new_word = create_word_node(groups[0].words[i]);
        if (new_word == NULL) {
            // CRITICAL: malloc failed - linked list is now partially modified!
            // Caller (handle_write_request) will rollback from undo backup
            fprintf(stderr, "[Write LL] CRITICAL: malloc failed for word node (word %d/%d)\n", 
                    i, groups[0].word_count);
            fprintf(stderr, "[Write LL] Linked list partially modified - requires rollback\n");
            pthread_rwlock_unlock(&file->file_rwlock);
            file_release(file);  // CRITICAL FIX: Release reference
            return -1;
        }
        
        if (last_inserted == NULL) {
            // Insert at head
            new_word->next = target_sent->words_head;
            target_sent->words_head = new_word;
        } else {
            // Insert after last_inserted
            new_word->next = last_inserted->next;
            last_inserted->next = new_word;
        }
        last_inserted = new_word;
    }
    
    // If first group has delimiter, update target sentence delimiter
    if (groups[0].delimiter != '\0') {
        target_sent->delimiter = groups[0].delimiter;
        
        // CRITICAL FIX: If we just added a delimiter to the target sentence,
        // we need to create an empty trailing sentence (unless there are more groups)
        if (group_count == 1) {
            SentenceNode *empty_sent = create_sentence_node('\0');
            if (empty_sent == NULL) {
                fprintf(stderr, "[Write LL] CRITICAL: malloc failed for trailing empty sentence\n");
                pthread_rwlock_unlock(&file->file_rwlock);
                file_release(file);
                return -1;
            }
            empty_sent->next = target_sent->next;
            target_sent->next = empty_sent;
            file->sentence_count++;
        }
    }
    
    // If there are additional groups, create new sentences
    if (group_count > 1) {
        // Remaining words from target sentence go to last new sentence
        WordNode *remaining_words = (last_inserted != NULL) ? last_inserted->next : target_sent->words_head;
        if (last_inserted != NULL) {
            last_inserted->next = NULL;
        } else if (remaining_words == target_sent->words_head) {
            target_sent->words_head = NULL;
        }
        
        // Create new sentences for additional groups
        SentenceNode *current_new_sent = target_sent;
        
        for (int g = 1; g < group_count; g++) {
            SentenceNode *new_sent = create_sentence_node(groups[g].delimiter);
            if (new_sent == NULL) {
                fprintf(stderr, "[Write LL] CRITICAL: malloc failed for sentence node (sentence %d/%d)\n", 
                        g, group_count);
                fprintf(stderr, "[Write LL] Linked list partially modified - requires rollback\n");
                pthread_rwlock_unlock(&file->file_rwlock);
                file_release(file);  // CRITICAL FIX: Release reference
                return -1;
            }
            
            // Add words to new sentence
            WordNode *last_word = NULL;
            for (int w = 0; w < groups[g].word_count; w++) {
                WordNode *new_word = create_word_node(groups[g].words[w]);
                if (new_word == NULL) {
                    fprintf(stderr, "[Write LL] CRITICAL: malloc failed for word node in new sentence\n");
                    fprintf(stderr, "[Write LL] Linked list partially modified - requires rollback\n");
                    pthread_rwlock_unlock(&file->file_rwlock);
                    file_release(file);  // CRITICAL FIX: Release reference
                    return -1;
                }
                
                if (last_word == NULL) {
                    new_sent->words_head = new_word;
                } else {
                    last_word->next = new_word;
                }
                last_word = new_word;
            }
            
            // If this is the last new sentence, append remaining words
            if (g == group_count - 1 && remaining_words != NULL) {
                if (last_word == NULL) {
                    new_sent->words_head = remaining_words;
                } else {
                    last_word->next = remaining_words;
                }
                // Keep original delimiter if last group doesn't have one
                if (new_sent->delimiter == '\0') {
                    // Find delimiter from original target sentence
                    SentenceNode *next_orig = target_sent->next;
                    if (next_orig != target_sent) {
                        new_sent->delimiter = target_sent->delimiter;
                    }
                }
            }
            
            // Link new sentence after current
            new_sent->next = current_new_sent->next;
            current_new_sent->next = new_sent;
            current_new_sent = new_sent;
            file->sentence_count++;
        }
    }
    
    pthread_rwlock_unlock(&file->file_rwlock);
    file_release(file);  // CRITICAL FIX: Release reference
    
    // NOTE: Do NOT sync to disk here - sync happens only after ETIRW
    // This ensures atomicity: other clients don't see partial writes
    
    printf("Write completed in memory: %s at sentence %d, word %d\n", filename, sentence_index, word_index);
    return 0;
}

// Forward declarations for helper functions
extern void get_file_path(const char *filename, char *path, size_t size);
extern void get_undo_path(const char *filename, char *path, size_t size);
extern int unload_file_from_memory(const char *filename);

// Global mutex for UNDO operations to prevent concurrent access
static pthread_mutex_t undo_mutex = PTHREAD_MUTEX_INITIALIZER;

// Undo implementation
int undo_file_change_ll(const char *filename) {
    // CRITICAL FIX: Lock to prevent concurrent UNDO operations on same file
    pthread_mutex_lock(&undo_mutex);
    
    char filepath[MAX_PATH];
    char undo_path[MAX_PATH];
    get_file_path(filename, filepath, MAX_PATH);
    get_undo_path(filename, undo_path, MAX_PATH);
    
    // Check if undo backup exists
    FILE *undo_fp = fopen(undo_path, "r");
    if (undo_fp == NULL) {
        fprintf(stderr, "No undo history for file '%s'\n", filename);
        pthread_mutex_unlock(&undo_mutex);
        return -1;
    }
    fclose(undo_fp);
    
    // Replace current file with undo version
    FILE *src = fopen(undo_path, "r");
    FILE *dst = fopen(filepath, "w");
    
    if (src == NULL || dst == NULL) {
        if (src) fclose(src);
        if (dst) fclose(dst);
        fprintf(stderr, "Failed to restore undo backup for '%s'\n", filename);
        pthread_mutex_unlock(&undo_mutex);
        return -1;
    }
    
    char buffer[4096];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        fwrite(buffer, 1, bytes, dst);
    }
    
    fclose(src);
    fclose(dst);
    
    // CRITICAL FIX: After UNDO, reload the file from disk to keep cache consistent
    // This safely replaces the old cached version with the restored content
    if (reload_file_from_disk(filename) < 0) {
        fprintf(stderr, "[UNDO] Warning: Failed to reload file '%s' after undo\n", filename);
    }
    
    pthread_mutex_unlock(&undo_mutex);
    
    printf("Undo completed for '%s', restored to previous version\n", filename);
    return 0;
}

// Save backup for undo
int save_undo_backup_ll(const char *filename) {
    // Simply sync current in-memory state to undo file
    LoadedFile *file = get_file_from_cache(filename);
    if (file == NULL) {
        return -1;
    }
    
    char undo_path[MAX_PATH];
    get_undo_path(filename, undo_path, MAX_PATH);
    
    // CRITICAL FIX: Create parent directories for undo file if needed
    // For files in folders like "myfolder/file.txt", we need "undo/myfolder/" to exist
    char undo_dir_path[MAX_PATH];
    strncpy(undo_dir_path, undo_path, MAX_PATH - 1);
    char *last_slash = strrchr(undo_dir_path, '/');
    if (last_slash != NULL) {
        *last_slash = '\0';  // Truncate to get directory path
        // Create directory recursively (mkdir -p behavior)
        char temp_path[MAX_PATH];
        char *p = undo_dir_path;
        
        // Skip leading slash if present
        if (*p == '/') p++;
        
        for (char *ptr = p; *ptr; ptr++) {
            if (*ptr == '/') {
                *ptr = '\0';
                snprintf(temp_path, MAX_PATH, "%s", undo_dir_path);
                mkdir(temp_path, 0755);  // Ignore errors if already exists
                *ptr = '/';
            }
        }
        mkdir(undo_dir_path, 0755);  // Create final directory
    }
    
    pthread_rwlock_rdlock(&file->file_rwlock);
    
    FILE *fp = fopen(undo_path, "w");
    if (fp == NULL) {
        pthread_rwlock_unlock(&file->file_rwlock);
        fprintf(stderr, "Failed to create undo backup at '%s': %s\n", undo_path, strerror(errno));
        return -1;
    }
    
    // Write current state to undo file
    SentenceNode *sent = file->sentences_head;
    int first_sentence = 1;
    
    while (sent != NULL) {
        if (!first_sentence) fprintf(fp, " ");
        first_sentence = 0;
        
        WordNode *word = sent->words_head;
        int first_word = 1;
        while (word != NULL) {
            if (!first_word) fprintf(fp, " ");
            first_word = 0;
            fprintf(fp, "%s", word->word);
            word = word->next;
        }
        
        sent = sent->next;
    }
    
    fclose(fp);
    pthread_rwlock_unlock(&file->file_rwlock);
    
    printf("Undo backup created for '%s'\n", filename);
    return 0;
}
