#pragma once

#include <string>
#include <cstdint>
#include <cstddef>
#include <vector>

class WebSocketFrame {
public:
    static constexpr uint8_t OPCODE_CONTINUATION = 0x0;
    static constexpr uint8_t OPCODE_TEXT         = 0x1;
    static constexpr uint8_t OPCODE_BINARY       = 0x2;
    static constexpr uint8_t OPCODE_CLOSE        = 0x8;
    static constexpr uint8_t OPCODE_PING         = 0x9;
    static constexpr uint8_t OPCODE_PONG         = 0xA;

    // RFC 6455 Close Status Codes
    static constexpr uint16_t CLOSE_NORMAL            = 1000;
    static constexpr uint16_t CLOSE_GOING_AWAY        = 1001;
    static constexpr uint16_t CLOSE_PROTOCOL_ERROR    = 1002;
    static constexpr uint16_t CLOSE_UNSUPPORTED_DATA  = 1003;
    static constexpr uint16_t CLOSE_INVALID_PAYLOAD   = 1007;
    static constexpr uint16_t CLOSE_POLICY_VIOLATION  = 1008;
    static constexpr uint16_t CLOSE_MESSAGE_TOO_BIG   = 1009;

    static constexpr size_t MAX_FRAME_SIZE = 10 * 1024 * 1024; // 10 MB payload limit

    enum class ParseStatus {
        NEED_MORE_DATA,
        FRAME_COMPLETE,
        PROTOCOL_ERROR
    };

    struct Frame {
        bool fin = false;
        bool rsv1 = false;
        bool rsv2 = false;
        bool rsv3 = false;
        uint8_t opcode = 0;
        bool masked = false;
        uint8_t maskingKey[4] = {0};
        uint64_t payloadLength = 0;
        std::string payload;
        uint16_t closeCode = 0;
        std::string closeReason;
    };

    // Incremental frame parser. Examines inBuffer starting at index 0.
    // If complete frame is available: returns FRAME_COMPLETE, fills outFrame, sets bytesConsumed.
    // If buffer holds incomplete frame: returns NEED_MORE_DATA, bytesConsumed = 0.
    // If frame violates RFC 6455: returns PROTOCOL_ERROR, sets closeCode.
    static ParseStatus parseFrame(
        const std::string& inBuffer,
        Frame& outFrame,
        size_t& bytesConsumed,
        uint16_t& closeCode
    );

    // Encodes a server-to-client WebSocket frame (unmasked)
    static std::string encodeFrame(uint8_t opcode, const std::string& payload, bool fin = true);

    // Convenience encoders
    static std::string encodeTextFrame(const std::string& text, bool fin = true);
    static std::string encodePongFrame(const std::string& payload);
    static std::string encodeCloseFrame(uint16_t statusCode = CLOSE_NORMAL, const std::string& reason = "");

    // Validates that string complies with UTF-8 encoding (RFC 3629)
    static bool isValidUtf8(const std::string& str);
};
