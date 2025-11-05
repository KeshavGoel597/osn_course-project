#ifndef FILE_HANDLER_H
#define FILE_HANDLER_H

#include "../common/protocol.h"

// File metadata structure
typedef struct {
    char filename[MAX_FILENAME];
    char owner[MAX_USERNAME];
    char created_time[64];
    char modified_time[64];
    char accessed_time[64];
    long file_size;
    int word_count;
    int char_count;
    // Access control list stored as: "username1:RW,username2:R,..."
    char access_list[MAX_DATA_SIZE];
} FileMetadata;

// Initialize file handler (create directories if needed)
int init_file_handler(const char *storage_dir);

// Create a new file
int create_file(const char *filename, const char *owner);

// Delete a file
int delete_file(const char *filename);

// Read entire file content
int read_file(const char *filename, char *content, int max_size);

// Write to file at specific sentence and word index
int write_to_file(const char *filename, int sentence_index, int word_index, 
                  const char *content, const char *username);

// Undo last change to a file
int undo_file_change(const char *filename);

// Get file metadata
int get_file_metadata(const char *filename, FileMetadata *metadata);

// Update file metadata
int update_file_metadata(const char *filename, FileMetadata *metadata);

// Check if user has read access
int has_read_access(const char *filename, const char *username);

// Check if user has write access
int has_write_access(const char *filename, const char *username);

// Add access to user (READ or WRITE or READ_WRITE)
int add_user_access(const char *filename, const char *username, int access_type);

// Remove user access
int remove_user_access(const char *filename, const char *username);

// Get list of all files in storage
int get_file_list(char *file_list, int max_size);

// Save backup for undo
int save_undo_backup(const char *filename);

// Load all metadata from disk
int load_metadata();

// Save all metadata to disk
int save_metadata();

#endif // FILE_HANDLER_H
