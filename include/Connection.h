#pragma once

#include <string>
#include <cstdint>
#include <cstddef>

enum class ConnectionState {
    READING_REQUEST,
    PROCESSING,
    WRITING_RESPONSE,
    CLOSING,
    CLOSED
};

enum class ConnectionProtocol {
    HTTP,
    WEBSOCKET
};

class Connection {
public:
    int fd = -1;
    uint64_t id = 0;
    ConnectionState state = ConnectionState::READING_REQUEST;
    ConnectionProtocol protocol = ConnectionProtocol::HTTP;

    std::string inBuffer;
    std::string outBuffer;
    size_t bytesSent = 0;
    bool shouldClose = true;

    // WebSocket state tracking
    bool wsCloseSent = false;
    bool wsCloseReceived = false;
    bool wsInFragmentedMessage = false;
    uint8_t wsFragmentedOpcode = 0;
    std::string wsMessageBuffer;

    // Buffer & Backpressure limits
    static constexpr size_t MAX_HEADER_SIZE      = 8192;             // 8 KB
    static constexpr size_t MAX_BODY_SIZE        = 10 * 1024 * 1024; // 10 MB
    static constexpr size_t MAX_BUFFER_SIZE      = MAX_HEADER_SIZE + MAX_BODY_SIZE;
    static constexpr size_t MAX_OUT_BUFFER_SIZE  = 64 * 1024;        // 64 KB max queued output
    static constexpr size_t MAX_WS_MESSAGE_SIZE  = 10 * 1024 * 1024; // 10 MB max assembled WS message
    static constexpr size_t MAX_CONSECUTIVE_DROPPED_SNAPSHOTS = 50;  // 2.5 seconds of stalled writes

    size_t consecutiveDroppedSnapshots = 0;

    enum class ParseResult {
        INCOMPLETE,
        COMPLETE,
        TOO_LARGE,
        BAD_REQUEST
    };

    Connection() = default;
    Connection(int socketFd, uint64_t connId);

    // Checks if inBuffer currently contains a complete HTTP request.
    // If complete, sets requestLen to the byte length of that request.
    ParseResult checkRequest(size_t& requestLen) const;

    // Resets buffers and state for the next request on a persistent connection
    void resetForNextRequest();
};
