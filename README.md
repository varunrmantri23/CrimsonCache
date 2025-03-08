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


## Running the C program(main.c)

Open the terminal and execute this to create a crimsoncache execuatle file 
```bash
gcc -o crimsoncache main.c -pthread
```
Once the executable file is created type 
```bash
./crimsomcache
```
Open a new terminal and connect to the server using the redis 
```bash
redis-cli -p PORT
```

## RESP format - handle_client()
- PING command
Client sends:
```bash
*1\r\n$4\r\nPING\r\n
```
Server responds:
```bash
+PONG\r\n
```

- ECHO command
Client sends:
```bash
*2\r\n$4\r\nECHO\r\n$5\r\nhello\r\n
```
Server responds
```bash
$5\r\nhello\r\n
```


