#!/bin/bash

# Distributed File System Test Script
# This script helps automate basic testing of the DFS components

set -e  # Exit on any error

echo "=== Distributed File System Test Script ==="
echo "This script will help you test the DFS components step by step."
echo

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

print_step() {
    echo -e "${BLUE}=== $1 ===${NC}"
}

print_success() {
    echo -e "${GREEN}✅ $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}⚠️  $1${NC}"
}

print_error() {
    echo -e "${RED}❌ $1${NC}"
}

# Function to check if a process is running on a port
check_port() {
    if lsof -Pi :$1 -sTCP:LISTEN -t >/dev/null 2>&1; then
        return 0
    else
        return 1
    fi
}

# Function to wait for user input
wait_for_user() {
    echo -e "${YELLOW}Press Enter to continue...${NC}"
    read
}

print_step "Step 1: Environment Setup"

# Create necessary directories
echo "Creating storage directories..."
mkdir -p storage_data1 storage_data2 storage_data3 storage_data4
mkdir -p test_files
print_success "Storage directories created"

# Check if binaries exist
print_step "Step 2: Binary Check"

if [ ! -f "name_server/name_server" ]; then
    print_error "Name Server binary not found. Building..."
    cd name_server && make clean && make && cd ..
    print_success "Name Server built"
else
    print_success "Name Server binary found"
fi

if [ ! -f "storage server/storage_server" ]; then
    print_error "Storage Server binary not found. Building..."
    cd "storage server" && make clean && make && cd ..
    print_success "Storage Server built"
else
    print_success "Storage Server binary found"
fi

if [ ! -f "client/client" ]; then
    print_error "Client binary not found. Building..."
    cd client && make clean && make && cd ..
    print_success "Client built"
else
    print_success "Client binary found"
fi

print_step "Step 3: Port Availability Check"

ports=(8080 9001 9002 9003 9004 9005 9006 9007 9008)
for port in "${ports[@]}"; do
    if check_port $port; then
        print_warning "Port $port is already in use. Please stop the process using it."
        echo "You can find the process with: lsof -i :$port"
        echo "And kill it with: kill -9 <PID>"
        exit 1
    else
        echo "Port $port is available ✓"
    fi
done

print_success "All required ports are available"

print_step "Step 4: Manual Testing Instructions"

echo "Now you need to start the components manually in separate terminals."
echo "Follow these steps:"
echo

echo -e "${BLUE}Terminal 1 - Name Server:${NC}"
echo "cd name_server"
echo "./name_server"
echo

echo -e "${BLUE}Terminal 2 - Storage Server 1 (Primary):${NC}"
echo "cd \"storage server\""
echo "./storage_server 1 9001 9002 ../storage_data1"
echo

echo -e "${BLUE}Terminal 3 - Storage Server 2 (Backup):${NC}"
echo "cd \"storage server\""
echo "./storage_server 2 9003 9004 ../storage_data2"
echo

echo -e "${BLUE}Terminal 4 - Client:${NC}"
echo "cd client"
echo "./client"
echo

wait_for_user

print_step "Step 5: Verification"

echo "After starting all components, verify the following:"
echo "1. Name Server shows 'Waiting for connections...'"
echo "2. Storage Servers show successful registration"
echo "3. Client shows 'Connected to Name Server' and 'DFS>' prompt"
echo

wait_for_user

print_step "Step 6: Basic Test Commands"

echo "Try these commands in the client terminal:"
echo
echo -e "${GREEN}# Create a test file${NC}"
echo "CREATE test.txt yourusername"
echo
echo -e "${GREEN}# Write to the file${NC}"
echo "WRITE test.txt"
echo "Hello, this is a test!"
echo
echo -e "${GREEN}# Read the file${NC}"
echo "READ test.txt"
echo
echo -e "${GREEN}# List files${NC}"
echo "LIST"
echo
echo -e "${GREEN}# Get file info${NC}"
echo "INFO test.txt"
echo
echo -e "${GREEN}# Add access for another user${NC}"
echo "ADDACCESS test.txt otheruser READ"
echo
echo -e "${GREEN}# Remove access${NC}"
echo "REMACCESS test.txt otheruser"
echo
echo -e "${GREEN}# Delete the file${NC}"
echo "DELETE test.txt"
echo

wait_for_user

print_step "Step 7: Failover Testing"

echo "To test failover:"
echo "1. Create and write to a file using the client"
echo "2. Stop Storage Server 1 (Ctrl+C in Terminal 2)"
echo "3. Try to read the file - it should work from backup server"
echo "4. Restart Storage Server 1"
echo "5. Operations should continue normally"
echo

wait_for_user

print_step "Step 8: Load Balancing Testing"

echo "To test load balancing, start additional servers:"
echo
echo -e "${BLUE}Terminal 5 - Storage Server 3 (Primary):${NC}"
echo "cd \"storage server\""
echo "./storage_server 3 9005 9006 ../storage_data3"
echo
echo -e "${BLUE}Terminal 6 - Storage Server 4 (Backup):${NC}"
echo "cd \"storage server\""
echo "./storage_server 4 9007 9008 ../storage_data4"
echo
echo "Then create multiple files and verify they're distributed across servers."
echo

wait_for_user

print_step "Step 9: Cleanup"

echo "To clean up after testing:"
echo "1. Type 'EXIT' in all client terminals"
echo "2. Press Ctrl+C in all server terminals"
echo "3. Optional: Remove test data with 'rm -rf storage_data* test_files'"
echo

print_success "Test script completed!"
echo -e "${BLUE}For detailed testing instructions, see TESTING_GUIDE.md${NC}"
echo -e "${BLUE}For implementation details, see name_server/NAME_SERVER_IMPLEMENTATION.md${NC}"
