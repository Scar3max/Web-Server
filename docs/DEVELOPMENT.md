# Developer & Architecture Guide

This guide describes the internal architecture of the Multiplayer Racing Server, socket ownership invariants, threading model, and procedures for extending server components.

---

## 1. Core Architectural Invariants

### 1.1 The Golden Rule: Event Loop Socket Ownership

> [!IMPORTANT]
> **Strict Socket Ownership Invariant**: All network client socket file descriptors (`fd`), TCP buffers (`conn.inBuffer`, `conn.outBuffer`), and epoll registrations are **strictly owned by the single-threaded event loop** (`src/main.cpp`).
>
> Neither worker threads nor the 60 Hz simulation thread are ever permitted to directly read from, write to, or close network sockets.

```
       +--------------------------------------------------------------+
       |                  epoll Event Loop (Thread 1)                 |
       |  - Owns all socket FDs & TCP buffers                         |
       |  - Handles HTTP routing, WebSocket framing & fan-out         |
       |  - Reads incoming client frames & dispatches responses       |
       +--------------+-------------------------------+---------------+
                      |                               |
       Input Queue    |                               |  Snapshot Queue
     (Player Inputs)  v                               ^  (World Snapshots)
       +--------------+---------------+               |
       |   Simulation Thread (Thread 2)               |
       |  - Fixed 60 Hz steady_clock  +---------------+
       |  - Vehicle dynamics & tracks |
       |  - Collision resolution      |
       +------------------------------+
```

### 1.2 Thread Boundaries & Communication Channels

1. **Network I/O Thread (`main.cpp`)**:
   - Runs `epoll_wait()` non-blocking event loop.
   - Receives TCP chunks, parses HTTP requests and RFC 6455 WebSocket frames.
   - Pushes player inputs and room lifecycle events into `InputQueue`.
   - Receives snapshot ready notifications via `eventfd` (`snapshotWakeupFd`) and drains `SnapshotQueue`.
   - Enforces per-client output buffering, slow-client backpressure, and socket cleanup.

2. **Authoritative Simulation Thread (`SimulationThread.cpp`)**:
   - Runs independently on `std::chrono::steady_clock` at a fixed 60 Hz timestep ($\Delta t = 1/60\text{ s}$).
   - Drains pending inputs from `InputQueue`.
   - Updates authoritative physics (curved spline coordinates, collision detection, lap timing).
   - Generates immutable `WorldSnapshot` objects at 20 Hz and pushes them into bounded `SnapshotQueue`.
   - Signals the event loop via `eventfd` write.

3. **HTTP Worker Thread Pool (`ThreadPool.cpp`)**:
   - Handles CPU-bound or blocking static file I/O operations without stalling the network epoll loop.
   - Communicates with the event loop via `TaskQueue` and `ResponseQueue` signaled by `wakeupFd`.

---

## 2. Codebase Structure

```
http_server/
├── include/
│   ├── BinarySnapshot.h      # Schema v1 binary serializer & deserializer
│   ├── Config.h              # CLI flag & environment variable configuration
│   ├── Connection.h          # Client connection state, buffers & backpressure
│   ├── FileManager.h         # Path traversal defense, MIME types & binary I/O
│   ├── HttpParser.h          # Non-blocking streaming HTTP parser
│   ├── HttpRequest.h         # HTTP request representation
│   ├── InputQueue.h          # Thread-safe bounded player input queue
│   ├── Json.h                # Zero-dependency JSON parser & serializer
│   ├── Logger.h              # Level-filtered thread-safe logging
│   ├── Physics.h             # Vehicle physics equations & collision solver
│   ├── Player.h              # Vehicle state & input representation
│   ├── ResponseQueue.h       # Completed HTTP response queue
│   ├── Room.h                # Game room, lobby state & finish results
│   ├── RoomManager.h         # Lobby management, membership & host transfers
│   ├── RoomProtocol.h        # JSON room protocol message builders
│   ├── Router.h              # Path routing & handler registry
│   ├── ServerMetrics.h       # Atomic observability counters & Prometheus exporter
│   ├── SimulationThread.h    # 60 Hz authoritative simulation thread
│   ├── SnapshotQueue.h       # Thread-safe bounded snapshot queue
│   ├── TaskQueue.h           # HTTP worker task queue
│   ├── ThreadPool.h          # Fixed-size worker thread pool
│   ├── Track.h               # Catmull-Rom spline track geometry & projection
│   ├── WebSocketFrame.h      # RFC 6455 frame encoder/decoder
│   └── WebSocketHandshake.h  # RFC 6455 HTTP upgrade validator & SHA-1/Base64
├── src/                      # Component implementations
├── static/                   # Browser racing client (HTML5, Canvas, ES modules)
│   ├── js/
│   │   ├── binary_snapshot.js# JavaScript binary snapshot decoder
│   │   ├── game.js           # Client game loop & state synchronization
│   │   ├── input.js          # Keyboard & touch input sampler
│   │   ├── network.js        # WebSocket networking & format switching
│   │   ├── prediction.js     # Client prediction & server reconciliation
│   │   ├── renderer.js       # Track & vehicle Canvas renderer
│   │   └── track.js          # Browser track spline parity
│   ├── index.html            # Racing UI & HUD
│   └── style.css             # HUD styling
├── nginx/                    # Production reverse proxy configuration
├── Dockerfile                # Multi-stage container definition
└── CMakeLists.txt            # CMake build system definition
```

---

## 3. How to Extend the Server

### 3.1 Adding a New HTTP Route

To register a new HTTP endpoint, add the route in `src/main.cpp` using the `Router` instance:

```cpp
router.addRoute("/api/status", [](const HttpRequest& req) -> HttpResponse {
    HttpResponse res;
    res.status = 200;
    res.contentType = "application/json; charset=utf-8";
    res.body = "{\"status\":\"healthy\"}";
    return res;
});
```

For file serving or heavy computation, offload the work using `ThreadPool`:
```cpp
taskQueue.push([fd, connId, &responseQueue]() {
    HttpResponse res = computeHeavyPayload();
    responseQueue.push(PendingResponse{fd, connId, res});
});
```

### 3.2 Adding a New WebSocket Protocol Message

1. **Define the message schema** in `docs/PROTOCOL.md`.
2. **Handle message parsing** inside `handleWebSocketTextMessage` in `src/main.cpp`:
   ```cpp
   else if (type == "custom_event") {
       uint32_t pid = getOrAdmitPlayer(conn, epollFd, connections, socketToPlayer, playerToSocket, ...);
       if (pid == 0) return;

       // Forward to simulation thread via input queue
       PlayerEvent ev;
       ev.type = PlayerEvent::Type::CUSTOM_ACTION;
       ev.playerId = pid;
       inputQueue.pushEvent(ev);
   }
   ```
3. **Handle event in simulation** inside `SimulationThread::tick()`:
   ```cpp
   // Process event inside simulation thread
   case PlayerEvent::Type::CUSTOM_ACTION:
       applyCustomAction(ev.playerId);
       break;
   ```
4. **Broadcast feedback** to relevant room members from `main.cpp` using `sendWebSocketFrameSafe(conn, epollFd, frame)`.

### 3.3 Adding a New Server Metric

1. Add atomic member to `include/ServerMetrics.h`:
   ```cpp
   std::atomic<uint64_t> customCounterTotal{0};
   ```
2. Instrument event occurrence:
   ```cpp
   ServerMetrics::instance().customCounterTotal.fetch_add(1, std::memory_order_relaxed);
   ```
3. Export in `ServerMetrics::toPrometheusText()` and `ServerMetrics::toJson()` in `src/ServerMetrics.cpp`.

---

## 4. Building & Contribution Checklist

### 4.1 Building the Server

```bash
# Configure and build optimized production binary
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run server
./build/server
```

### 4.2 Pull Request Checklist

Before submitting modifications:
- [ ] No direct socket writes outside `main.cpp` (strict event loop ownership).
- [ ] All inputs validated and bounded (no unbounded memory growth).
- [ ] Binary snapshot schema compatibility maintained in `docs/PROTOCOL.md`.
- [ ] Static asset changes verified across browser clients.
