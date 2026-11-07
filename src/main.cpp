#include <cstring>
#include <cctype>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <csignal>
#include <cerrno>
#include <chrono>

#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sstream>
#include <iomanip>

#include "HttpParser.h"
#include "Router.h"
#include "FileManager.h"
#include "Logger.h"
#include "TaskQueue.h"
#include "ResponseQueue.h"
#include "ThreadPool.h"
#include "Connection.h"
#include "WebSocketHandshake.h"
#include "WebSocketFrame.h"
#include "Json.h"
#include "Player.h"
#include "Physics.h"
#include "InputQueue.h"
#include "SnapshotQueue.h"
#include "SimulationThread.h"

using namespace std;

constexpr int SERVER_PORT       = 8080;
constexpr int THREAD_POOL_SIZE  = 8;
constexpr int RECV_BUFFER_SIZE  = 4096;
constexpr int MAX_EPOLL_EVENTS  = 64;
constexpr int LISTEN_BACKLOG    = 128;

static volatile sig_atomic_t g_running = 1;

static void handleSignal(int sig) {
    (void)sig;
    g_running = 0;
}

static string HomeHandler() {
    string content = FileManager::readFile("../static/index.html");
    if (content.empty()) {
        content = FileManager::readFile("static/index.html");
    }
    return content;
}

static string AboutHandler() {
    string content = FileManager::readFile("../static/about.html");
    if (content.empty()) {
        content = FileManager::readFile("static/about.html");
    }
    return content;
}

static string LargeHandler() {
    string data;
    data.reserve(256 * 1024);
    for (int i = 0; i < 256; i++) {
        data += "Chunk " + to_string(i) + ": " + string(990, 'A') + "\n";
    }
    return data;
}

// Removes a player from the active simulation registry and queues a LEAVE event
static void removePlayerIfPresent(int fd,
                                  unordered_map<int, uint32_t>& socketToPlayer,
                                  unordered_map<uint32_t, int>& playerToSocket,
                                  unordered_map<uint32_t, uint64_t>& playerLastSeq,
                                  unordered_map<uint32_t, std::chrono::steady_clock::time_point>& playerLastTime,
                                  InputQueue& inputQueue) {
    auto it = socketToPlayer.find(fd);
    if (it != socketToPlayer.end()) {
        uint32_t pid = it->second;
        socketToPlayer.erase(it);
        playerToSocket.erase(pid);
        playerLastSeq.erase(pid);
        playerLastTime.erase(pid);

        PlayerEvent ev;
        ev.type = PlayerEvent::Type::LEAVE;
        ev.playerId = pid;
        inputQueue.pushLeave(ev);
    }
}

// Closes a connection, removes it from epoll, cleans up player state, and erases from registry.
static void closeConnection(int fd, int epollFd,
                            unordered_map<int, Connection>& connections,
                            unordered_map<int, uint32_t>& socketToPlayer,
                            unordered_map<uint32_t, int>& playerToSocket,
                            unordered_map<uint32_t, uint64_t>& playerLastSeq,
                            unordered_map<uint32_t, std::chrono::steady_clock::time_point>& playerLastTime,
                            InputQueue& inputQueue) {
    removePlayerIfPresent(fd, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
    auto it = connections.find(fd);
    if (it != connections.end()) {
        epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        connections.erase(it);
    }
}

// Sends an immediate HTTP error response from the event loop
static void sendImmediateError(int fd, int statusCode, const string& reason) {
    string body = "<h1>" + to_string(statusCode) + " " + reason + "</h1>";
    string response = "HTTP/1.1 " + to_string(statusCode) + " " + reason + "\r\n"
                    + "Content-Type: text/html\r\n"
                    + "Content-Length: " + to_string(body.size()) + "\r\n"
                    + "Connection: close\r\n\r\n"
                    + body;
    send(fd, response.c_str(), response.size(), MSG_NOSIGNAL);
}

// Attempts to flush buffered outgoing data for a connection.
// Returns true on fatal socket error (EPIPE, ECONNRESET, etc.), false on success or EAGAIN/EWOULDBLOCK.
static bool flushSendBuffer(Connection& conn) {
    while (conn.bytesSent < conn.outBuffer.size()) {
        const char* dataPtr = conn.outBuffer.data() + conn.bytesSent;
        size_t remaining = conn.outBuffer.size() - conn.bytesSent;
        ssize_t sent = send(conn.fd, dataPtr, remaining, MSG_NOSIGNAL);

        if (sent > 0) {
            conn.bytesSent += sent;
        } else if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;
            }
            if (errno == EINTR) {
                continue;
            }
            return true;
        } else {
            break;
        }
    }
    return false;
}

// Safely buffers and flushes an outgoing WebSocket frame, enforcing a strict maximum buffer limit.
// Resets outBuffer when fully written or registers EPOLLOUT if write blocked.
// Returns true on success/EAGAIN, false on fatal socket error or memory limit violation.
static bool sendWebSocketFrameSafe(Connection& conn, int epollFd, const string& frame, size_t maxBufferSize = Connection::MAX_OUT_BUFFER_SIZE) {
    if (conn.outBuffer.size() + frame.size() > maxBufferSize) {
        return false;
    }
    conn.outBuffer.append(frame);
    bool sockErr = flushSendBuffer(conn);
    if (sockErr) {
        return false;
    }
    if (conn.bytesSent >= conn.outBuffer.size()) {
        conn.outBuffer.clear();
        conn.bytesSent = 0;
    } else {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
        ev.data.fd = conn.fd;
        epoll_ctl(epollFd, EPOLL_CTL_MOD, conn.fd, &ev);
    }
    return true;
}

// Processes incoming WebSocket text message (input protocol, telemetry echo, or plain text echo)
static void handleWebSocketTextMessage(Connection& conn,
                                       int epollFd,
                                       const string& payload,
                                       unordered_map<int, Connection>& connections,
                                       unordered_map<int, uint32_t>& socketToPlayer,
                                       unordered_map<uint32_t, int>& playerToSocket,
                                       unordered_map<uint32_t, uint64_t>& playerLastSeq,
                                       unordered_map<uint32_t, std::chrono::steady_clock::time_point>& playerLastTime,
                                       uint32_t& nextPlayerId,
                                       InputQueue& inputQueue) {
    json::Value root;
    std::string err;
    bool isJsonCandidate = (!payload.empty() && (payload.front() == '{' || payload.front() == '['));
    if (!json::parse(payload, root, err) || !root.isObject()) {
        if (isJsonCandidate) {
            // Malformed JSON: drop safely without echoing
            return;
        }
        // Plain text: echo back for backwards compatibility with Phase 2 echo tests
        string echo = WebSocketFrame::encodeTextFrame(payload);
        if (!sendWebSocketFrameSafe(conn, epollFd, echo)) {
            closeConnection(conn.fd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
            return;
        }
        return;
    }

    std::string type = root["type"].asString();

    if (type == "input") {
        uint32_t pid = 0;
        auto it = socketToPlayer.find(conn.fd);
        if (it == socketToPlayer.end()) {
            // First input message from this client: attempt to admit as active player
            PlayerEvent ev;
            ev.type = PlayerEvent::Type::JOIN;
            ev.playerId = nextPlayerId;
            ev.socketFd = conn.fd;
            ev.connectionId = conn.id;

            if (!inputQueue.pushJoin(ev)) {
                // Lifecycle admission capacity exhausted or max active players reached!
                // Reject join cleanly with 1013 Try Again Later
                json::Value errObj(json::Type::OBJECT);
                errObj["type"] = "error";
                errObj["message"] = "Server player capacity reached; please retry later";
                string errFrame = WebSocketFrame::encodeTextFrame(errObj.serialize());
                sendWebSocketFrameSafe(conn, epollFd, errFrame);

                string closeFrame = WebSocketFrame::encodeCloseFrame(1013, "Try Again Later");
                sendWebSocketFrameSafe(conn, epollFd, closeFrame);
                conn.shouldClose = true;
                return;
            }

            pid = nextPlayerId++;
            socketToPlayer[conn.fd] = pid;
            playerToSocket[pid] = conn.fd;
            playerLastSeq[pid] = 0;
            playerLastTime[pid] = std::chrono::steady_clock::now();

            // Send authoritative welcome message with assigned playerId
            json::Value welcome(json::Type::OBJECT);
            welcome["type"] = "welcome";
            welcome["playerId"] = static_cast<int64_t>(pid);
            string welcomeFrame = WebSocketFrame::encodeTextFrame(welcome.serialize());
            if (!sendWebSocketFrameSafe(conn, epollFd, welcomeFrame)) {
                closeConnection(conn.fd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                return;
            }
        } else {
            pid = it->second;
        }

        // Validate presence and types of required input fields
        if (!root.contains("seq") || !root["seq"].isNumber()) return;
        if (!root.contains("controls") || !root["controls"].isObject()) return;

        const auto& cObj = root["controls"];
        if (!cObj.contains("up") || !cObj["up"].isBool()) return;
        if (!cObj.contains("down") || !cObj["down"].isBool()) return;
        if (!cObj.contains("left") || !cObj["left"].isBool()) return;
        if (!cObj.contains("right") || !cObj["right"].isBool()) return;

        uint64_t seq = static_cast<uint64_t>(root["seq"].asInt());
        uint64_t lastSeq = playerLastSeq[pid];
        if (seq <= lastSeq) {
            // Drop stale or repeated sequence number safely
            return;
        }

        // Rate limiting: prevent input packet spam (maximum ~160 inputs/s)
        auto now = std::chrono::steady_clock::now();
        auto lastTime = playerLastTime[pid];
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTime).count();
        if (elapsedMs < 6) {
            return; // Drop excessive input packets
        }

        playerLastSeq[pid] = seq;
        playerLastTime[pid] = now;

        // Construct validated input (ignoring any client-supplied position/speed)
        PlayerInput in;
        in.playerId = pid;
        in.seq = seq;
        in.controls.up = cObj["up"].asBool();
        in.controls.down = cObj["down"].asBool();
        in.controls.left = cObj["left"].asBool();
        in.controls.right = cObj["right"].asBool();

        inputQueue.pushInput(in);
    } else if (type == "telemetry") {
        // Backwards compatibility for Phase 3 telemetry tests
        string echo = WebSocketFrame::encodeTextFrame(payload);
        if (!sendWebSocketFrameSafe(conn, epollFd, echo)) {
            closeConnection(conn.fd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
            return;
        }
    } else {
        // Generic JSON echo for other tests
        string echo = WebSocketFrame::encodeTextFrame(payload);
        if (!sendWebSocketFrameSafe(conn, epollFd, echo)) {
            closeConnection(conn.fd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
            return;
        }
    }
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    Router router;
    router.addRoute("/", HomeHandler);
    router.addRoute("/about", AboutHandler);
    router.addRoute("/large", LargeHandler);

    TaskQueue taskQueue;
    ResponseQueue responseQueue;
    ThreadPool pool(THREAD_POOL_SIZE, taskQueue, responseQueue);

    InputQueue inputQueue;
    SnapshotQueue snapshotQueue;
    SimulationThread simulationThread(inputQueue, snapshotQueue);

    router.addRoute("/metrics", [&simulationThread]() -> HttpResponse {
        HttpResponse res;
        res.status = 200;
        res.contentType = "application/json";

        std::ostringstream oss;
        oss << "{"
            << "\"tick\":" << simulationThread.getTick() << ","
            << "\"tickRate\":" << std::fixed << std::setprecision(2) << simulationThread.getTickRate() << ","
            << "\"snapshotRate\":" << std::fixed << std::setprecision(2) << simulationThread.getSnapshotRate() << ","
            << "\"activePlayers\":" << simulationThread.getPlayerCount() << ","
            << "\"totalSnapshotsPublished\":" << simulationThread.getTotalSnapshots() << ","
            << "\"inputQueueLifecycleSize\":" << simulationThread.getInputQueue().getLifecycleQueueSize() << ","
            << "\"inputQueueHighWaterEvents\":" << simulationThread.getInputQueue().getHighWaterEvents() << ","
            << "\"inputQueueHighWaterInputs\":" << simulationThread.getInputQueue().getHighWaterInputs() << ","
            << "\"totalJoinsAttempted\":" << simulationThread.getInputQueue().getTotalJoinsAttempted() << ","
            << "\"totalJoinsAdmitted\":" << simulationThread.getInputQueue().getTotalJoinsAdmitted() << ","
            << "\"totalJoinsRejected\":" << simulationThread.getInputQueue().getTotalJoinsRejected() << ","
            << "\"totalLeavesPushed\":" << simulationThread.getInputQueue().getTotalLeavesPushed() << ","
            << "\"snapshotQueueHighWater\":" << simulationThread.getSnapshotQueue().getHighWaterMark() << ","
            << "\"snapshotQueueTotalDropped\":" << simulationThread.getSnapshotQueue().getTotalDropped()
            << "}";

        res.body = oss.str();
        return res;
    });

    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    int serverFlags = fcntl(serverSocket, F_GETFL, 0);
    fcntl(serverSocket, F_SETFL, serverFlags | O_NONBLOCK);

    struct sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(SERVER_PORT);
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    int opt = 1;

    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR failed");
        close(serverSocket);
        exit(EXIT_FAILURE);
    }

    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("Bind failed");
        close(serverSocket);
        exit(EXIT_FAILURE);
    }

    if (listen(serverSocket, LISTEN_BACKLOG) < 0) {
        perror("Listen failed");
        close(serverSocket);
        exit(EXIT_FAILURE);
    }

    int epollFd = epoll_create1(0);
    if (epollFd < 0) {
        perror("epoll_create1 failed");
        close(serverSocket);
        exit(EXIT_FAILURE);
    }

    // Register server socket for incoming connections
    epoll_event serverEvent{};
    serverEvent.events = EPOLLIN;
    serverEvent.data.fd = serverSocket;
    if (epoll_ctl(epollFd, EPOLL_CTL_ADD, serverSocket, &serverEvent) < 0) {
        perror("epoll_ctl serverSocket failed");
        close(epollFd);
        close(serverSocket);
        exit(EXIT_FAILURE);
    }

    // Setup eventfd for HTTP worker thread completion signaling
    int wakeupFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeupFd < 0) {
        perror("eventfd creation failed");
        close(epollFd);
        close(serverSocket);
        exit(EXIT_FAILURE);
    }

    responseQueue.setWakeupFd(wakeupFd);

    epoll_event wakeupEvent{};
    wakeupEvent.events = EPOLLIN;
    wakeupEvent.data.fd = wakeupFd;
    if (epoll_ctl(epollFd, EPOLL_CTL_ADD, wakeupFd, &wakeupEvent) < 0) {
        perror("epoll_ctl wakeupFd failed");
        close(wakeupFd);
        close(epollFd);
        close(serverSocket);
        exit(EXIT_FAILURE);
    }

    // Setup eventfd for simulation thread snapshot publication signaling
    int snapshotWakeupFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (snapshotWakeupFd < 0) {
        perror("eventfd snapshotWakeupFd failed");
        close(wakeupFd);
        close(epollFd);
        close(serverSocket);
        exit(EXIT_FAILURE);
    }

    snapshotQueue.setWakeupFd(snapshotWakeupFd);

    epoll_event snapWakeupEvent{};
    snapWakeupEvent.events = EPOLLIN;
    snapWakeupEvent.data.fd = snapshotWakeupFd;
    if (epoll_ctl(epollFd, EPOLL_CTL_ADD, snapshotWakeupFd, &snapWakeupEvent) < 0) {
        perror("epoll_ctl snapshotWakeupFd failed");
        close(snapshotWakeupFd);
        close(wakeupFd);
        close(epollFd);
        close(serverSocket);
        exit(EXIT_FAILURE);
    }

    // Event-loop-owned connection and player registries
    unordered_map<int, Connection> connections;
    unordered_map<int, uint32_t> socketToPlayer;
    unordered_map<uint32_t, int> playerToSocket;
    unordered_map<uint32_t, uint64_t> playerLastSeq;
    unordered_map<uint32_t, std::chrono::steady_clock::time_point> playerLastTime;
    uint64_t nextConnectionId = 1;
    uint32_t nextPlayerId = 1;

    // Start 60 Hz simulation thread
    simulationThread.start();

    epoll_event events[MAX_EPOLL_EVENTS];
    Logger::log("Server started on port " + to_string(SERVER_PORT));

    while (g_running) {
        int readyCount = epoll_wait(epollFd, events, MAX_EPOLL_EVENTS, 500);
        if (readyCount < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait failed");
            break;
        }

        for (int i = 0; i < readyCount; i++) {
            int currentFd = events[i].data.fd;
            uint32_t currentEvents = events[i].events;

            // 1. Worker thread finished processing HTTP responses
            if (currentFd == wakeupFd) {
                uint64_t counter = 0;
                while (read(wakeupFd, &counter, sizeof(counter)) > 0) {}

                auto responses = responseQueue.popAll();
                for (auto& resp : responses) {
                    auto it = connections.find(resp.clientSocket);
                    if (it == connections.end() || it->second.id != resp.connectionId) {
                        continue;
                    }

                    Connection& conn = it->second;
                    conn.outBuffer = std::move(resp.rawResponse);
                    conn.bytesSent = 0;
                    conn.shouldClose = resp.closeConnection;
                    conn.state = ConnectionState::WRITING_RESPONSE;

                    bool sockErr = flushSendBuffer(conn);
                    if (sockErr) {
                        closeConnection(conn.fd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                        continue;
                    }

                    if (conn.bytesSent >= conn.outBuffer.size()) {
                        if (conn.shouldClose) {
                            closeConnection(conn.fd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                        } else {
                            conn.resetForNextRequest();
                            epoll_event ev{};
                            ev.events = EPOLLIN | EPOLLRDHUP;
                            ev.data.fd = conn.fd;
                            epoll_ctl(epollFd, EPOLL_CTL_MOD, conn.fd, &ev);
                        }
                    } else {
                        epoll_event ev{};
                        ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                        ev.data.fd = conn.fd;
                        epoll_ctl(epollFd, EPOLL_CTL_MOD, conn.fd, &ev);
                    }
                }
            }
            // 2. Simulation thread published authoritative world snapshots
            else if (currentFd == snapshotWakeupFd) {
                uint64_t counter = 0;
                while (read(snapshotWakeupFd, &counter, sizeof(counter)) > 0) {}

                WorldSnapshot latestSnap;
                if (snapshotQueue.popLatest(latestSnap)) {
                    string jsonStr = latestSnap.toJson();
                    string frame = WebSocketFrame::encodeTextFrame(jsonStr);

                    vector<int> slowClientsToDisconnect;

                    for (const auto& [pid, sockFd] : playerToSocket) {
                        auto cIt = connections.find(sockFd);
                        if (cIt != connections.end() && cIt->second.protocol == ConnectionProtocol::WEBSOCKET) {
                            Connection& playerConn = cIt->second;

                            // Backpressure check:
                            // If socket is blocked (pending unsent data) or buffer exceeds 64 KB:
                            bool socketBlocked = (playerConn.bytesSent < playerConn.outBuffer.size());
                            bool bufferTooLarge = (playerConn.outBuffer.size() >= Connection::SNAPSHOT_BACKPRESSURE_THRESHOLD);

                            if (socketBlocked || bufferTooLarge) {
                                playerConn.consecutiveDroppedSnapshots++;
                                if (playerConn.consecutiveDroppedSnapshots >= Connection::MAX_CONSECUTIVE_DROPPED_SNAPSHOTS) {
                                    // Client stalled for >= 50 consecutive snapshots (~2.5s)
                                    slowClientsToDisconnect.push_back(sockFd);
                                }
                                // Drop this snapshot for this client to prevent queuing stale state
                                continue;
                            }

                            // Socket write buffer is clear: reset consecutive dropped snapshots counter
                            playerConn.consecutiveDroppedSnapshots = 0;
                            playerConn.outBuffer.append(frame);
                            bool sockErr = flushSendBuffer(playerConn);
                            if (sockErr) {
                                slowClientsToDisconnect.push_back(sockFd);
                                continue;
                            }

                            if (playerConn.bytesSent >= playerConn.outBuffer.size()) {
                                playerConn.outBuffer.clear();
                                playerConn.bytesSent = 0;
                            } else {
                                epoll_event ev{};
                                ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                                ev.data.fd = sockFd;
                                epoll_ctl(epollFd, EPOLL_CTL_MOD, sockFd, &ev);
                            }
                        }
                    }

                    for (int sfd : slowClientsToDisconnect) {
                        closeConnection(sfd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                    }
                }
            }
            // 3. Incoming connection on server socket
            else if (currentFd == serverSocket) {
                while (true) {
                    sockaddr_in clientAddr{};
                    socklen_t clientLen = sizeof(clientAddr);
                    int clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientLen);

                    if (clientSocket < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        perror("accept failed");
                        break;
                    }

                    int flags = fcntl(clientSocket, F_GETFL, 0);
                    if (flags == -1) {
                        close(clientSocket);
                        continue;
                    }
                    fcntl(clientSocket, F_SETFL, flags | O_NONBLOCK);

                    Connection conn(clientSocket, nextConnectionId++);
                    connections[clientSocket] = std::move(conn);

                    epoll_event clientEvent{};
                    clientEvent.events = EPOLLIN | EPOLLRDHUP;
                    clientEvent.data.fd = clientSocket;
                    if (epoll_ctl(epollFd, EPOLL_CTL_ADD, clientSocket, &clientEvent) < 0) {
                        perror("epoll_ctl add client failed");
                        close(clientSocket);
                        connections.erase(clientSocket);
                    }
                }
            }
            // 4. Client socket I/O events
            else {
                auto it = connections.find(currentFd);
                if (it == connections.end()) {
                    continue;
                }

                Connection& conn = it->second;
                int clientSocketFd = currentFd;

                // Handle peer hangup or errors
                if (currentEvents & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                    if (conn.bytesSent >= conn.outBuffer.size()) {
                        closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                        continue;
                    }
                }

                // Handle writable event (EPOLLOUT) for pending outgoing data
                if (currentEvents & EPOLLOUT) {
                    if (conn.bytesSent < conn.outBuffer.size()) {
                        bool sockErr = flushSendBuffer(conn);
                        if (sockErr) {
                            closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                            continue;
                        }

                        if (conn.bytesSent >= conn.outBuffer.size()) {
                            conn.outBuffer.clear();
                            conn.bytesSent = 0;

                            if (conn.shouldClose) {
                                closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                                continue;
                            } else {
                                epoll_event ev{};
                                ev.events = EPOLLIN | EPOLLRDHUP;
                                ev.data.fd = clientSocketFd;
                                epoll_ctl(epollFd, EPOLL_CTL_MOD, clientSocketFd, &ev);
                            }
                        }
                    }
                }

                // Handle readable event (EPOLLIN) for incoming request or frame data
                if (currentEvents & EPOLLIN) {
                    bool disconnected = false;
                    while (true) {
                        char buffer[RECV_BUFFER_SIZE];
                        ssize_t bytes = recv(conn.fd, buffer, sizeof(buffer), 0);

                        if (bytes > 0) {
                            conn.inBuffer.append(buffer, bytes);
                            if (conn.inBuffer.size() > Connection::MAX_BUFFER_SIZE) {
                                if (conn.protocol == ConnectionProtocol::HTTP) {
                                    sendImmediateError(conn.fd, 413, "Payload Too Large");
                                }
                                disconnected = true;
                                break;
                            }
                        } else if (bytes == 0) {
                            disconnected = true;
                            break;
                        } else {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                break;
                            }
                            if (errno == EINTR) {
                                continue;
                            }
                            disconnected = true;
                            break;
                        }
                    }

                    if (disconnected) {
                        closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                        continue;
                    }

                    // A. Protocol: HTTP
                    if (conn.protocol == ConnectionProtocol::HTTP) {
                        size_t requestLen = 0;
                        auto parseResult = conn.checkRequest(requestLen);

                        if (parseResult == Connection::ParseResult::COMPLETE) {
                            string rawRequest = conn.inBuffer.substr(0, requestLen);
                            conn.inBuffer.erase(0, requestLen);

                            if (WebSocketHandshake::isUpgradeRequest(rawRequest)) {
                                string responseText, clientKey;
                                auto hsResult = WebSocketHandshake::validateHandshake(rawRequest, responseText, clientKey);

                                if (hsResult == WebSocketHandshake::HandshakeResult::SUCCESS) {
                                    conn.protocol = ConnectionProtocol::WEBSOCKET;
                                    int sndbuf = 8192; // Bounded socket send buffer for responsive backpressure
                                    setsockopt(conn.fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
                                    conn.shouldClose = false;
                                    conn.outBuffer = std::move(responseText);
                                    conn.bytesSent = 0;
                                    conn.state = ConnectionState::WRITING_RESPONSE;

                                    bool sockErr = flushSendBuffer(conn);
                                    if (sockErr) {
                                        closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                                        continue;
                                    }

                                    if (conn.bytesSent >= conn.outBuffer.size()) {
                                        conn.outBuffer.clear();
                                        conn.bytesSent = 0;
                                        epoll_event ev{};
                                        ev.events = EPOLLIN | EPOLLRDHUP;
                                        ev.data.fd = clientSocketFd;
                                        epoll_ctl(epollFd, EPOLL_CTL_MOD, clientSocketFd, &ev);
                                    } else {
                                        epoll_event ev{};
                                        ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                                        ev.data.fd = clientSocketFd;
                                        epoll_ctl(epollFd, EPOLL_CTL_MOD, clientSocketFd, &ev);
                                    }
                                } else {
                                    conn.outBuffer = std::move(responseText);
                                    conn.bytesSent = 0;
                                    conn.shouldClose = true;
                                    conn.state = ConnectionState::WRITING_RESPONSE;

                                    flushSendBuffer(conn);
                                    if (conn.bytesSent >= conn.outBuffer.size()) {
                                        closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                                    } else {
                                        epoll_event ev{};
                                        ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                                        ev.data.fd = clientSocketFd;
                                        epoll_ctl(epollFd, EPOLL_CTL_MOD, clientSocketFd, &ev);
                                    }
                                }
                            } else {
                                conn.state = ConnectionState::PROCESSING;
                                epoll_event ev{};
                                ev.events = EPOLLRDHUP;
                                ev.data.fd = clientSocketFd;
                                epoll_ctl(epollFd, EPOLL_CTL_MOD, clientSocketFd, &ev);

                                Task task;
                                task.connectionId = conn.id;
                                task.clientSocket = clientSocketFd;
                                task.rawRequest = std::move(rawRequest);
                                task.router = &router;

                                taskQueue.push(std::move(task));
                            }
                        } else if (parseResult == Connection::ParseResult::TOO_LARGE) {
                            sendImmediateError(clientSocketFd, 431, "Request Header Fields Too Large");
                            closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                            continue;
                        } else if (parseResult == Connection::ParseResult::BAD_REQUEST) {
                            sendImmediateError(clientSocketFd, 400, "Bad Request");
                            closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                            continue;
                        }
                    }
                    // B. Protocol: WEBSOCKET
                    else if (conn.protocol == ConnectionProtocol::WEBSOCKET) {
                        bool connectionClosed = false;

                        while (!conn.inBuffer.empty()) {
                            WebSocketFrame::Frame frame;
                            size_t bytesConsumed = 0;
                            uint16_t closeCode = WebSocketFrame::CLOSE_NORMAL;
                            auto status = WebSocketFrame::parseFrame(conn.inBuffer, frame, bytesConsumed, closeCode);

                            if (status == WebSocketFrame::ParseStatus::NEED_MORE_DATA) {
                                break;
                            }

                            if (status == WebSocketFrame::ParseStatus::PROTOCOL_ERROR) {
                                string closeFrame = WebSocketFrame::encodeCloseFrame(closeCode, "Protocol Error");
                                conn.outBuffer.append(closeFrame);
                                conn.wsCloseSent = true;
                                conn.shouldClose = true;
                                flushSendBuffer(conn);

                                if (conn.bytesSent >= conn.outBuffer.size()) {
                                    closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                                } else {
                                    epoll_event ev{};
                                    ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                                    ev.data.fd = clientSocketFd;
                                    epoll_ctl(epollFd, EPOLL_CTL_MOD, clientSocketFd, &ev);
                                }
                                connectionClosed = true;
                                break;
                            }

                            conn.inBuffer.erase(0, bytesConsumed);

                            // Control Frames
                            if (frame.opcode == WebSocketFrame::OPCODE_PING) {
                                string pong = WebSocketFrame::encodePongFrame(frame.payload);
                                if (!sendWebSocketFrameSafe(conn, epollFd, pong, Connection::MAX_CONGESTED_CONTROL_BUFFER)) {
                                    closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                                    connectionClosed = true;
                                    break;
                                }
                            } else if (frame.opcode == WebSocketFrame::OPCODE_PONG) {
                                // Ignore
                            } else if (frame.opcode == WebSocketFrame::OPCODE_CLOSE) {
                                conn.wsCloseReceived = true;
                                if (!conn.wsCloseSent) {
                                    conn.wsCloseSent = true;
                                    uint16_t respCode = (frame.closeCode != 0) ? frame.closeCode : WebSocketFrame::CLOSE_NORMAL;
                                    string closeResponse = WebSocketFrame::encodeCloseFrame(respCode, "");
                                    if (conn.outBuffer.size() + closeResponse.size() <= Connection::MAX_OUT_BUFFER_SIZE) {
                                        conn.outBuffer.append(closeResponse);
                                    }
                                }
                                conn.shouldClose = true;
                                flushSendBuffer(conn);

                                if (conn.bytesSent >= conn.outBuffer.size()) {
                                    closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                                } else {
                                    epoll_event ev{};
                                    ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                                    ev.data.fd = clientSocketFd;
                                    epoll_ctl(epollFd, EPOLL_CTL_MOD, clientSocketFd, &ev);
                                }
                                connectionClosed = true;
                                break;
                            }
                            // Data Frames: Text
                            else if (frame.opcode == WebSocketFrame::OPCODE_TEXT) {
                                if (conn.wsInFragmentedMessage) {
                                    string closeFrame = WebSocketFrame::encodeCloseFrame(WebSocketFrame::CLOSE_PROTOCOL_ERROR, "Unexpected text frame");
                                    conn.outBuffer.append(closeFrame);
                                    conn.wsCloseSent = true;
                                    conn.shouldClose = true;
                                    flushSendBuffer(conn);

                                    if (conn.bytesSent >= conn.outBuffer.size()) {
                                        closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                                    } else {
                                        epoll_event ev{};
                                        ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                                        ev.data.fd = clientSocketFd;
                                        epoll_ctl(epollFd, EPOLL_CTL_MOD, clientSocketFd, &ev);
                                    }
                                    connectionClosed = true;
                                    break;
                                }

                                if (!frame.fin) {
                                    conn.wsInFragmentedMessage = true;
                                    conn.wsFragmentedOpcode = WebSocketFrame::OPCODE_TEXT;
                                    conn.wsMessageBuffer = frame.payload;
                                } else {
                                    if (!WebSocketFrame::isValidUtf8(frame.payload)) {
                                        string closeFrame = WebSocketFrame::encodeCloseFrame(WebSocketFrame::CLOSE_INVALID_PAYLOAD, "Invalid UTF-8");
                                        conn.outBuffer.append(closeFrame);
                                        conn.wsCloseSent = true;
                                        conn.shouldClose = true;
                                        flushSendBuffer(conn);

                                        if (conn.bytesSent >= conn.outBuffer.size()) {
                                            closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                                        } else {
                                            epoll_event ev{};
                                            ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                                            ev.data.fd = clientSocketFd;
                                            epoll_ctl(epollFd, EPOLL_CTL_MOD, clientSocketFd, &ev);
                                        }
                                        connectionClosed = true;
                                        break;
                                    }

                                    // Route through message handler
                                    handleWebSocketTextMessage(conn, epollFd, frame.payload, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, nextPlayerId, inputQueue);
                                }
                            }
                            // Data Frames: Continuation
                            else if (frame.opcode == WebSocketFrame::OPCODE_CONTINUATION) {
                                if (!conn.wsInFragmentedMessage) {
                                    string closeFrame = WebSocketFrame::encodeCloseFrame(WebSocketFrame::CLOSE_PROTOCOL_ERROR, "Unexpected continuation");
                                    conn.outBuffer.append(closeFrame);
                                    conn.wsCloseSent = true;
                                    conn.shouldClose = true;
                                    flushSendBuffer(conn);

                                    if (conn.bytesSent >= conn.outBuffer.size()) {
                                        closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                                    } else {
                                        epoll_event ev{};
                                        ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                                        ev.data.fd = clientSocketFd;
                                        epoll_ctl(epollFd, EPOLL_CTL_MOD, clientSocketFd, &ev);
                                    }
                                    connectionClosed = true;
                                    break;
                                }

                                if (conn.wsMessageBuffer.size() + frame.payload.size() > Connection::MAX_WS_MESSAGE_SIZE) {
                                    string closeFrame = WebSocketFrame::encodeCloseFrame(WebSocketFrame::CLOSE_MESSAGE_TOO_BIG, "Message too big");
                                    conn.outBuffer.append(closeFrame);
                                    conn.wsCloseSent = true;
                                    conn.shouldClose = true;
                                    flushSendBuffer(conn);

                                    if (conn.bytesSent >= conn.outBuffer.size()) {
                                        closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                                    } else {
                                        epoll_event ev{};
                                        ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                                        ev.data.fd = clientSocketFd;
                                        epoll_ctl(epollFd, EPOLL_CTL_MOD, clientSocketFd, &ev);
                                    }
                                    connectionClosed = true;
                                    break;
                                }

                                conn.wsMessageBuffer.append(frame.payload);
                                if (frame.fin) {
                                    conn.wsInFragmentedMessage = false;
                                    if (conn.wsFragmentedOpcode == WebSocketFrame::OPCODE_TEXT) {
                                        if (!WebSocketFrame::isValidUtf8(conn.wsMessageBuffer)) {
                                            string closeFrame = WebSocketFrame::encodeCloseFrame(WebSocketFrame::CLOSE_INVALID_PAYLOAD, "Invalid UTF-8");
                                            conn.outBuffer.append(closeFrame);
                                            conn.wsCloseSent = true;
                                            conn.shouldClose = true;
                                            flushSendBuffer(conn);

                                            if (conn.bytesSent >= conn.outBuffer.size()) {
                                                closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                                            } else {
                                                epoll_event ev{};
                                                ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                                                ev.data.fd = clientSocketFd;
                                                epoll_ctl(epollFd, EPOLL_CTL_MOD, clientSocketFd, &ev);
                                            }
                                            connectionClosed = true;
                                            break;
                                        }

                                        handleWebSocketTextMessage(conn, epollFd, conn.wsMessageBuffer, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, nextPlayerId, inputQueue);
                                    } else if (conn.wsFragmentedOpcode == WebSocketFrame::OPCODE_BINARY) {
                                        string echoFrame = WebSocketFrame::encodeFrame(WebSocketFrame::OPCODE_BINARY, conn.wsMessageBuffer, true);
                                        if (!sendWebSocketFrameSafe(conn, epollFd, echoFrame)) {
                                            closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                                            connectionClosed = true;
                                            break;
                                        }
                                    }
                                    conn.wsMessageBuffer.clear();
                                    conn.wsFragmentedOpcode = 0;
                                }
                            }
                            // Data Frames: Binary
                            else if (frame.opcode == WebSocketFrame::OPCODE_BINARY) {
                                if (conn.wsInFragmentedMessage) {
                                    string closeFrame = WebSocketFrame::encodeCloseFrame(WebSocketFrame::CLOSE_PROTOCOL_ERROR, "Unexpected binary frame");
                                    conn.outBuffer.append(closeFrame);
                                    conn.wsCloseSent = true;
                                    conn.shouldClose = true;
                                    flushSendBuffer(conn);

                                    if (conn.bytesSent >= conn.outBuffer.size()) {
                                        closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                                    } else {
                                        epoll_event ev{};
                                        ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                                        ev.data.fd = clientSocketFd;
                                        epoll_ctl(epollFd, EPOLL_CTL_MOD, clientSocketFd, &ev);
                                    }
                                    connectionClosed = true;
                                    break;
                                }

                                if (!frame.fin) {
                                    conn.wsInFragmentedMessage = true;
                                    conn.wsFragmentedOpcode = WebSocketFrame::OPCODE_BINARY;
                                    conn.wsMessageBuffer = frame.payload;
                                } else {
                                    string echoFrame = WebSocketFrame::encodeFrame(WebSocketFrame::OPCODE_BINARY, frame.payload, true);
                                    if (!sendWebSocketFrameSafe(conn, epollFd, echoFrame)) {
                                        closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue);
                                        connectionClosed = true;
                                        break;
                                    }
                                }
                            }
                        }

                        if (connectionClosed) {
                            continue;
                        }

                        if (conn.bytesSent < conn.outBuffer.size()) {
                            epoll_event ev{};
                            ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                            ev.data.fd = clientSocketFd;
                            epoll_ctl(epollFd, EPOLL_CTL_MOD, clientSocketFd, &ev);
                        } else if (!conn.outBuffer.empty()) {
                            conn.outBuffer.clear();
                            conn.bytesSent = 0;
                        }
                    }
                }
            }
        }
    }

    // Clean shutdown
    simulationThread.stop();

    for (auto& [fd, conn] : connections) {
        epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
    }
    connections.clear();

    close(snapshotWakeupFd);
    close(wakeupFd);
    close(epollFd);
    close(serverSocket);

    Logger::log("Server shut down cleanly.");
    return 0;
}
