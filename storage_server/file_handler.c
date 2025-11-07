#include "file_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

// Global storage directory
static char storage_dir[MAX_PATH];
static char files_dir[MAX_PATH];
static char undo_dir[MAX_PATH];
static char metadata_file[MAX_PATH];

// In-memory metadata (array of file metadata)
#define MAX_FILES 1000
static FileMetadata metadata_list[MAX_FILES];
static int metadata_count = 0;

// Helper function to get current timestamp
static void get_timestamp(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", t);
}

// Helper function to get file path
static void get_file_path(const char *filename, char *path, size_t size) {
    snprintf(path, size, "%s/%s", files_dir, filename);
}

// Helper function to get undo file path
static void get_undo_path(const char *filename, char *path, size_t size) {
    snprintf(path, size, "%s/%s.undo", undo_dir, filename);
}

// Initialize file handler (create directories if needed)
int init_file_handler(const char *storage_directory) {
    strncpy(storage_dir, storage_directory, MAX_PATH - 1);
    snprintf(files_dir, MAX_PATH, "%s/files", storage_dir);
    snprintf(undo_dir, MAX_PATH, "%s/undo", storage_dir);
    snprintf(metadata_file, MAX_PATH, "%s/metadata.txt", storage_dir);
    
    // Create directories
    mkdir(storage_dir, 0755);
    mkdir(files_dir, 0755);
    mkdir(undo_dir, 0755);
    
    printf("File handler initialized\n");
    printf("  Files directory: %s\n", files_dir);
    printf("  Undo directory: %s\n", undo_dir);
    printf("  Metadata file: %s\n", metadata_file);
    
    return 0;
}

// Find metadata for a file
static FileMetadata* find_metadata(const char *filename) {
    for (int i = 0; i < metadata_count; i++) {
        if (strcmp(metadata_list[i].filename, filename) == 0) {
            return &metadata_list[i];
        }
    }
    return NULL;
}

// Create a new file
int create_file(const char *filename, const char *owner) {
    // Check if file already exists
    if (find_metadata(filename) != NULL) {
        fprintf(stderr, "File '%s' already exists\n", filename);
        return ERR_FILE_EXISTS;
    }
    
    if (metadata_count >= MAX_FILES) {
        fprintf(stderr, "Maximum file limit reached\n");
        return -1;
    }
    
    // Create the file
    char filepath[MAX_PATH];
    get_file_path(filename, filepath, MAX_PATH);
    
    FILE *fp = fopen(filepath, "w");
    if (fp == NULL) {
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
    
    // Owner has read-write access
    snprintf(meta->access_list, MAX_DATA_SIZE, "%s:RW", owner);
    
    metadata_count++;
    
    // Save metadata to disk
    save_metadata();
    
    printf("File '%s' created by owner '%s'\n", filename, owner);
    return 0;
}

// Delete a file
int delete_file(const char *filename) {
    // Find metadata
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) {
        fprintf(stderr, "File '%s' not found\n", filename);
        return ERR_FILE_NOT_FOUND;
    }
    
    // Delete the file
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
    int index = meta - metadata_list;
    for (int i = index; i < metadata_count - 1; i++) {
        metadata_list[i] = metadata_list[i + 1];
    }
    metadata_count--;
    
    // Save metadata
    save_metadata();
    
    printf("File '%s' deleted\n", filename);
    return 0;
}

// Read entire file content
int read_file(const char *filename, char *content, int max_size) {
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) {
        fprintf(stderr, "File '%s' not found\n", filename);
        return ERR_FILE_NOT_FOUND;
    }
    
    char filepath[MAX_PATH];
    get_file_path(filename, filepath, MAX_PATH);
    
    FILE *fp = fopen(filepath, "r");
    if (fp == NULL) {
        perror("Failed to open file");
        return -1;
    }
    
    // Read entire file
    size_t bytes_read = fread(content, 1, max_size - 1, fp);
    content[bytes_read] = '\0';
    
    fclose(fp);
    
    // Update access time
    get_timestamp(meta->accessed_time, sizeof(meta->accessed_time));
    
    return 0;
}

// Helper: Count words and characters
static void count_words_chars(const char *content, int *words, int *chars) {
    *words = 0;
    *chars = strlen(content);
    
    int in_word = 0;
    for (int i = 0; content[i]; i++) {
        if (content[i] == ' ' || content[i] == '\t' || content[i] == '\n' || content[i] == '\r') {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            (*words)++;
        }
    }
}

// Helper: Update file statistics
static void update_file_stats(const char *filename) {
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) return;
    
    char filepath[MAX_PATH];
    get_file_path(filename, filepath, MAX_PATH);
    
    // Get file size
    struct stat st;
    if (stat(filepath, &st) == 0) {
        meta->file_size = st.st_size;
    }
    
    // Read file and count words/chars
    FILE *fp = fopen(filepath, "r");
    if (fp != NULL) {
        char content[MAX_DATA_SIZE];
        size_t bytes_read = fread(content, 1, MAX_DATA_SIZE - 1, fp);
        content[bytes_read] = '\0';
        fclose(fp);
        
        count_words_chars(content, &meta->word_count, &meta->char_count);
    }
    
    get_timestamp(meta->modified_time, sizeof(meta->modified_time));
}

// Write to file at specific sentence and word index
int write_to_file(const char *filename, int sentence_index, int word_index, 
                  const char *content, const char *username) {
    (void)username; // Suppress unused parameter warning
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) {
        return ERR_FILE_NOT_FOUND;
    }
    
    char filepath[MAX_PATH];
    get_file_path(filename, filepath, MAX_PATH);
    
    // Read entire file
    FILE *fp = fopen(filepath, "r");
    if (fp == NULL) {
        perror("Failed to open file");
        return -1;
    }
    
    char file_content[MAX_DATA_SIZE];
    size_t bytes_read = fread(file_content, 1, MAX_DATA_SIZE - 1, fp);
    file_content[bytes_read] = '\0';
    fclose(fp);
    
    // Split into sentences
    char sentences[1000][MAX_DATA_SIZE];
    int sentence_count = 0;
    
    char *start = file_content;
    char *end;
    
    while (*start && sentence_count < 1000) {
        // Find end of sentence
        end = start;
        while (*end && *end != '.' && *end != '!' && *end != '?') {
            end++;
        }
        
        if (*end == '\0') {
            // Last part without delimiter
            if (end > start && *(end - 1) != ' ') {
                strncpy(sentences[sentence_count], start, end - start);
                sentences[sentence_count][end - start] = '\0';
                sentence_count++;
            }
            break;
        } else {
            // Include the delimiter
            end++;
            strncpy(sentences[sentence_count], start, end - start);
            sentences[sentence_count][end - start] = '\0';
            sentence_count++;
            start = end;
            
            // Skip whitespace
            while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') {
                start++;
            }
        }
    }
    
    // Check if sentence index is valid
    if (sentence_index < 0 || sentence_index > sentence_count) {
        fprintf(stderr, "Sentence index %d out of range (0-%d)\n", sentence_index, sentence_count);
        return ERR_SENTENCE_OUT_OF_RANGE;
    }
    
    // Get the target sentence (or create empty if appending)
    char target_sentence[MAX_DATA_SIZE];
    if (sentence_index == sentence_count) {
        strcpy(target_sentence, "");
    } else {
        strcpy(target_sentence, sentences[sentence_index]);
    }
    
    // Split sentence into words
    char words[1000][256];
    int word_count = 0;
    
    char *word_token = strtok(target_sentence, " \t\n\r");
    while (word_token != NULL && word_count < 1000) {
        strcpy(words[word_count], word_token);
        word_count++;
        word_token = strtok(NULL, " \t\n\r");
    }
    
    // Check if word index is valid (can be word_count for appending)
    if (word_index < 0 || word_index > word_count + 1) {
        fprintf(stderr, "Word index %d out of range (0-%d)\n", word_index, word_count + 1);
        return ERR_WORD_OUT_OF_RANGE;
    }
    
    // Insert content at word_index
    // Shift words to make room
    for (int i = word_count; i > word_index; i--) {
        strcpy(words[i], words[i - 1]);
    }
    strcpy(words[word_index], content);
    word_count++;
    
    // Reconstruct the sentence
    char new_sentence[MAX_DATA_SIZE] = "";
    for (int i = 0; i < word_count; i++) {
        if (i > 0) strcat(new_sentence, " ");
        strcat(new_sentence, words[i]);
    }
    
    // Check if new content contains sentence delimiters
    // If it does, split into multiple sentences
    char temp_sentences[100][MAX_DATA_SIZE];
    int temp_count = 0;
    
    char *s_start = new_sentence;
    char *s_end;
    
    while (*s_start && temp_count < 100) {
        s_end = s_start;
        while (*s_end && *s_end != '.' && *s_end != '!' && *s_end != '?') {
            s_end++;
        }
        
        if (*s_end == '\0') {
            strcpy(temp_sentences[temp_count], s_start);
            temp_count++;
            break;
        } else {
            s_end++;
            strncpy(temp_sentences[temp_count], s_start, s_end - s_start);
            temp_sentences[temp_count][s_end - s_start] = '\0';
            temp_count++;
            s_start = s_end;
            while (*s_start == ' ') s_start++;
        }
    }
    
    // Replace the original sentence with new sentence(s)
    char final_content[MAX_DATA_SIZE] = "";
    
    // Add sentences before target
    for (int i = 0; i < sentence_index && i < sentence_count; i++) {
        if (i > 0) strcat(final_content, " ");
        strcat(final_content, sentences[i]);
    }
    
    // Add the modified sentence(s)
    for (int i = 0; i < temp_count; i++) {
        if (final_content[0]) strcat(final_content, " ");
        strcat(final_content, temp_sentences[i]);
    }
    
    // Add sentences after target
    for (int i = sentence_index + 1; i < sentence_count; i++) {
        if (final_content[0]) strcat(final_content, " ");
        strcat(final_content, sentences[i]);
    }
    
    // Write back to file
    fp = fopen(filepath, "w");
    if (fp == NULL) {
        perror("Failed to write file");
        return -1;
    }
    
    fwrite(final_content, 1, strlen(final_content), fp);
    fclose(fp);
    
    // Update file stats
    update_file_stats(filename);
    save_metadata();
    
    printf("Write completed: %s at sentence %d, word %d\n", filename, sentence_index, word_index);
    return 0;
}

// Save backup for undo
int save_undo_backup(const char *filename) {
    char filepath[MAX_PATH];
    char undo_path[MAX_PATH];
    get_file_path(filename, filepath, MAX_PATH);
    get_undo_path(filename, undo_path, MAX_PATH);
    
    // Copy file to undo location
    FILE *src = fopen(filepath, "r");
    if (src == NULL) {
        perror("Failed to open source file for undo backup");
        return -1;
    }
    
    FILE *dst = fopen(undo_path, "w");
    if (dst == NULL) {
        perror("Failed to create undo backup");
        fclose(src);
        return -1;
    }
    
    char buffer[4096];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        fwrite(buffer, 1, bytes, dst);
    }
    
    fclose(src);
    fclose(dst);
    
    printf("Undo backup created for '%s'\n", filename);
    return 0;
}

// Undo last change to a file
int undo_file_change(const char *filename) {
    char filepath[MAX_PATH];
    char undo_path[MAX_PATH];
    get_file_path(filename, filepath, MAX_PATH);
    get_undo_path(filename, undo_path, MAX_PATH);
    
    // Check if undo backup exists
    FILE *undo_fp = fopen(undo_path, "r");
    if (undo_fp == NULL) {
        fprintf(stderr, "No undo history for file '%s'\n", filename);
        return -1;
    }
    fclose(undo_fp);
    
    // Copy undo file back to original
    FILE *src = fopen(undo_path, "r");
    FILE *dst = fopen(filepath, "w");
    
    if (src == NULL || dst == NULL) {
        perror("Failed to perform undo");
        if (src) fclose(src);
        if (dst) fclose(dst);
        return -1;
    }
    
    char buffer[4096];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        fwrite(buffer, 1, bytes, dst);
    }
    
    fclose(src);
    fclose(dst);
    
    // Update file stats
    update_file_stats(filename);
    save_metadata();
    
    printf("Undo completed for '%s'\n", filename);
    return 0;
}

// Get file metadata
int get_file_metadata(const char *filename, FileMetadata *metadata) {
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) {
        return ERR_FILE_NOT_FOUND;
    }
    
    memcpy(metadata, meta, sizeof(FileMetadata));
    return 0;
}

// Update file metadata
int update_file_metadata(const char *filename, FileMetadata *metadata) {
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) {
        return ERR_FILE_NOT_FOUND;
    }
    
    memcpy(meta, metadata, sizeof(FileMetadata));
    save_metadata();
    return 0;
}

// Check if user has read access
int has_read_access(const char *filename, const char *username) {
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) {
        return 0;
    }
    
    // Owner always has access
    if (strcmp(meta->owner, username) == 0) {
        return 1;
    }
    
    // Check access list
    char search[128];
    snprintf(search, sizeof(search), "%s:R", username);
    if (strstr(meta->access_list, search) != NULL) {
        return 1;
    }
    
    snprintf(search, sizeof(search), "%s:RW", username);
    if (strstr(meta->access_list, search) != NULL) {
        return 1;
    }
    
    return 0;
}

// Check if user has write access
int has_write_access(const char *filename, const char *username) {
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) {
        return 0;
    }
    
    // Owner always has access
    if (strcmp(meta->owner, username) == 0) {
        return 1;
    }
    
    // Check access list for write access
    char search[128];
    snprintf(search, sizeof(search), "%s:RW", username);
    if (strstr(meta->access_list, search) != NULL) {
        return 1;
    }
    
    return 0;
}

// Add access to user
int add_user_access(const char *filename, const char *username, int access_type) {
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) {
        return ERR_FILE_NOT_FOUND;
    }
    
    // Remove existing access first (if any)
    remove_user_access(filename, username);
    
    // Add new access
    char access_str[64];
    if (access_type == ACCESS_READ) {
        snprintf(access_str, sizeof(access_str), ",%s:R", username);
    } else if (access_type == ACCESS_WRITE || access_type == ACCESS_READ_WRITE) {
        snprintf(access_str, sizeof(access_str), ",%s:RW", username);
    } else {
        return -1;
    }
    
    strncat(meta->access_list, access_str, MAX_DATA_SIZE - strlen(meta->access_list) - 1);
    save_metadata();
    
    printf("Access added for user '%s' to file '%s'\n", username, filename);
    return 0;
}

// Remove user access
int remove_user_access(const char *filename, const char *username) {
    FileMetadata *meta = find_metadata(filename);
    if (meta == NULL) {
        return ERR_FILE_NOT_FOUND;
    }
    
    // Search for user in access list
    char search_rw[128], search_r[128];
    snprintf(search_rw, sizeof(search_rw), "%s:RW", username);
    snprintf(search_r, sizeof(search_r), "%s:R", username);
    
    char *pos_rw = strstr(meta->access_list, search_rw);
    char *pos_r = strstr(meta->access_list, search_r);
    char *pos = pos_rw ? pos_rw : pos_r;
    
    if (pos != NULL) {
        // Find comma before or after
        char *comma_before = (pos > meta->access_list && *(pos - 1) == ',') ? pos - 1 : NULL;
        char *comma_after = strchr(pos, ',');
        
        if (comma_before) {
            // Remove ", username:X"
            if (comma_after) {
                memmove(comma_before, comma_after, strlen(comma_after) + 1);
            } else {
                *comma_before = '\0';
            }
        } else if (comma_after) {
            // Remove "username:X,"
            memmove(pos, comma_after + 1, strlen(comma_after + 1) + 1);
        } else {
            // Only entry, just remove it
            *pos = '\0';
        }
        
        save_metadata();
        printf("Access removed for user '%s' from file '%s'\n", username, filename);
    }
    
    return 0;
}

// Get list of all files in storage
int get_file_list(char *file_list, int max_size) {
    file_list[0] = '\0';
    
    for (int i = 0; i < metadata_count; i++) {
        if (i > 0) {
            strncat(file_list, ",", max_size - strlen(file_list) - 1);
        }
        strncat(file_list, metadata_list[i].filename, max_size - strlen(file_list) - 1);
    }
    
    return 0;
}

// Load all metadata from disk
int load_metadata() {
    FILE *fp = fopen(metadata_file, "r");
    if (fp == NULL) {
        fprintf(stderr, "No existing metadata file (first run?)\n");
        return -1;
    }
    
    metadata_count = 0;
    while (metadata_count < MAX_FILES && 
           fread(&metadata_list[metadata_count], sizeof(FileMetadata), 1, fp) == 1) {
        metadata_count++;
    }
    
    fclose(fp);
    printf("Loaded %d file metadata entries\n", metadata_count);
    return 0;
}

// Save all metadata to disk
int save_metadata() {
    FILE *fp = fopen(metadata_file, "w");
    if (fp == NULL) {
        perror("Failed to save metadata");
        return -1;
    }
    
    for (int i = 0; i < metadata_count; i++) {
        fwrite(&metadata_list[i], sizeof(FileMetadata), 1, fp);
    }
    
    fclose(fp);
    return 0;
}
