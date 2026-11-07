#include <cstring>
#include <cctype>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <csignal>
#include <cerrno>

#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

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
    // Generate a 256 KB response for testing large payload transmission and partial writes
    string data;
    data.reserve(256 * 1024);
    for (int i = 0; i < 256; i++) {
        data += "Chunk " + to_string(i) + ": " + string(990, 'A') + "\n";
    }
    return data;
}

// Closes a connection, removes it from epoll, and erases it from the connection registry.
// Exclusively called by the event loop thread.
static void closeConnection(int fd, int epollFd, unordered_map<int, Connection>& connections) {
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
                // Socket output buffer full; remaining bytes remain in outBuffer
                return false;
            }
            if (errno == EINTR) {
                continue; // Interrupted by signal, retry immediately
            }
            // Fatal socket error
            return true;
        } else {
            // sent == 0 on stream socket send; treat as no progress
            break;
        }
    }
    return false;
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

    // Setup eventfd for worker thread completion signaling
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

    // Event-loop-owned connection registry
    unordered_map<int, Connection> connections;
    uint64_t nextConnectionId = 1;

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

            // 1. Worker thread finished processing one or more responses
            if (currentFd == wakeupFd) {
                uint64_t counter = 0;
                while (read(wakeupFd, &counter, sizeof(counter)) > 0) {}

                auto responses = responseQueue.popAll();
                for (auto& resp : responses) {
                    auto it = connections.find(resp.clientSocket);
                    // Prevent stale descriptor writes: ensure connection exists and ID matches
                    if (it == connections.end() || it->second.id != resp.connectionId) {
                        continue; // Discard stale response
                    }

                    Connection& conn = it->second;
                    conn.outBuffer = std::move(resp.rawResponse);
                    conn.bytesSent = 0;
                    conn.shouldClose = resp.closeConnection;
                    conn.state = ConnectionState::WRITING_RESPONSE;

                    // Attempt immediate non-blocking flush
                    bool sockErr = flushSendBuffer(conn);
                    if (sockErr) {
                        closeConnection(conn.fd, epollFd, connections);
                        continue;
                    }

                    if (conn.bytesSent >= conn.outBuffer.size()) {
                        // Fully transmitted
                        if (conn.shouldClose) {
                            closeConnection(conn.fd, epollFd, connections);
                        } else {
                            conn.resetForNextRequest();
                            epoll_event ev{};
                            ev.events = EPOLLIN | EPOLLRDHUP;
                            ev.data.fd = conn.fd;
                            epoll_ctl(epollFd, EPOLL_CTL_MOD, conn.fd, &ev);
                        }
                    } else {
                        // Partial write occurred: register EPOLLOUT to send remainder when writable
                        epoll_event ev{};
                        ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                        ev.data.fd = conn.fd;
                        epoll_ctl(epollFd, EPOLL_CTL_MOD, conn.fd, &ev);
                    }
                }
            }
            // 2. Incoming connection on server socket
            else if (currentFd == serverSocket) {
                while (true) {
                    sockaddr_in clientAddr{};
                    socklen_t clientLen = sizeof(clientAddr);
                    int clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientLen);

                    if (clientSocket < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break; // Backlog drained
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
            // 3. Client socket I/O events
            else {
                auto it = connections.find(currentFd);
                if (it == connections.end()) {
                    continue; // Stale event
                }

                Connection& conn = it->second;

                // Handle peer hangup or errors
                if (currentEvents & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                    // If no pending output to write, close immediately
                    if (conn.bytesSent >= conn.outBuffer.size()) {
                        closeConnection(conn.fd, epollFd, connections);
                        continue;
                    }
                }

                // Handle writable event (EPOLLOUT) for pending outgoing data
                if (currentEvents & EPOLLOUT) {
                    if (conn.bytesSent < conn.outBuffer.size()) {
                        bool sockErr = flushSendBuffer(conn);
                        if (sockErr) {
                            closeConnection(conn.fd, epollFd, connections);
                            continue;
                        }

                        if (conn.bytesSent >= conn.outBuffer.size()) {
                            // Outgoing buffer completely drained
                            conn.outBuffer.clear();
                            conn.bytesSent = 0;

                            if (conn.shouldClose) {
                                closeConnection(conn.fd, epollFd, connections);
                                continue;
                            } else {
                                // Remove EPOLLOUT, keep listening for readable data
                                epoll_event ev{};
                                ev.events = EPOLLIN | EPOLLRDHUP;
                                ev.data.fd = conn.fd;
                                epoll_ctl(epollFd, EPOLL_CTL_MOD, conn.fd, &ev);
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
                            disconnected = true; // Clean client disconnect
                            break;
                        } else {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                break; // Drained socket buffer
                            }
                            if (errno == EINTR) {
                                continue; // Interrupted by signal, retry
                            }
                            disconnected = true;
                            break;
                        }
                    }

                    if (disconnected) {
                        closeConnection(conn.fd, epollFd, connections);
                        continue;
                    }

                    // A. Protocol: HTTP (Awaiting Request or Handshake)
                    if (conn.protocol == ConnectionProtocol::HTTP) {
                        size_t requestLen = 0;
                        auto parseResult = conn.checkRequest(requestLen);

                        if (parseResult == Connection::ParseResult::COMPLETE) {
                            string rawRequest = conn.inBuffer.substr(0, requestLen);
                            conn.inBuffer.erase(0, requestLen);

                            // Check if this is a WebSocket Upgrade Request
                            if (WebSocketHandshake::isUpgradeRequest(rawRequest)) {
                                string responseText, clientKey;
                                auto hsResult = WebSocketHandshake::validateHandshake(rawRequest, responseText, clientKey);

                                if (hsResult == WebSocketHandshake::HandshakeResult::SUCCESS) {
                                    // Successfully upgraded to WebSocket!
                                    conn.protocol = ConnectionProtocol::WEBSOCKET;
                                    conn.shouldClose = false;
                                    conn.outBuffer = std::move(responseText);
                                    conn.bytesSent = 0;
                                    conn.state = ConnectionState::WRITING_RESPONSE;

                                    bool sockErr = flushSendBuffer(conn);
                                    if (sockErr) {
                                        closeConnection(conn.fd, epollFd, connections);
                                        continue;
                                    }

                                    if (conn.bytesSent >= conn.outBuffer.size()) {
                                        conn.outBuffer.clear();
                                        conn.bytesSent = 0;
                                        epoll_event ev{};
                                        ev.events = EPOLLIN | EPOLLRDHUP;
                                        ev.data.fd = conn.fd;
                                        epoll_ctl(epollFd, EPOLL_CTL_MOD, conn.fd, &ev);
                                    } else {
                                        epoll_event ev{};
                                        ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                                        ev.data.fd = conn.fd;
                                        epoll_ctl(epollFd, EPOLL_CTL_MOD, conn.fd, &ev);
                                    }
                                } else {
                                    // Handshake rejected with appropriate HTTP error
                                    conn.outBuffer = std::move(responseText);
                                    conn.bytesSent = 0;
                                    conn.shouldClose = true;
                                    conn.state = ConnectionState::WRITING_RESPONSE;

                                    flushSendBuffer(conn);
                                    if (conn.bytesSent >= conn.outBuffer.size()) {
                                        closeConnection(conn.fd, epollFd, connections);
                                    } else {
                                        epoll_event ev{};
                                        ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                                        ev.data.fd = conn.fd;
                                        epoll_ctl(epollFd, EPOLL_CTL_MOD, conn.fd, &ev);
                                    }
                                }
                            } else {
                                // Regular HTTP request: hand off to ThreadPool worker
                                conn.state = ConnectionState::PROCESSING;

                                // Temporarily disable EPOLLIN while request is being processed
                                epoll_event ev{};
                                ev.events = EPOLLRDHUP;
                                ev.data.fd = conn.fd;
                                epoll_ctl(epollFd, EPOLL_CTL_MOD, conn.fd, &ev);

                                Task task;
                                task.connectionId = conn.id;
                                task.clientSocket = conn.fd;
                                task.rawRequest = std::move(rawRequest);
                                task.router = &router;

                                taskQueue.push(std::move(task));
                            }
                        } else if (parseResult == Connection::ParseResult::TOO_LARGE) {
                            sendImmediateError(conn.fd, 431, "Request Header Fields Too Large");
                            closeConnection(conn.fd, epollFd, connections);
                            continue;
                        } else if (parseResult == Connection::ParseResult::BAD_REQUEST) {
                            sendImmediateError(conn.fd, 400, "Bad Request");
                            closeConnection(conn.fd, epollFd, connections);
                            continue;
                        }
                    }
                    // B. Protocol: WEBSOCKET (Incremental frame parsing & Echo Application)
                    else if (conn.protocol == ConnectionProtocol::WEBSOCKET) {
                        bool fatalError = false;

                        while (!conn.inBuffer.empty()) {
                            WebSocketFrame::Frame frame;
                            size_t bytesConsumed = 0;
                            uint16_t closeCode = WebSocketFrame::CLOSE_NORMAL;
                            auto status = WebSocketFrame::parseFrame(conn.inBuffer, frame, bytesConsumed, closeCode);

                            if (status == WebSocketFrame::ParseStatus::NEED_MORE_DATA) {
                                break; // Wait for more TCP segments
                            }

                            if (status == WebSocketFrame::ParseStatus::PROTOCOL_ERROR) {
                                string closeFrame = WebSocketFrame::encodeCloseFrame(closeCode, "Protocol Error");
                                conn.outBuffer.append(closeFrame);
                                conn.shouldClose = true;
                                flushSendBuffer(conn);

                                if (conn.bytesSent >= conn.outBuffer.size()) {
                                    closeConnection(conn.fd, epollFd, connections);
                                } else {
                                    epoll_event ev{};
                                    ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                                    ev.data.fd = conn.fd;
                                    epoll_ctl(epollFd, EPOLL_CTL_MOD, conn.fd, &ev);
                                }
                                fatalError = true;
                                break;
                            }

                            // Frame is complete
                            conn.inBuffer.erase(0, bytesConsumed);

                            // Control Frames
                            if (frame.opcode == WebSocketFrame::OPCODE_PING) {
                                string pong = WebSocketFrame::encodePongFrame(frame.payload);
                                conn.outBuffer.append(pong);
                                flushSendBuffer(conn);
                            } else if (frame.opcode == WebSocketFrame::OPCODE_PONG) {
                                // Ignore unsolicited/solicited pong
                            } else if (frame.opcode == WebSocketFrame::OPCODE_CLOSE) {
                                conn.wsCloseReceived = true;
                                if (!conn.wsCloseSent) {
                                    conn.wsCloseSent = true;
                                    uint16_t respCode = (frame.closeCode != 0) ? frame.closeCode : WebSocketFrame::CLOSE_NORMAL;
                                    string closeResponse = WebSocketFrame::encodeCloseFrame(respCode, "");
                                    conn.outBuffer.append(closeResponse);
                                }
                                conn.shouldClose = true;
                                flushSendBuffer(conn);

                                if (conn.bytesSent >= conn.outBuffer.size()) {
                                    closeConnection(conn.fd, epollFd, connections);
                                } else {
                                    epoll_event ev{};
                                    ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                                    ev.data.fd = conn.fd;
                                    epoll_ctl(epollFd, EPOLL_CTL_MOD, conn.fd, &ev);
                                }
                                fatalError = true;
                                break;
                            }
                            // Data Frames: Text
                            else if (frame.opcode == WebSocketFrame::OPCODE_TEXT) {
                                if (conn.wsInFragmentedMessage) {
                                    string closeFrame = WebSocketFrame::encodeCloseFrame(WebSocketFrame::CLOSE_PROTOCOL_ERROR, "Unexpected text frame");
                                    conn.outBuffer.append(closeFrame);
                                    conn.shouldClose = true;
                                    flushSendBuffer(conn);
                                    fatalError = true;
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
                                        conn.shouldClose = true;
                                        flushSendBuffer(conn);
                                        fatalError = true;
                                        break;
                                    }
                                    // Echo back text frame
                                    string echoFrame = WebSocketFrame::encodeTextFrame(frame.payload);
                                    conn.outBuffer.append(echoFrame);
                                    flushSendBuffer(conn);
                                }
                            }
                            // Data Frames: Continuation
                            else if (frame.opcode == WebSocketFrame::OPCODE_CONTINUATION) {
                                if (!conn.wsInFragmentedMessage) {
                                    string closeFrame = WebSocketFrame::encodeCloseFrame(WebSocketFrame::CLOSE_PROTOCOL_ERROR, "Unexpected continuation");
                                    conn.outBuffer.append(closeFrame);
                                    conn.shouldClose = true;
                                    flushSendBuffer(conn);
                                    fatalError = true;
                                    break;
                                }

                                conn.wsMessageBuffer.append(frame.payload);
                                if (frame.fin) {
                                    conn.wsInFragmentedMessage = false;
                                    if (conn.wsFragmentedOpcode == WebSocketFrame::OPCODE_TEXT) {
                                        if (!WebSocketFrame::isValidUtf8(conn.wsMessageBuffer)) {
                                            string closeFrame = WebSocketFrame::encodeCloseFrame(WebSocketFrame::CLOSE_INVALID_PAYLOAD, "Invalid UTF-8");
                                            conn.outBuffer.append(closeFrame);
                                            conn.shouldClose = true;
                                            flushSendBuffer(conn);
                                            fatalError = true;
                                            break;
                                        }
                                        string echoFrame = WebSocketFrame::encodeTextFrame(conn.wsMessageBuffer);
                                        conn.outBuffer.append(echoFrame);
                                        flushSendBuffer(conn);
                                    }
                                    conn.wsMessageBuffer.clear();
                                }
                            }
                            // Data Frames: Binary
                            else if (frame.opcode == WebSocketFrame::OPCODE_BINARY) {
                                if (frame.fin) {
                                    string echoFrame = WebSocketFrame::encodeFrame(WebSocketFrame::OPCODE_BINARY, frame.payload, true);
                                    conn.outBuffer.append(echoFrame);
                                    flushSendBuffer(conn);
                                }
                            }
                        }

                        if (fatalError) {
                            if (connections.find(conn.fd) != connections.end() && conn.bytesSent >= conn.outBuffer.size()) {
                                closeConnection(conn.fd, epollFd, connections);
                            }
                            continue;
                        }

                        // Register EPOLLOUT if data is queued in outBuffer
                        if (conn.bytesSent < conn.outBuffer.size()) {
                            epoll_event ev{};
                            ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                            ev.data.fd = conn.fd;
                            epoll_ctl(epollFd, EPOLL_CTL_MOD, conn.fd, &ev);
                        } else if (!conn.outBuffer.empty()) {
                            conn.outBuffer.clear();
                            conn.bytesSent = 0;
                        }
                    }
                }
            }
        }
    }

    // Clean shutdown: close all remaining client connections
    for (auto& [fd, conn] : connections) {
        epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
    }
    connections.clear();

    close(wakeupFd);
    close(epollFd);
    close(serverSocket);

    Logger::log("Server shut down cleanly.");
    return 0;
}
