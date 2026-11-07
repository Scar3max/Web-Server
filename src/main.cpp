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
#include "Room.h"
#include "RoomManager.h"
#include "RoomProtocol.h"
#include "ServerMetrics.h"
#include "BinarySnapshot.h"
#include "Config.h"

using namespace std;

constexpr int RECV_BUFFER_SIZE  = 4096;

static volatile sig_atomic_t g_running = 1;

static void handleSignal(int sig) {
    (void)sig;
    g_running = 0;
}

static string sanitizePlayerName(const string& raw, uint32_t pid) {
    string cleaned;
    cleaned.reserve(raw.size());
    for (char c : raw) {
        if (static_cast<unsigned char>(c) >= 32 && static_cast<unsigned char>(c) <= 126) {
            cleaned.push_back(c);
        }
    }
    size_t first = cleaned.find_first_not_of(" \t");
    if (first == string::npos) {
        return "Player " + to_string(pid);
    }
    size_t last = cleaned.find_last_not_of(" \t");
    cleaned = cleaned.substr(first, (last - first + 1));
    if (cleaned.size() > 32) {
        cleaned = cleaned.substr(0, 32);
    }
    return cleaned;
}

static string sanitizeRoomCode(const string& raw) {
    string cleaned;
    cleaned.reserve(raw.size());
    for (char c : raw) {
        if (isalnum(static_cast<unsigned char>(c))) {
            cleaned.push_back(static_cast<char>(toupper(static_cast<unsigned char>(c))));
        }
    }
    if (cleaned.size() > 8) {
        cleaned = cleaned.substr(0, 8);
    }
    return cleaned;
}

static string LargeHandler() {
    string data;
    data.reserve(256 * 1024);
    for (int i = 0; i < 256; i++) {
        data += "Chunk " + to_string(i) + ": " + string(990, 'A') + "\n";
    }
    return data;
}

// Forward declaration of closeConnection
static void closeConnection(int fd, int epollFd,
                            unordered_map<int, Connection>& connections,
                            unordered_map<int, uint32_t>& socketToPlayer,
                            unordered_map<uint32_t, int>& playerToSocket,
                            unordered_map<uint32_t, uint64_t>& playerLastSeq,
                            unordered_map<uint32_t, std::chrono::steady_clock::time_point>& playerLastTime,
                            InputQueue& inputQueue,
                            RoomManager& roomManager);

// Sends an immediate HTTP error response from the event loop
static void sendImmediateError(int fd, int statusCode, const string& reason) {
    ServerMetrics::instance().recordHttpRequest(statusCode);
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
            ServerMetrics::instance().recordBytesTransmitted(sent);
            ServerMetrics::instance().recordOutputBufferSize(conn.outBuffer.size() - conn.bytesSent);
            if (static_cast<size_t>(sent) < remaining) {
                ServerMetrics::instance().recordPartialWrite();
            }
        } else if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ServerMetrics::instance().recordEagain();
                return false;
            }
            if (errno == EINTR) {
                continue;
            }
            ServerMetrics::instance().recordConnectionDrop();
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
        ServerMetrics::instance().backpressureControlFrameCongestionTotal.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    conn.outBuffer.append(frame);
    ServerMetrics::instance().wsFramesSentTotal.fetch_add(1, std::memory_order_relaxed);
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

// Removes a player from the active simulation registry and rooms, and queues a LEAVE event
static void removePlayerIfPresent(int fd,
                                  int epollFd,
                                  unordered_map<int, Connection>& connections,
                                  unordered_map<int, uint32_t>& socketToPlayer,
                                  unordered_map<uint32_t, int>& playerToSocket,
                                  unordered_map<uint32_t, uint64_t>& playerLastSeq,
                                  unordered_map<uint32_t, std::chrono::steady_clock::time_point>& playerLastTime,
                                  InputQueue& inputQueue,
                                  RoomManager& roomManager) {
    auto it = socketToPlayer.find(fd);
    if (it != socketToPlayer.end()) {
        uint32_t pid = it->second;
        socketToPlayer.erase(it);
        playerToSocket.erase(pid);
        playerLastSeq.erase(pid);
        playerLastTime.erase(pid);

        uint32_t oldRoomId = 0;
        bool roomDestroyed = false;
        uint32_t newHostId = 0;
        bool countdownCancelled = false;

        if (roomManager.leaveRoom(pid, oldRoomId, roomDestroyed, newHostId, countdownCancelled)) {
            if (roomDestroyed) {
                PlayerEvent ev;
                ev.type = PlayerEvent::Type::DESTROY_ROOM;
                ev.roomId = oldRoomId;
                inputQueue.pushEvent(ev);
            } else {
                Room* room = roomManager.getRoom(oldRoomId);
                if (room) {
                    if (countdownCancelled) {
                        PlayerEvent ev;
                        ev.type = PlayerEvent::Type::CANCEL_COUNTDOWN;
                        ev.roomId = oldRoomId;
                        inputQueue.pushEvent(ev);
                    }
                    PlayerEvent evLeave;
                    evLeave.type = PlayerEvent::Type::ROOM_LEAVE;
                    evLeave.roomId = oldRoomId;
                    evLeave.playerId = pid;
                    inputQueue.pushEvent(evLeave);

                    string playerLeftMsg = WebSocketFrame::encodeTextFrame(RoomProtocol::makePlayerLeft(*room, pid, newHostId));
                    string roomUpdatedMsg = WebSocketFrame::encodeTextFrame(RoomProtocol::makeRoomUpdated(*room));
                    for (const auto& [mPid, m] : room->members) {
                        auto cIt = connections.find(m.socketFd);
                        if (cIt != connections.end() && cIt->second.protocol == ConnectionProtocol::WEBSOCKET) {
                            sendWebSocketFrameSafe(cIt->second, epollFd, playerLeftMsg);
                            sendWebSocketFrameSafe(cIt->second, epollFd, roomUpdatedMsg);
                        }
                    }
                }
            }
        }

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
                            InputQueue& inputQueue,
                            RoomManager& roomManager) {
    removePlayerIfPresent(fd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
    auto it = connections.find(fd);
    if (it != connections.end()) {
        ServerMetrics::instance().recordTcpClose();
        if (it->second.protocol == ConnectionProtocol::WEBSOCKET) {
            ServerMetrics::instance().recordWebSocketClose();
        }
        epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        connections.erase(it);
    }
}

// Helper to look up or admit a new player to the server
static uint32_t getOrAdmitPlayer(Connection& conn,
                                 int epollFd,
                                 unordered_map<int, Connection>& connections,
                                 unordered_map<int, uint32_t>& socketToPlayer,
                                 unordered_map<uint32_t, int>& playerToSocket,
                                 unordered_map<uint32_t, uint64_t>& playerLastSeq,
                                 unordered_map<uint32_t, std::chrono::steady_clock::time_point>& playerLastTime,
                                 uint32_t& nextPlayerId,
                                 InputQueue& inputQueue,
                                 RoomManager& roomManager) {
    auto it = socketToPlayer.find(conn.fd);
    if (it != socketToPlayer.end()) {
        return it->second;
    }

    PlayerEvent ev;
    ev.type = PlayerEvent::Type::JOIN;
    ev.playerId = nextPlayerId;
    ev.socketFd = conn.fd;
    ev.connectionId = conn.id;

    if (!inputQueue.pushJoin(ev)) {
        json::Value errObj(json::Type::OBJECT);
        errObj["type"] = "error";
        errObj["message"] = "Server player capacity reached; please retry later";
        string errFrame = WebSocketFrame::encodeTextFrame(errObj.serialize());
        sendWebSocketFrameSafe(conn, epollFd, errFrame);

        string closeFrame = WebSocketFrame::encodeCloseFrame(1013, "Try Again Later");
        sendWebSocketFrameSafe(conn, epollFd, closeFrame);
        conn.shouldClose = true;
        return 0;
    }

    uint32_t pid = nextPlayerId++;
    socketToPlayer[conn.fd] = pid;
    playerToSocket[pid] = conn.fd;
    playerLastSeq[pid] = 0;
    playerLastTime[pid] = std::chrono::steady_clock::now();

    json::Value welcome(json::Type::OBJECT);
    welcome["type"] = "welcome";
    welcome["playerId"] = static_cast<int64_t>(pid);
    string welcomeFrame = WebSocketFrame::encodeTextFrame(welcome.serialize());
    if (!sendWebSocketFrameSafe(conn, epollFd, welcomeFrame)) {
        closeConnection(conn.fd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
        return 0;
    }

    return pid;
}

// Processes incoming WebSocket text message (room protocol, input protocol, telemetry echo, or plain text echo)
static void handleWebSocketTextMessage(Connection& conn,
                                       int epollFd,
                                       const string& payload,
                                       unordered_map<int, Connection>& connections,
                                       unordered_map<int, uint32_t>& socketToPlayer,
                                       unordered_map<uint32_t, int>& playerToSocket,
                                       unordered_map<uint32_t, uint64_t>& playerLastSeq,
                                       unordered_map<uint32_t, std::chrono::steady_clock::time_point>& playerLastTime,
                                       uint32_t& nextPlayerId,
                                       InputQueue& inputQueue,
                                       RoomManager& roomManager) {
    json::Value root;
    std::string err;
    bool isJsonCandidate = (!payload.empty() && (payload.front() == '{' || payload.front() == '['));
    if (!json::parse(payload, root, err) || !root.isObject()) {
        if (isJsonCandidate) {
            return;
        }
        string echo = WebSocketFrame::encodeTextFrame(payload);
        if (!sendWebSocketFrameSafe(conn, epollFd, echo)) {
            closeConnection(conn.fd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
        }
        return;
    }

    std::string type = root["type"].asString();

    if (type == "create_room") {
        uint32_t pid = getOrAdmitPlayer(conn, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, nextPlayerId, inputQueue, roomManager);
        if (pid == 0) return;

        string rawName = (root.contains("name") && root["name"].isString()) ? root["name"].asString() : "";
        string name = sanitizePlayerName(rawName, pid);
        double trackLen = (root.contains("trackLength") && root["trackLength"].isNumber()) ? root["trackLength"].asDouble() : 1500.0;
        if (trackLen < 50.0) trackLen = 50.0;
        string code, error;
        uint32_t roomId = 0;
        if (!roomManager.createRoom(pid, conn.fd, name, code, roomId, error)) {
            string errFrame = WebSocketFrame::encodeTextFrame(RoomProtocol::makeError(error));
            sendWebSocketFrameSafe(conn, epollFd, errFrame);
            return;
        }

        Room* room = roomManager.getRoom(roomId);
        if (room) {
            room->trackLength = trackLen;
            PlayerEvent ev;
            ev.type = PlayerEvent::Type::ROOM_JOIN;
            ev.roomId = roomId;
            ev.roomCode = code;
            ev.playerId = pid;
            ev.socketFd = conn.fd;
            ev.connectionId = conn.id;
            ev.playerName = name;
            ev.lane = 0;
            ev.trackLength = room->trackLength;
            if (!inputQueue.pushEvent(ev)) {
                uint32_t oldRoomId = 0;
                bool roomDestroyed = false;
                uint32_t newHostId = 0;
                bool countdownCancelled = false;
                roomManager.leaveRoom(pid, oldRoomId, roomDestroyed, newHostId, countdownCancelled);
                string errFrame = WebSocketFrame::encodeTextFrame(RoomProtocol::makeError("Server event queue congested, could not create room"));
                sendWebSocketFrameSafe(conn, epollFd, errFrame);
                return;
            }

            string createdMsg = WebSocketFrame::encodeTextFrame(RoomProtocol::makeRoomCreated(*room, pid));
            sendWebSocketFrameSafe(conn, epollFd, createdMsg);
        }
    } else if (type == "join_room") {
        uint32_t pid = getOrAdmitPlayer(conn, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, nextPlayerId, inputQueue, roomManager);
        if (pid == 0) return;

        if (!root.contains("code") || !root["code"].isString()) {
            string errFrame = WebSocketFrame::encodeTextFrame(RoomProtocol::makeError("Missing room code"));
            sendWebSocketFrameSafe(conn, epollFd, errFrame);
            return;
        }

        string code = sanitizeRoomCode(root["code"].asString());
        if (code.empty()) {
            string errFrame = WebSocketFrame::encodeTextFrame(RoomProtocol::makeError("Invalid room code"));
            sendWebSocketFrameSafe(conn, epollFd, errFrame);
            return;
        }
        string rawName = (root.contains("name") && root["name"].isString()) ? root["name"].asString() : "";
        string name = sanitizePlayerName(rawName, pid);
        string error;
        uint32_t roomId = 0;
        if (!roomManager.joinRoom(code, pid, conn.fd, name, roomId, error)) {
            string errFrame = WebSocketFrame::encodeTextFrame(RoomProtocol::makeError(error));
            sendWebSocketFrameSafe(conn, epollFd, errFrame);
            return;
        }

        Room* room = roomManager.getRoom(roomId);
        if (room) {
            const auto& member = room->members[pid];
            PlayerEvent ev;
            ev.type = PlayerEvent::Type::ROOM_JOIN;
            ev.roomId = roomId;
            ev.roomCode = room->code;
            ev.playerId = pid;
            ev.socketFd = conn.fd;
            ev.connectionId = conn.id;
            ev.playerName = member.name;
            ev.lane = member.lane;
            ev.trackLength = room->trackLength;
            if (!inputQueue.pushEvent(ev)) {
                uint32_t oldRoomId = 0;
                bool roomDestroyed = false;
                uint32_t newHostId = 0;
                bool countdownCancelled = false;
                roomManager.leaveRoom(pid, oldRoomId, roomDestroyed, newHostId, countdownCancelled);
                string errFrame = WebSocketFrame::encodeTextFrame(RoomProtocol::makeError("Server event queue congested, could not join room"));
                sendWebSocketFrameSafe(conn, epollFd, errFrame);
                return;
            }

            string joinedMsg = WebSocketFrame::encodeTextFrame(RoomProtocol::makeRoomJoined(*room, pid));
            sendWebSocketFrameSafe(conn, epollFd, joinedMsg);

            string playerJoinedMsg = WebSocketFrame::encodeTextFrame(RoomProtocol::makePlayerJoined(*room, pid, member.name));
            string roomUpdatedMsg = WebSocketFrame::encodeTextFrame(RoomProtocol::makeRoomUpdated(*room));
            for (const auto& [mPid, m] : room->members) {
                auto cIt = connections.find(m.socketFd);
                if (cIt != connections.end() && cIt->second.protocol == ConnectionProtocol::WEBSOCKET) {
                    sendWebSocketFrameSafe(cIt->second, epollFd, playerJoinedMsg);
                    sendWebSocketFrameSafe(cIt->second, epollFd, roomUpdatedMsg);
                }
            }
        }
    } else if (type == "leave_room") {
        auto it = socketToPlayer.find(conn.fd);
        if (it == socketToPlayer.end()) {
            string errFrame = WebSocketFrame::encodeTextFrame(RoomProtocol::makeError("Player is not in any room"));
            sendWebSocketFrameSafe(conn, epollFd, errFrame);
            return;
        }
        uint32_t pid = it->second;

        uint32_t oldRoomId = 0;
        bool roomDestroyed = false;
        uint32_t newHostId = 0;
        bool countdownCancelled = false;

        if (!roomManager.leaveRoom(pid, oldRoomId, roomDestroyed, newHostId, countdownCancelled)) {
            string errFrame = WebSocketFrame::encodeTextFrame(RoomProtocol::makeError("Player is not in any room"));
            sendWebSocketFrameSafe(conn, epollFd, errFrame);
            return;
        }

        json::Value leftRes(json::Type::OBJECT);
        leftRes["type"] = "room_left";
        leftRes["roomId"] = static_cast<int64_t>(oldRoomId);
        string leftResFrame = WebSocketFrame::encodeTextFrame(leftRes.serialize());
        sendWebSocketFrameSafe(conn, epollFd, leftResFrame);

        if (roomDestroyed) {
            PlayerEvent ev;
            ev.type = PlayerEvent::Type::DESTROY_ROOM;
            ev.roomId = oldRoomId;
            inputQueue.pushEvent(ev);
        } else {
            Room* room = roomManager.getRoom(oldRoomId);
            if (room) {
                if (countdownCancelled) {
                    PlayerEvent ev;
                    ev.type = PlayerEvent::Type::CANCEL_COUNTDOWN;
                    ev.roomId = oldRoomId;
                    inputQueue.pushEvent(ev);
                }
                PlayerEvent evLeave;
                evLeave.type = PlayerEvent::Type::ROOM_LEAVE;
                evLeave.roomId = oldRoomId;
                evLeave.playerId = pid;
                inputQueue.pushEvent(evLeave);

                string playerLeftMsg = WebSocketFrame::encodeTextFrame(RoomProtocol::makePlayerLeft(*room, pid, newHostId));
                string roomUpdatedMsg = WebSocketFrame::encodeTextFrame(RoomProtocol::makeRoomUpdated(*room));
                for (const auto& [mPid, m] : room->members) {
                    auto cIt = connections.find(m.socketFd);
                    if (cIt != connections.end() && cIt->second.protocol == ConnectionProtocol::WEBSOCKET) {
                        sendWebSocketFrameSafe(cIt->second, epollFd, playerLeftMsg);
                        sendWebSocketFrameSafe(cIt->second, epollFd, roomUpdatedMsg);
                    }
                }
            }
        }
    } else if (type == "set_ready") {
        auto it = socketToPlayer.find(conn.fd);
        if (it == socketToPlayer.end()) {
            string errFrame = WebSocketFrame::encodeTextFrame(RoomProtocol::makeError("Player is not in any room"));
            sendWebSocketFrameSafe(conn, epollFd, errFrame);
            return;
        }
        uint32_t pid = it->second;

        bool ready = root.contains("ready") && root["ready"].asBool();
        uint32_t roomId = 0;
        bool countdownCancelled = false;
        string error;
        if (!roomManager.setReady(pid, ready, roomId, countdownCancelled, error)) {
            string errFrame = WebSocketFrame::encodeTextFrame(RoomProtocol::makeError(error));
            sendWebSocketFrameSafe(conn, epollFd, errFrame);
            return;
        }

        Room* room = roomManager.getRoom(roomId);
        if (room) {
            if (countdownCancelled) {
                PlayerEvent ev;
                ev.type = PlayerEvent::Type::CANCEL_COUNTDOWN;
                ev.roomId = roomId;
                inputQueue.pushEvent(ev);
            }

            string readyMsg = WebSocketFrame::encodeTextFrame(RoomProtocol::makeReadyUpdated(*room, pid, ready));
            string updatedMsg = WebSocketFrame::encodeTextFrame(RoomProtocol::makeRoomUpdated(*room));
            for (const auto& [mPid, m] : room->members) {
                auto cIt = connections.find(m.socketFd);
                if (cIt != connections.end() && cIt->second.protocol == ConnectionProtocol::WEBSOCKET) {
                    sendWebSocketFrameSafe(cIt->second, epollFd, readyMsg);
                    sendWebSocketFrameSafe(cIt->second, epollFd, updatedMsg);
                }
            }
        }
    } else if (type == "start_race") {
        auto it = socketToPlayer.find(conn.fd);
        if (it == socketToPlayer.end()) {
            string errFrame = WebSocketFrame::encodeTextFrame(RoomProtocol::makeError("Player is not in any room"));
            sendWebSocketFrameSafe(conn, epollFd, errFrame);
            return;
        }
        uint32_t pid = it->second;

        uint32_t roomId = 0;
        string error;
        if (!roomManager.startRace(pid, roomId, error)) {
            string errFrame = WebSocketFrame::encodeTextFrame(RoomProtocol::makeError(error));
            sendWebSocketFrameSafe(conn, epollFd, errFrame);
            return;
        }

        Room* room = roomManager.getRoom(roomId);
        if (room) {
            PlayerEvent ev;
            ev.type = PlayerEvent::Type::START_COUNTDOWN;
            ev.roomId = roomId;
            ev.roomCode = room->code;
            ev.trackLength = room->trackLength;
            if (!inputQueue.pushEvent(ev)) {
                room->state = RoomState::WAITING;
                string errFrame = WebSocketFrame::encodeTextFrame(RoomProtocol::makeError("Server event queue congested, could not start race"));
                sendWebSocketFrameSafe(conn, epollFd, errFrame);
                return;
            }

            string countdownMsg = WebSocketFrame::encodeTextFrame(RoomProtocol::makeCountdown(*room, room->countdownSeconds));
            string updatedMsg = WebSocketFrame::encodeTextFrame(RoomProtocol::makeRoomUpdated(*room));
            for (const auto& [mPid, m] : room->members) {
                auto cIt = connections.find(m.socketFd);
                if (cIt != connections.end() && cIt->second.protocol == ConnectionProtocol::WEBSOCKET) {
                    sendWebSocketFrameSafe(cIt->second, epollFd, countdownMsg);
                    sendWebSocketFrameSafe(cIt->second, epollFd, updatedMsg);
                }
            }
        }
    } else if (type == "get_room_info") {
        auto it = socketToPlayer.find(conn.fd);
        if (it == socketToPlayer.end()) {
            string errFrame = WebSocketFrame::encodeTextFrame(RoomProtocol::makeError("Player is not in any room"));
            sendWebSocketFrameSafe(conn, epollFd, errFrame);
            return;
        }
        uint32_t pid = it->second;
        Room* room = roomManager.getRoomForPlayer(pid);
        if (!room) {
            string errFrame = WebSocketFrame::encodeTextFrame(RoomProtocol::makeError("Player is not in any room"));
            sendWebSocketFrameSafe(conn, epollFd, errFrame);
            return;
        }
        string updatedMsg = WebSocketFrame::encodeTextFrame(RoomProtocol::makeRoomUpdated(*room));
        sendWebSocketFrameSafe(conn, epollFd, updatedMsg);
    } else if (type == "input") {
        uint32_t pid = getOrAdmitPlayer(conn, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, nextPlayerId, inputQueue, roomManager);
        if (pid == 0) return;

        uint32_t roomId = roomManager.getPlayerRoomId(pid);
        if (roomId != 0) {
            Room* room = roomManager.getRoom(roomId);
            if (!room || (room->state != RoomState::RACING && room->state != RoomState::COUNTDOWN)) {
                return;
            }
        }

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
            return;
        }

        auto now = std::chrono::steady_clock::now();
        auto lastTime = playerLastTime[pid];
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTime).count();
        if (elapsedMs < 6) {
            return;
        }

        playerLastSeq[pid] = seq;
        playerLastTime[pid] = now;

        PlayerInput in;
        in.roomId = roomId;
        in.playerId = pid;
        in.seq = seq;
        in.controls.up = cObj["up"].asBool();
        in.controls.down = cObj["down"].asBool();
        in.controls.left = cObj["left"].asBool();
        in.controls.right = cObj["right"].asBool();

        if (root.contains("format") && root["format"].isString()) {
            string fmt = root["format"].asString();
            if (fmt == "binary" || fmt == "bin") {
                conn.snapshotFormat = Connection::SnapshotFormat::BINARY;
            } else if (fmt == "json") {
                conn.snapshotFormat = Connection::SnapshotFormat::JSON;
            }
        }

        inputQueue.pushInput(in);
    } else if (type == "set_format") {
        if (root.contains("format") && root["format"].isString()) {
            string fmt = root["format"].asString();
            if (fmt == "binary" || fmt == "bin") {
                conn.snapshotFormat = Connection::SnapshotFormat::BINARY;
            } else if (fmt == "json") {
                conn.snapshotFormat = Connection::SnapshotFormat::JSON;
            }
        }
    } else if (type == "subscribe_binary") {
        conn.snapshotFormat = Connection::SnapshotFormat::BINARY;
    } else if (type == "telemetry") {
        string echo = WebSocketFrame::encodeTextFrame(payload);
        if (!sendWebSocketFrameSafe(conn, epollFd, echo)) {
            closeConnection(conn.fd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
        }
    } else {
        string echo = WebSocketFrame::encodeTextFrame(payload);
        if (!sendWebSocketFrameSafe(conn, epollFd, echo)) {
            closeConnection(conn.fd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
        }
    }
}

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    ServerConfig config;
    string configErr;
    int parseRes = ServerConfig::parse(argc, argv, config, configErr);
    if (parseRes > 0) {
        return 0; // --help was displayed
    }
    if (parseRes < 0) {
        cerr << "Configuration Error: " << configErr << "\n";
        return 1;
    }

    Logger::init(config.logLevel);

    Router router(config.staticDir);
    router.addRoute("/", [&config]() -> HttpResponse {
        string content;
        if (FileManager::readFileBinary(config.staticDir + "/index.html", content)) {
            return HttpResponse{200, "text/html; charset=utf-8", std::move(content)};
        }
        return HttpResponse{404, "text/html; charset=utf-8", "<h1>404 Not Found</h1>"};
    });
    router.addRoute("/about", [&config]() -> HttpResponse {
        string content;
        if (FileManager::readFileBinary(config.staticDir + "/about.html", content)) {
            return HttpResponse{200, "text/html; charset=utf-8", std::move(content)};
        }
        return HttpResponse{404, "text/html; charset=utf-8", "<h1>404 Not Found</h1>"};
    });
    router.addRoute("/large", LargeHandler);

    auto serverStartTime = std::chrono::steady_clock::now();
    router.addRoute("/health", [&config, serverStartTime](const HttpRequest&) -> HttpResponse {
        HttpResponse res;
        res.status = 200;
        res.contentType = "application/json; charset=utf-8";
        auto now = std::chrono::steady_clock::now();
        double uptime = std::chrono::duration<double>(now - serverStartTime).count();
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(3);
        ss << "{\"status\":\"ok\",\"uptime_seconds\":" << uptime
           << ",\"version\":\"1.0.0\""
           << ",\"port\":" << config.port << "}";
        res.body = ss.str();
        return res;
    });

    TaskQueue taskQueue;
    ResponseQueue responseQueue;
    ThreadPool pool(config.threadPoolWorkers, taskQueue, responseQueue);

    InputQueue inputQueue;
    SnapshotQueue snapshotQueue;
    SimulationThread simulationThread(inputQueue, snapshotQueue);
    RoomManager roomManager;

    router.addRoute("/metrics", [](const HttpRequest& req) -> HttpResponse {
        HttpResponse res;
        res.status = 200;

        bool wantsJson = (req.query.find("format=json") != std::string::npos ||
                          req.accept.find("application/json") != std::string::npos ||
                          req.userAgent.find("Python-urllib") != std::string::npos);

        if (req.query.find("format=prometheus") != std::string::npos ||
            req.path == "/prometheus" ||
            req.path == "/metrics.prom") {
            wantsJson = false;
        }

        if (wantsJson) {
            res.contentType = "application/json; charset=utf-8";
            res.body = ServerMetrics::instance().toJson();
        } else {
            res.contentType = "text/plain; version=0.0.4; charset=utf-8";
            res.body = ServerMetrics::instance().toPrometheusText();
        }
        return res;
    });

    router.addRoute("/prometheus", [](const HttpRequest&) -> HttpResponse {
        HttpResponse res;
        res.status = 200;
        res.contentType = "text/plain; version=0.0.4; charset=utf-8";
        res.body = ServerMetrics::instance().toPrometheusText();
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
    serverAddr.sin_port = htons(static_cast<uint16_t>(config.port));
    if (config.host == "0.0.0.0" || config.host.empty()) {
        serverAddr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, config.host.c_str(), &serverAddr.sin_addr) <= 0) {
            cerr << "Invalid listen host address: " << config.host << "\n";
            close(serverSocket);
            exit(EXIT_FAILURE);
        }
    }
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

    if (listen(serverSocket, config.listenBacklog) < 0) {
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

    vector<epoll_event> events(config.maxEpollEvents);
    Logger::info("Server started on " + config.host + ":" + to_string(config.port) +
                 " (static dir: " + config.staticDir + ", workers: " + to_string(config.threadPoolWorkers) + ")");

    while (g_running) {
        int readyCount = epoll_wait(epollFd, events.data(), config.maxEpollEvents, 500);
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
                        closeConnection(conn.fd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
                        continue;
                    }

                    if (conn.bytesSent >= conn.outBuffer.size()) {
                        if (conn.shouldClose) {
                            closeConnection(conn.fd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
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

                auto snapshots = snapshotQueue.popLatestPerRoom();
                vector<int> slowClientsToDisconnect;

                // Shared non-blocking snapshot delivery helper enforcing per-client backpressure,
                // direct zero-copy write for uncongested sockets, and remainder buffering for partial writes.
                auto deliverSnapshot = [&](int sockFd, Connection& playerConn, const string& frame) {
                    bool socketBlocked = (playerConn.bytesSent < playerConn.outBuffer.size());
                    bool bufferTooLarge = (playerConn.outBuffer.size() >= Connection::SNAPSHOT_BACKPRESSURE_THRESHOLD);

                    if (socketBlocked || bufferTooLarge) {
                        playerConn.consecutiveDroppedSnapshots++;
                        ServerMetrics::instance().backpressureSnapshotDropsTotal.fetch_add(1, std::memory_order_relaxed);
                        if (playerConn.consecutiveDroppedSnapshots >= Connection::MAX_CONSECUTIVE_DROPPED_SNAPSHOTS) {
                            ServerMetrics::instance().backpressureSlowClientDisconnectsTotal.fetch_add(1, std::memory_order_relaxed);
                            slowClientsToDisconnect.push_back(sockFd);
                        }
                        return;
                    }

                    playerConn.consecutiveDroppedSnapshots = 0;
                    ServerMetrics::instance().snapshotFanoutDeliveriesTotal.fetch_add(1, std::memory_order_relaxed);

                    // Fast path: direct zero-copy socket transmission if output buffer is clear
                    if (playerConn.outBuffer.empty()) {
                        ssize_t sent = send(playerConn.fd, frame.data(), frame.size(), MSG_NOSIGNAL);
                        if (sent > 0) {
                            ServerMetrics::instance().recordBytesTransmitted(sent);
                            ServerMetrics::instance().wsFramesSentTotal.fetch_add(1, std::memory_order_relaxed);
                            if (playerConn.snapshotFormat == Connection::SnapshotFormat::BINARY) {
                                ServerMetrics::instance().snapshotBinaryFramesSentTotal.fetch_add(1, std::memory_order_relaxed);
                            } else {
                                ServerMetrics::instance().snapshotJsonFramesSentTotal.fetch_add(1, std::memory_order_relaxed);
                            }

                            if (static_cast<size_t>(sent) == frame.size()) {
                                ServerMetrics::instance().snapshotDirectSendsTotal.fetch_add(1, std::memory_order_relaxed);
                                ServerMetrics::instance().recordOutputBufferSize(0);
                                return;
                            } else {
                                ServerMetrics::instance().recordPartialWrite();
                                ServerMetrics::instance().snapshotBufferedSendsTotal.fetch_add(1, std::memory_order_relaxed);
                                playerConn.outBuffer.assign(frame.data() + sent, frame.size() - sent);
                                playerConn.bytesSent = 0;
                                ServerMetrics::instance().recordOutputBufferSize(playerConn.outBuffer.size());
                                epoll_event ev{};
                                ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                                ev.data.fd = sockFd;
                                epoll_ctl(epollFd, EPOLL_CTL_MOD, sockFd, &ev);
                                return;
                            }
                        } else if (sent < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                ServerMetrics::instance().recordEagain();
                                ServerMetrics::instance().snapshotBufferedSendsTotal.fetch_add(1, std::memory_order_relaxed);
                                playerConn.outBuffer.assign(frame.data(), frame.size());
                                playerConn.bytesSent = 0;
                                ServerMetrics::instance().recordOutputBufferSize(playerConn.outBuffer.size());
                                epoll_event ev{};
                                ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                                ev.data.fd = sockFd;
                                epoll_ctl(epollFd, EPOLL_CTL_MOD, sockFd, &ev);
                                return;
                            }
                            if (errno != EINTR) {
                                slowClientsToDisconnect.push_back(sockFd);
                                return;
                            }
                        }
                    }

                    // Fallback path: buffer frame and flush
                    playerConn.outBuffer.append(frame);
                    ServerMetrics::instance().snapshotBufferedSendsTotal.fetch_add(1, std::memory_order_relaxed);
                    ServerMetrics::instance().wsFramesSentTotal.fetch_add(1, std::memory_order_relaxed);
                    if (playerConn.snapshotFormat == Connection::SnapshotFormat::BINARY) {
                        ServerMetrics::instance().snapshotBinaryFramesSentTotal.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        ServerMetrics::instance().snapshotJsonFramesSentTotal.fetch_add(1, std::memory_order_relaxed);
                    }

                    bool sockErr = flushSendBuffer(playerConn);
                    if (sockErr) {
                        slowClientsToDisconnect.push_back(sockFd);
                        return;
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
                };

                for (const auto& snap : snapshots) {
                    for (const auto& p : snap.players) {
                        if (p.ackSeq > 0) {
                            ServerMetrics::instance().backpressureInputAcksTotal.fetch_add(1, std::memory_order_relaxed);
                        }
                    }

                    // Lazy immutable snapshot serializer: generates frame at most once per room per format
                    string jsonFrame;
                    string binaryFrame;
                    bool jsonSerialized = false;
                    bool binarySerialized = false;

                    auto getFrame = [&](Connection::SnapshotFormat fmt) -> const string& {
                        if (fmt == Connection::SnapshotFormat::BINARY) {
                            if (!binarySerialized) {
                                auto t0 = std::chrono::steady_clock::now();
                                string binPayload = BinarySnapshot::serialize(snap);
                                ServerMetrics::instance().snapshotBinaryPayloadBytesTotal.fetch_add(binPayload.size(), std::memory_order_relaxed);
                                binaryFrame = WebSocketFrame::encodeBinaryFrame(binPayload);
                                auto t1 = std::chrono::steady_clock::now();
                                uint64_t micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
                                ServerMetrics::instance().snapshotSerializationMicrosTotal.fetch_add(micros, std::memory_order_relaxed);
                                binarySerialized = true;
                            }
                            return binaryFrame;
                        } else {
                            if (!jsonSerialized) {
                                auto t0 = std::chrono::steady_clock::now();
                                string jsonStr = snap.toJson();
                                ServerMetrics::instance().snapshotJsonPayloadBytesTotal.fetch_add(jsonStr.size(), std::memory_order_relaxed);
                                jsonFrame = WebSocketFrame::encodeTextFrame(jsonStr);
                                auto t1 = std::chrono::steady_clock::now();
                                uint64_t micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
                                ServerMetrics::instance().snapshotSerializationMicrosTotal.fetch_add(micros, std::memory_order_relaxed);
                                jsonSerialized = true;
                            }
                            return jsonFrame;
                        }
                    };

                    if (snap.roomId == 0) {
                        // Practice mode fan-out: isolate strictly to practice players
                        for (const auto& [pid, sockFd] : playerToSocket) {
                            if (roomManager.getPlayerRoomId(pid) == 0) {
                                auto cIt = connections.find(sockFd);
                                if (cIt != connections.end() && cIt->second.protocol == ConnectionProtocol::WEBSOCKET) {
                                    deliverSnapshot(sockFd, cIt->second, getFrame(cIt->second.snapshotFormat));
                                }
                            }
                        }
                    } else {
                        // Room mode fan-out: isolate strictly to members of this room
                        Room* room = roomManager.getRoom(snap.roomId);
                        if (!room) continue;

                        if (room->state == RoomState::COUNTDOWN && snap.roomState == RoomState::RACING) {
                            room->state = RoomState::RACING;
                            string startedMsg = WebSocketFrame::encodeTextFrame(RoomProtocol::makeRaceStarted(*room));
                            for (const auto& [pid, m] : room->members) {
                                auto cIt = connections.find(m.socketFd);
                                if (cIt != connections.end() && cIt->second.protocol == ConnectionProtocol::WEBSOCKET) {
                                    sendWebSocketFrameSafe(cIt->second, epollFd, startedMsg);
                                }
                            }
                        } else if (room->state == RoomState::RACING && snap.roomState == RoomState::FINISHED) {
                            room->state = RoomState::FINISHED;
                            room->results = snap.results;
                            ServerMetrics::instance().multiplayerRaceCompletionsTotal.fetch_add(1, std::memory_order_relaxed);
                            string finishedMsg = WebSocketFrame::encodeTextFrame(RoomProtocol::makeRaceFinished(*room));
                            for (const auto& [pid, m] : room->members) {
                                auto cIt = connections.find(m.socketFd);
                                if (cIt != connections.end() && cIt->second.protocol == ConnectionProtocol::WEBSOCKET) {
                                    sendWebSocketFrameSafe(cIt->second, epollFd, finishedMsg);
                                }
                            }
                        }

                        for (const auto& [pid, member] : room->members) {
                            int sockFd = member.socketFd;
                            auto cIt = connections.find(sockFd);
                            if (cIt != connections.end() && cIt->second.protocol == ConnectionProtocol::WEBSOCKET) {
                                deliverSnapshot(sockFd, cIt->second, getFrame(cIt->second.snapshotFormat));
                            }
                        }
                    }
                }

                for (int sfd : slowClientsToDisconnect) {
                    closeConnection(sfd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
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
                    ServerMetrics::instance().recordTcpAccept();

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
                        closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
                        continue;
                    }
                }

                // Handle writable event (EPOLLOUT) for pending outgoing data
                if (currentEvents & EPOLLOUT) {
                    if (conn.bytesSent < conn.outBuffer.size()) {
                        bool sockErr = flushSendBuffer(conn);
                        if (sockErr) {
                            closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
                            continue;
                        }

                        if (conn.bytesSent >= conn.outBuffer.size()) {
                            conn.outBuffer.clear();
                            conn.bytesSent = 0;

                            if (conn.shouldClose) {
                                closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
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
                            ServerMetrics::instance().recordBytesReceived(bytes);
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
                                ServerMetrics::instance().recordEagain();
                                break;
                            }
                            if (errno == EINTR) {
                                continue;
                            }
                            ServerMetrics::instance().recordConnectionDrop();
                            disconnected = true;
                            break;
                        }
                    }

                    if (disconnected) {
                        closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
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
                                    if (rawRequest.find("format=binary") != string::npos ||
                                        rawRequest.find("format=bin") != string::npos ||
                                        rawRequest.find("racing-binary") != string::npos) {
                                        conn.snapshotFormat = Connection::SnapshotFormat::BINARY;
                                        if (rawRequest.find("racing-binary") != string::npos) {
                                            size_t endHeader = responseText.rfind("\r\n\r\n");
                                            if (endHeader != string::npos) {
                                                responseText.insert(endHeader + 2, "Sec-WebSocket-Protocol: racing-binary-v1\r\n");
                                            }
                                        }
                                    }
                                    ServerMetrics::instance().recordWebSocketUpgrade();
                                    int sndbuf = 8192; // Bounded socket send buffer for responsive backpressure
                                    setsockopt(conn.fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
                                    conn.shouldClose = false;
                                    conn.outBuffer = std::move(responseText);
                                    conn.bytesSent = 0;
                                    conn.state = ConnectionState::WRITING_RESPONSE;

                                    bool sockErr = flushSendBuffer(conn);
                                    if (sockErr) {
                                        closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
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
                                        closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
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
                            closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
                            continue;
                        } else if (parseResult == Connection::ParseResult::BAD_REQUEST) {
                            sendImmediateError(clientSocketFd, 400, "Bad Request");
                            closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
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
                                ServerMetrics::instance().wsProtocolErrorsTotal.fetch_add(1, std::memory_order_relaxed);
                                string closeFrame = WebSocketFrame::encodeCloseFrame(closeCode, "Protocol Error");
                                conn.outBuffer.append(closeFrame);
                                conn.wsCloseSent = true;
                                conn.shouldClose = true;
                                flushSendBuffer(conn);

                                if (conn.bytesSent >= conn.outBuffer.size()) {
                                    closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
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
                            ServerMetrics::instance().wsFramesReceivedTotal.fetch_add(1, std::memory_order_relaxed);

                            // Control Frames
                            if (frame.opcode == WebSocketFrame::OPCODE_PING) {
                                string pong = WebSocketFrame::encodePongFrame(frame.payload);
                                if (!sendWebSocketFrameSafe(conn, epollFd, pong, Connection::MAX_CONGESTED_CONTROL_BUFFER)) {
                                    closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
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
                                    closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
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
                                        closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
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
                                            closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
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
                                    handleWebSocketTextMessage(conn, epollFd, frame.payload, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, nextPlayerId, inputQueue, roomManager);
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
                                        closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
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
                                        closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
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
                                                closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
                                            } else {
                                                epoll_event ev{};
                                                ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                                                ev.data.fd = clientSocketFd;
                                                epoll_ctl(epollFd, EPOLL_CTL_MOD, clientSocketFd, &ev);
                                            }
                                            connectionClosed = true;
                                            break;
                                        }

                                        handleWebSocketTextMessage(conn, epollFd, conn.wsMessageBuffer, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, nextPlayerId, inputQueue, roomManager);
                                    } else if (conn.wsFragmentedOpcode == WebSocketFrame::OPCODE_BINARY) {
                                        string echoFrame = WebSocketFrame::encodeFrame(WebSocketFrame::OPCODE_BINARY, conn.wsMessageBuffer, true);
                                        if (!sendWebSocketFrameSafe(conn, epollFd, echoFrame)) {
                                            closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
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
                                        closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
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
                                        closeConnection(clientSocketFd, epollFd, connections, socketToPlayer, playerToSocket, playerLastSeq, playerLastTime, inputQueue, roomManager);
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
    auto shutdownStartTime = std::chrono::steady_clock::now();
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

    auto shutdownEndTime = std::chrono::steady_clock::now();
    double shutdownDuration = std::chrono::duration<double>(shutdownEndTime - shutdownStartTime).count();
    ServerMetrics::instance().serverShutdownDurationSeconds.store(shutdownDuration, std::memory_order_relaxed);

    Logger::log("Server shut down cleanly.");
    return 0;
}
