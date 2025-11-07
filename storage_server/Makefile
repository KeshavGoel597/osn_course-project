# Makefile for Storage Server

CC = gcc
CFLAGS = -Wall -Wextra -g -pthread
INCLUDES = -I../common

# Directories
COMMON_DIR = ../common
SS_DIR = .

# Source files
COMMON_SRC = $(COMMON_DIR)/network_utils.c
SS_SRC = storage_server.c file_handler_ll.c file_write_ll.c backup_handler.c ss_nm_comm.c ss_client_comm.c file_handler.c lock_manager.c 

# Object files
COMMON_OBJ = $(COMMON_SRC:.c=.o)
SS_OBJ = $(SS_SRC:.c=.o)

# Output executable
TARGET = storage_server

# Default target
all: $(TARGET)

# Link
$(TARGET): $(COMMON_OBJ) $(SS_OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(COMMON_OBJ) $(SS_OBJ)
	@echo "Storage Server compiled successfully!"

# Compile common files
$(COMMON_DIR)/%.o: $(COMMON_DIR)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Compile storage server files
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Clean
clean:
	rm -f $(SS_OBJ) $(COMMON_OBJ) $(TARGET)
	rm -rf storage_data
	@echo "Cleaned!"

# Create test storage directory
init:
	mkdir -p storage_data

# Run with default parameters (SS_ID=1, primary server)
run: $(TARGET) init
	./$(TARGET) 1 9001 9002 ./storage_data1

# Run backup server (SS_ID=2, backup for SS1)
run-backup: $(TARGET) init
	./$(TARGET) 2 9003 9004 ./storage_data2

.PHONY: all clean init run
