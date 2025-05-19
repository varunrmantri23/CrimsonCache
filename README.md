# CrimsonCache

CrimsonCache is a custom in-memory data store inspired by Redis, offering essential caching commands, data persistence, and replication. It's built as a robust learning tool for exploring networking, concurrency, and distributed systems.

## Features

-   **Core Functionality**

    -   In-memory key-value storage with efficient hash table implementation
    -   Support for string data type (with more planned)
    -   Key expiration mechanism with TTL
    -   LRU cache eviction for memory management

-   **Networking**

    -   TCP server with IPv4/IPv6 dual-stack support
    -   RESP (Redis Serialization Protocol) compatible responses
    -   Support for 50+ concurrent client connections via multi-threading
    -   Clean connection handling and error management

-   **Persistence**

    -   RDB-style snapshot persistence for point-in-time recovery
    -   Background saving with fork() for non-blocking operation
    -   Automatic periodic saving based on changes and time
    -   Atomic file operations for crash-safe persistence

-   **Planned Features**
    -   Primary-replica replication
    -   Additional data types (lists, sets)
    -   Transaction support (MULTI/EXEC)
    -   Pub/Sub messaging system
    -   Basic authentication

## Getting Started

### Prerequisites

-   GCC/Clang compiler
-   Make build system
-   POSIX-compliant OS (Linux, macOS, etc.)

### Installation

```bash
git clone https://github.com/yourusername/CrimsonCache.git
cd CrimsonCache
make
```

## Usage

```bash
./bin/crimsoncache
```

## Connect to Running Server

### Method 1: Using Netcat (nc)

Open a new terminal window and use netcat to connect to your server:

```bash
nc localhost 6379
```

Once connected, type PING and press Enter. You should see:

```bash
PING
+PONG
```

### Method 2: Using Telnet

```bash
telnet localhost 6379
```

Once connected, type PING and press Enter. You should see:

```bash
PING
+PONG
```

### Method 3: Using Redis CLI (if installed)

If you happen to have the Redis command line interface installed:

```bash
redis-cli -p 6379 ping
```

This should return PONG.

## Supported Commands

CrimsonCache currently supports the following commands:

### Basic Commands

-   `PING [message]` - Test connectivity, returns PONG or the message if provided
-   `SET key value [EX seconds]` - Set a key to a value with optional expiration
-   `GET key` - Get the value of a key
-   `DEL key [key ...]` - Delete one or more keys
-   `EXISTS key [key ...]` - Check if keys exist
-   `EXPIRE key seconds` - Set a key's time to live in seconds
-   `TTL key` - Get the time to live for a key

### Persistence Operations

-   `SAVE` - Synchronously save the dataset to disk
-   `BGSAVE` - Asynchronously save the dataset to disk in the background

## Data Persistence

CrimsonCache provides RDB-style persistence similar to Redis:

-   **Automatic Saving**: The database is automatically saved to disk:

    -   After a specified number of changes (default: 1000)
    -   After a specified time period (default: 300 seconds/5 minutes)

-   **Manual Saving**: You can trigger persistence manually:

    -   `SAVE` command performs a blocking save operation
    -   `BGSAVE` command saves in the background without blocking

-   **Recovery**: When the server starts, it automatically loads the latest snapshot from disk.

## Testing Your Redis-compatible Commands

Since CrimsonCache implements Redis protocol, you can use any Redis client to interact with it:

```bash
# Using redis-cli
redis-cli -p 6379 set mykey "Hello World"
redis-cli -p 6379 get mykey

# Using netcat
nc localhost 6379
SET mykey "Hello World"
GET mykey
```

## Implementation Details

-   Multi-threaded architecture for handling client connections
-   Dual-stack IPv4/IPv6 networking implementation
-   LRU cache eviction algorithm for memory management
-   Fork-based background saving for non-blocking persistence
-   Properly handles quoted strings in commands

## License

MIT License - See [LICENSE](LICENSE) file for details.
