# PROJECT CONTEXT: Custom C++20 Event-Driven HTTP Server

## 1. High-Level Overview
- **Project:** Lightweight, high-performance HTTP server built completely from scratch in C++20.
- **Purpose:** Built without external libraries or frameworks using raw POSIX networking, Linux kernel `epoll`, and modern C++ concurrency primitives.
- **Ultimate Goal:** Extend this server to host both the static web assets (HTML5 Canvas/WebGL game client) and the real-time server-authoritative backend for a multiplayer racing game.
- **Target OS:** Linux (Ubuntu/Debian / WSL2).
- **Build System:** CMake 3.10+, GCC 10+ / Clang 12+ (C++20).

---

## 2. Tech Stack & Systems Architecture
- **Language:** C++20 (`std::thread`, `std::mutex`, `std::condition_variable`, `std::atomic`, `std::optional`).
- **Networking Model:** Half-Sync / Half-Async (Reactor pattern using Linux `epoll` in Level-Triggered mode + synchronized Worker Pool).
- **Non-blocking I/O:** Sockets set to `O_NONBLOCK` via `fcntl`.
- **Concurrency:**
  - 1 main I/O thread running the `epoll_wait` event loop (connection accept + request receive).
  - 1 thread-safe producer-consumer queue (`TaskQueue`).
  - 8 worker threads (`ThreadPool`) consuming tasks, parsing HTTP, routing, and sending responses.

---

## 3. Directory Layout & Codebase Structure
```
http_server/
├── CMakeLists.txt          # Standard CMake build setup (-std=c++20, -pthread)
├── include/
│   ├── HttpRequest.h       # Request data container
│   ├── HttpParser.h        # Request line parsing declarations
│   ├── Router.h            # URL-to-handler function routing table
│   ├── FileManager.h       # Static file reading utility
│   ├── Logger.h            # Timestamped logging utility
│   ├── TaskQueue.h         # Synchronized task queue & Task definition
│   └── ThreadPool.h        # Fixed worker thread pool manager
├── src/
│   ├── main.cpp            # Entry point: socket init, epoll loop, accept loop, task dispatch
│   ├── HttpParser.cpp      # HTTP/1.1 request-line parser
│   ├── Router.cpp          # Route registration and matching
│   ├── FileManager.cpp     # File streaming via std::ifstream
│   ├── Logger.cpp          # File logger appending to server.log
│   ├── TaskQueue.cpp       # Thread-safe queue implementation (mutex + cond_var)
│   └── ThreadPool.cpp      # Worker loop, response building, socket transmission
└── static/
    ├── index.html          # Default home page
    └── about.html          # About page
```

---

## 4. Component Implementation Details

### A. Data Models & Task Queue (`TaskQueue.h`, `TaskQueue.cpp`)
```cpp
struct Task {
    int clientSocket;
    std::string rawRequest;
    Router* router;
};

class TaskQueue {
private:
    std::queue<Task> tasks;
    std::mutex queueMutex;
    std::condition_variable cv;
    bool stop = false;
public:
    void push(Task task);             // Locks mutex, pushes task, calls cv.notify_one()
    std::optional<Task> pop();        // Waits on cv until (!tasks.empty() || stop)
    void notifyAll();                 // cv.notify_all()
    void shutdown();                  // Sets stop=true, notifies all threads
};
```

### B. Thread Pool (`ThreadPool.h`, `ThreadPool.cpp`)
- Initializes $N$ worker threads (default = 8).
- Workers execute a continuous loop:
  ```cpp
  void ThreadPool::worker() {
      while (true) {
          auto task = taskQueue.pop();
          if (!task) break; // Shutdown signaled

          HttpRequest request = parseRequest(task->rawRequest);
          std::string body = task->router->route(request.path);
          int status = body.empty() ? 404 : 200;
          if (body.empty()) body = "<h1>404 Not Found</h1>";
          std::string statusText = (status == 200) ? "OK" : "Not Found";

          std::string response = 
              "HTTP/1.1 " + std::to_string(status) + " " + statusText + "\r\n" +
              "Content-Type: text/html\r\n" +
              "Content-Length: " + std::to_string(body.size()) + "\r\n" +
              "Connection: close\r\n\r\n" + body;

          send(task->clientSocket, response.c_str(), response.size(), 0);
          close(task->clientSocket);
      }
  }
  ```
- Destructor invokes `taskQueue.shutdown()` and joins all worker threads.

### C. Request Parser (`HttpRequest.h`, `HttpParser.cpp`)
```cpp
class HttpRequest {
public:
    std::string method;   // e.g., "GET"
    std::string path;     // e.g., "/" or "/about"
    std::string version;  // e.g., "HTTP/1.1"
};

// Extracts method, path, and version from the first line via std::istringstream.
HttpRequest parseRequest(const std::string& rawRequest);
```

### D. Router & Static Files (`Router.cpp`, `FileManager.cpp`)
```cpp
class Router {
    std::unordered_map<std::string, std::function<std::string()>> routes;
public:
    void addRoute(const std::string& path, std::function<std::string()> handler);
    std::string route(const std::string& path);
};

class FileManager {
public:
    static std::string readFile(const std::string& path); // Reads entire file to string
};
```

### E. Event Loop (`main.cpp`)
- **Non-blocking socket setup:**
  - `socket(AF_INET, SOCK_STREAM, 0)`
  - `fcntl(serverSocket, F_SETFL, flags | O_NONBLOCK)`
  - `setsockopt(..., SO_REUSEADDR, ...)`
  - `bind` to `0.0.0.0:8080`, `listen(..., 128)`
- **`epoll` Configuration:**
  - `epoll_create1(0)`
  - Listening socket registered with `EPOLLIN`
  - Loop handles two primary event cases:
    1. `currentFd == serverSocket`: Non-blocking drain loop accepting client connections until `EAGAIN`/`EWOULDBLOCK`, setting client sockets to `O_NONBLOCK`, registering them to epoll with `EPOLLIN`.
    2. `currentFd != serverSocket`: Calls `recv(currentFd, buffer, 4095, 0)`. If valid and contains `\r\n\r\n`, deregisters socket via `epoll_ctl(EPOLL_CTL_DEL)`, builds `Task`, and pushes it to `taskQueue`.

---

## 5. Current Capabilities & Limitations

### Working Features:
- Handles concurrent TCP connections without blocking the main accept loop.
- Decouples connection multiplexing from HTTP processing via a worker thread pool.
- Graceful shutdown handles teardown cleanly.
- Routes static HTML endpoints (`/` and `/about`).

### Known Current Limitations to Address:
1. **Single Recv & Partial Reads:** If a TCP segment is fragmented or headers span multiple packets, checking only the first `recv()` for `\r\n\r\n` causes premature socket closure.
2. **Fixed MIME Type:** Content-Type is hardcoded to `text/html`. CSS, JavaScript, textures, models, sound, and WebAssembly cannot be served properly yet.
3. **Short-Lived Connections:** Always sends `Connection: close` and tears down sockets immediately. Does not support HTTP keep-alive.
4. **No Full-Duplex/Real-Time Protocol:** No WebSocket support (RFC 6455) or persistent connection multiplexing.

---

## 6. Objective: Multiplayer Racing Game Architecture Requirements

The goal is to evolve this codebase so it can host a 2D/3D multiplayer racing game:

### Layer 1: Web Asset Delivery (HTTP/Static Serving)
- Serve the client game engine bundle (HTML5 Canvas or Three.js / WebGL client).
- Dynamic file serving with proper MIME types:
  - `.html` -> `text/html`
  - `.js`, `.mjs` -> `application/javascript`
  - `.css` -> `text/css`
  - `.png`, `.jpg` -> `image/png`, `image/jpeg`
  - `.json` -> `application/json`
  - `.wasm` -> `application/wasm`
- Binary file support in `FileManager` (currently uses standard string buffer which fails on raw binary).

### Layer 2: Real-Time Multiplayer Networking
- HTTP Upgrade to WebSocket (`Upgrade: websocket` / `Sec-WebSocket-Key` handshake with SHA-1 + Base64).
- Persistent socket retention in `epoll` instead of closing after response.
- WebSocket frame decoding and encoding (RFC 6455 framing: fin, opcode, masking key, payload).

### Layer 3: Authoritative Game Server Logic
- Game Room / Lobby system: Multiple players connecting to the same race session.
- Fixed Tick Rate Loop (e.g., 30Hz or 60Hz):
  - Ingest player input packets (throttle, brake, steer angle).
  - Physics/movement update & checkpoint tracking.
  - Broadcast compressed world state snapshot (player X, Y, angle, velocity, lap count) to all players in the room.

---

## 7. Recommended Downstream LLM Prompt Template

```
I have an existing high-performance C++20 HTTP server built with Linux epoll and a worker thread pool. Above is the complete project context and implementation status. 

Based on this exact architecture, design/generate the next phase:
[SPECIFY YOUR REQUIREMENT HERE: e.g.,
  1. The HTML5 Canvas racing game client and asset structure
  2. The WebSocket upgrade handshake and frame parser for this epoll loop
  3. The authoritative 30Hz multiplayer game state sync loop]

Constraints:
- Preserve the existing C++20 standard and epoll Reactor + ThreadPool pattern.
- Do not introduce heavy third-party framework dependencies unless specified.
```
