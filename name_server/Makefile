# Makefile for Name Server (Control Plane)
# OSN Course Project - Distributed File System

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -pthread -g
TARGET = name_server
COMMON_DIR = ../common

# Source files
SOURCES = name_server.c ss_manager.c client_handler.c heartbeat.c fault_tolerance.c
COMMON_SOURCES = $(COMMON_DIR)/network_utils.c

# Object files
OBJECTS = $(SOURCES:.c=.o)
COMMON_OBJECTS = $(COMMON_SOURCES:.c=.o)

# Header files
HEADERS = name_server.h $(COMMON_DIR)/protocol.h $(COMMON_DIR)/network_utils.h

# Default target
all: $(TARGET)

# Build the name server executable
$(TARGET): $(OBJECTS) $(COMMON_OBJECTS)
	@echo "=== Building Name Server ==="
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS) $(COMMON_OBJECTS)
	@echo "Name Server built successfully!"

# Compile source files
%.o: %.c $(HEADERS)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Compile common source files
$(COMMON_DIR)/%.o: $(COMMON_DIR)/%.c $(COMMON_DIR)/%.h
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	@echo "Cleaning Name Server build artifacts..."
	rm -f $(OBJECTS) $(COMMON_OBJECTS) $(TARGET)
	rm -f name_server.log
	@echo "Clean complete!"

# Install (copy to bin directory)
install: $(TARGET)
	@echo "Installing Name Server..."
	mkdir -p ../bin
	cp $(TARGET) ../bin/
	@echo "Name Server installed to ../bin/"

# Run the name server
run: $(TARGET)
	@echo "=== Starting Name Server ==="
	./$(TARGET)

# Debug build
debug: CFLAGS += -DDEBUG -O0
debug: $(TARGET)

# Release build
release: CFLAGS += -O2 -DNDEBUG
release: clean $(TARGET)

# Check for memory leaks with valgrind
valgrind: $(TARGET)
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./$(TARGET)

# Show help
help:
	@echo "Available targets:"
	@echo "  all      - Build the name server (default)"
	@echo "  clean    - Remove build artifacts"
	@echo "  install  - Install to ../bin directory"
	@echo "  run      - Build and run the name server"
	@echo "  debug    - Build with debug flags"
	@echo "  release  - Build optimized release version"
	@echo "  valgrind - Run with memory leak detection"
	@echo "  help     - Show this help message"

# Declare phony targets
.PHONY: all clean install run debug release valgrind help

# Print build information
info:
	@echo "=== Name Server Build Information ==="
	@echo "Compiler: $(CC)"
	@echo "Flags: $(CFLAGS)"
	@echo "Target: $(TARGET)"
	@echo "Sources: $(SOURCES)"
	@echo "Common Sources: $(COMMON_SOURCES)"
	@echo "====================================="
