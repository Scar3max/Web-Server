# ⚡ High-Performance HTTP Server in C++

A lightweight, event-driven HTTP server built from scratch in C++20 using **Linux epoll**, **non-blocking I/O**, and a **custom thread pool** — no frameworks, no dependencies, just raw sockets and systems programming.

---

## Why This Exists

Most web servers abstract away the complexity of concurrent networking. This project peels back those layers and implements the core architecture found in production-grade servers like Nginx — including an event loop, non-blocking sockets, and worker threads — from first principles.

## Architecture

```
                    ┌─────────────────────────┐
                    │     Incoming Requests    │
                    └────────────┬────────────┘
                                 │
                    ┌────────────▼────────────┐
                    │    Non-Blocking Socket   │
                    │     (O_NONBLOCK)         │
                    └────────────┬────────────┘
                                 │
                    ┌────────────▼────────────┐
                    │    epoll Event Loop      │
                    │  (Level-Triggered I/O)   │
                    └────────────┬────────────┘
                                 │
                    ┌────────────▼────────────┐
                    │    Thread-Safe Task      │
                    │       Queue              │
                    │  (mutex + cond_var)      │
                    └────────────┬────────────┘
                                 │
              ┌──────────┬───────┴───────┬──────────┐
              │          │               │          │
         ┌────▼───┐ ┌────▼───┐     ┌────▼───┐ ┌────▼───┐
         │Worker 1│ │Worker 2│ ... │Worker 7│ │Worker 8│
         └────┬───┘ └────┬───┘     └────┬───┘ └────┬───┘
              │          │               │          │
              └──────────┴───────┬───────┴──────────┘
                                 │
                    ┌────────────▼────────────┐
                    │   HTTP Parse → Route    │
                    │   → Response → send()   │
                    └─────────────────────────┘
```

### How It Works

1. **Server socket** is created and set to non-blocking mode (`O_NONBLOCK`)
2. **epoll** monitors the server socket and all accepted client connections for read-readiness
3. When a new connection arrives, it's accepted in a non-blocking loop and registered with epoll
4. When data is ready on a client socket, the raw request bytes are read and packaged into a **Task**
5. The Task is pushed onto a **thread-safe queue** (mutex + condition variable)
6. A pool of **8 worker threads** pops tasks, parses the HTTP request, routes it, builds the response, and sends it back
7. Connections are closed after each response (`Connection: close`)

## Core Modules

| Module | File | Responsibility |
|---|---|---|
| **Entry Point** | `src/main.cpp` | Socket creation, epoll event loop, accepts connections, dispatches tasks |
| **Thread Pool** | `src/ThreadPool.cpp` | Manages 8 worker threads that consume tasks and send HTTP responses |
| **Task Queue** | `src/TaskQueue.cpp` | Thread-safe producer-consumer queue using `std::mutex` and `std::condition_variable` |
| **HTTP Parser** | `src/HttpParser.cpp` | Extracts method, path, and HTTP version from raw request strings |
| **Router** | `src/Router.cpp` | Maps URL paths to handler functions via `std::unordered_map` |
| **File Manager** | `src/FileManager.cpp` | Reads static HTML files from disk using `std::ifstream` |
| **Logger** | `src/Logger.cpp` | Appends timestamped entries to `server.log` |

## Key Design Decisions

- **epoll over select/poll** — O(1) readiness notification instead of O(n) scanning. Scales to thousands of concurrent connections.
- **Non-blocking sockets everywhere** — Both the server socket and every accepted client socket are set to `O_NONBLOCK`, preventing any single slow client from stalling the event loop.
- **Producer-consumer pattern** — The main thread (event loop) produces tasks; worker threads consume them. Decouples I/O multiplexing from request processing.
- **Graceful shutdown** — `TaskQueue::shutdown()` sets a stop flag and wakes all waiting threads. `ThreadPool::~ThreadPool()` joins all workers cleanly.
- **No external dependencies** — Built entirely with POSIX sockets, Linux epoll, and the C++20 standard library.

## Tech Stack

- **Language:** C++20
- **Build System:** CMake 3.10+
- **I/O Model:** Linux epoll (level-triggered)
- **Concurrency:** `std::thread`, `std::mutex`, `std::condition_variable`, `std::atomic`
- **Platform:** Linux (requires `<sys/epoll.h>`)

## Getting Started

### Prerequisites

- Linux (or WSL on Windows)
- GCC 10+ or Clang 12+ (C++20 support)
- CMake 3.10+

### Build

```bash
mkdir build && cd build
cmake ..
make
```

### Run

```bash
./server
```

The server starts on **port 8080**. Open your browser or curl it:

```bash
curl http://localhost:8080/        # → Home Page
curl http://localhost:8080/about   # → About Page
curl http://localhost:8080/xyz     # → 404 Not Found
```

Logs are written to `build/server.log`.

## Project Structure

```
http_server/
├── CMakeLists.txt          # Build configuration
├── include/
│   ├── FileManager.h       # Static file reader
│   ├── HttpParser.h        # Request parser declaration
│   ├── HttpRequest.h       # Request data structure
│   ├── Logger.h            # Logging utility
│   ├── Router.h            # URL → handler mapping
│   ├── TaskQueue.h         # Thread-safe queue + Task struct
│   └── ThreadPool.h        # Worker pool manager
├── src/
│   ├── main.cpp            # Server entry point & event loop
│   ├── FileManager.cpp     # File I/O implementation
│   ├── HttpParser.cpp      # HTTP/1.1 request parsing
│   ├── Logger.cpp          # Timestamped file logging
│   ├── Router.cpp          # Route matching logic
│   ├── TaskQueue.cpp       # Producer-consumer queue
│   └── ThreadPool.cpp      # Thread lifecycle management
└── static/
    ├── index.html           # Home page
    └── about.html           # About page
```

## Evolution

This project was built incrementally — each commit represents a distinct architectural milestone:

| # | Commit | What Changed |
|---|--------|-------------|
| 1 | `header files and parser added` | Core data structures and HTTP parsing |
| 2 | `non blocking sockets, logging and static files added` | Non-blocking I/O, file serving, logging |
| 3 | `added non blocking components to the server` | Full non-blocking socket pipeline |
| 4 | `integrated epoll into the web server` | Replaced busy-wait with epoll event loop |
| 5 | `added Threaded architecture to the web server` | Thread pool + task queue for concurrent processing |

## Possible Extensions

- [ ] HTTP keep-alive (`Connection: keep-alive`) with per-connection read buffers
- [ ] MIME type detection for serving CSS, JS, images, etc.
- [ ] Edge-triggered epoll (`EPOLLET`) with proper partial-read handling
- [ ] Dynamic route handlers with path parameters (`/user/:id`)
- [ ] Request body parsing for POST/PUT methods
- [ ] Benchmarking with `wrk` or `ab` against Nginx/Node.js

## License

MIT
