# CrimsonCache

CrimsonCache is a custom in-memory data store inspired by Redis, offering essential caching commands, data persistence, and replication. It’s built as a robust learning tool for exploring networking, concurrency, and distributed systems.

## Features (Planned)

-   **Core Functionality**

    -   In-memory key-value storage
    -   Support for various data types (strings, lists, sets)
    -   Key expiration mechanism
    -   LRU cache eviction

-   **Networking**

    -   TCP server for client connections
    -   Protocol similar to RESP (Redis Serialization Protocol)
    -   Support for concurrent client connections

-   **Persistence**

    -   RDB-style snapshot persistence
    -   Configurable persistence settings

-   **Replication**

    -   Primary-replica architecture
    -   Command propagation to replicas
    -   Replication handshake protocol

-   **Advanced Features**
    -   Transaction support
    -   Pub/Sub messaging
    -   Basic authentication
    -   Command rate limiting

## Getting Started

### Prerequisites

-   GCC/Clang compiler
-   Make build system
-   POSIX-compliant OS

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

### Method 1: Using Telnet

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
- `PING [message]` - Test connectivity, returns PONG or the message if provided
- `SET key value [EX seconds]` - Set a key to a value with optional expiration
- `GET key` - Get the value of a key
- `DEL key [key ...]` - Delete one or more keys
- `EXISTS key [key ...]` - Check if keys exist
- `EXPIRE key seconds` - Set a key's time to live in seconds
- `TTL key` - Get the time to live for a key

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
