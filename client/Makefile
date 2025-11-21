# Makefile for Client

CC = gcc
CFLAGS = -Wall -Wextra -g -pthread
INCLUDES = -I../common

# Directories
COMMON_DIR = ../common
CLIENT_DIR = .

# Source files
COMMON_SRC = $(COMMON_DIR)/network_utils.c
CLIENT_SRC = client.c command_parser.c client_nm_comm.c client_ss_comm.c

# Object files
COMMON_OBJ = $(COMMON_SRC:.c=.o)
CLIENT_OBJ = $(CLIENT_SRC:.c=.o)

# Output executable
TARGET = client

# Default target
all: $(TARGET)

# Link
$(TARGET): $(COMMON_OBJ) $(CLIENT_OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(COMMON_OBJ) $(CLIENT_OBJ)

# Compile common files
$(COMMON_DIR)/%.o: $(COMMON_DIR)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Compile client files
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Clean
clean:
	rm -f $(CLIENT_OBJ) $(COMMON_OBJ) $(TARGET)

# Run with default parameters (prompts for username)
run: $(TARGET)
	./$(TARGET)

# Run with username argument
run-user: $(TARGET)
	./$(TARGET) testuser

.PHONY: all clean run run-user
