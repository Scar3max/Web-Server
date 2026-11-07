#include "WebSocketFrame.h"
#include <cstring>

WebSocketFrame::ParseStatus WebSocketFrame::parseFrame(
    const std::string& inBuffer,
    Frame& outFrame,
    size_t& bytesConsumed,
    uint16_t& closeCode
) {
    bytesConsumed = 0;
    closeCode = CLOSE_NORMAL;

    if (inBuffer.size() < 2) {
        return ParseStatus::NEED_MORE_DATA;
    }

    const uint8_t* data = reinterpret_cast<const uint8_t*>(inBuffer.data());
    uint8_t b0 = data[0];
    uint8_t b1 = data[1];

    outFrame.fin  = (b0 & 0x80) != 0;
    outFrame.rsv1 = (b0 & 0x40) != 0;
    outFrame.rsv2 = (b0 & 0x20) != 0;
    outFrame.rsv3 = (b0 & 0x10) != 0;
    outFrame.opcode = b0 & 0x0F;

    // RFC 6455 5.2: RSV bits MUST be 0 unless extension is negotiated
    if (outFrame.rsv1 || outFrame.rsv2 || outFrame.rsv3) {
        closeCode = CLOSE_PROTOCOL_ERROR;
        return ParseStatus::PROTOCOL_ERROR;
    }

    // Validate opcode
    bool isControl = (outFrame.opcode & 0x08) != 0;
    switch (outFrame.opcode) {
        case OPCODE_CONTINUATION:
        case OPCODE_TEXT:
        case OPCODE_BINARY:
        case OPCODE_CLOSE:
        case OPCODE_PING:
        case OPCODE_PONG:
            break;
        default:
            closeCode = CLOSE_PROTOCOL_ERROR;
            return ParseStatus::PROTOCOL_ERROR;
    }

    // Control frame constraint: cannot be fragmented
    if (isControl && !outFrame.fin) {
        closeCode = CLOSE_PROTOCOL_ERROR;
        return ParseStatus::PROTOCOL_ERROR;
    }

    outFrame.masked = (b1 & 0x80) != 0;
    // RFC 6455 5.1: Client-to-server frames MUST be masked
    if (!outFrame.masked) {
        closeCode = CLOSE_PROTOCOL_ERROR;
        return ParseStatus::PROTOCOL_ERROR;
    }

    uint8_t len7 = b1 & 0x7F;
    // Control frame constraint: payload length <= 125
    if (isControl && len7 > 125) {
        closeCode = CLOSE_PROTOCOL_ERROR;
        return ParseStatus::PROTOCOL_ERROR;
    }

    size_t extLenBytes = 0;
    if (len7 == 126) {
        extLenBytes = 2;
    } else if (len7 == 127) {
        extLenBytes = 8;
    }

    size_t maskKeyBytes = outFrame.masked ? 4 : 0;
    size_t headerBytes = 2 + extLenBytes + maskKeyBytes;

    if (inBuffer.size() < headerBytes) {
        return ParseStatus::NEED_MORE_DATA;
    }

    uint64_t payloadLen = 0;
    if (len7 <= 125) {
        payloadLen = len7;
    } else if (len7 == 126) {
        payloadLen = (static_cast<uint64_t>(data[2]) << 8) | static_cast<uint64_t>(data[3]);
        // RFC 6455: payload length must not use non-minimal encoding
        if (payloadLen < 126) {
            closeCode = CLOSE_PROTOCOL_ERROR;
            return ParseStatus::PROTOCOL_ERROR;
        }
    } else { // 127
        // Most significant bit must be 0
        if ((data[2] & 0x80) != 0) {
            closeCode = CLOSE_PROTOCOL_ERROR;
            return ParseStatus::PROTOCOL_ERROR;
        }
        for (int i = 0; i < 8; i++) {
            payloadLen = (payloadLen << 8) | static_cast<uint64_t>(data[2 + i]);
        }
        if (payloadLen < 65536) {
            closeCode = CLOSE_PROTOCOL_ERROR;
            return ParseStatus::PROTOCOL_ERROR;
        }
    }

    if (payloadLen > MAX_FRAME_SIZE) {
        closeCode = CLOSE_MESSAGE_TOO_BIG;
        return ParseStatus::PROTOCOL_ERROR;
    }

    outFrame.payloadLength = payloadLen;

    if (inBuffer.size() < headerBytes + payloadLen) {
        return ParseStatus::NEED_MORE_DATA;
    }

    // Extract mask key
    size_t maskOffset = 2 + extLenBytes;
    std::memcpy(outFrame.maskingKey, data + maskOffset, 4);

    // Extract and unmask payload
    const uint8_t* payloadData = data + headerBytes;
    outFrame.payload.resize(payloadLen);
    for (size_t i = 0; i < payloadLen; i++) {
        outFrame.payload[i] = static_cast<char>(payloadData[i] ^ outFrame.maskingKey[i % 4]);
    }

    // Close frame payload validation
    if (outFrame.opcode == OPCODE_CLOSE) {
        if (payloadLen == 1) {
            closeCode = CLOSE_PROTOCOL_ERROR;
            return ParseStatus::PROTOCOL_ERROR;
        }
        if (payloadLen >= 2) {
            uint16_t code = (static_cast<uint8_t>(outFrame.payload[0]) << 8) |
                             static_cast<uint8_t>(outFrame.payload[1]);
            // Validate code values
            if (code < 1000 || code == 1004 || code == 1005 || code == 1006 ||
                (code >= 1015 && code <= 2999)) {
                closeCode = CLOSE_PROTOCOL_ERROR;
                return ParseStatus::PROTOCOL_ERROR;
            }
            outFrame.closeCode = code;
            if (payloadLen > 2) {
                outFrame.closeReason = outFrame.payload.substr(2);
                if (!isValidUtf8(outFrame.closeReason)) {
                    closeCode = CLOSE_INVALID_PAYLOAD;
                    return ParseStatus::PROTOCOL_ERROR;
                }
            }
        } else {
            outFrame.closeCode = CLOSE_NORMAL;
        }
    }

    bytesConsumed = headerBytes + payloadLen;
    return ParseStatus::FRAME_COMPLETE;
}

std::string WebSocketFrame::encodeFrame(uint8_t opcode, const std::string& payload, bool fin) {
    std::string frame;
    uint8_t b0 = (fin ? 0x80 : 0x00) | (opcode & 0x0F);
    frame.push_back(static_cast<char>(b0));

    // Server-to-client frames MUST NOT be masked (mask bit = 0)
    size_t len = payload.size();
    if (len <= 125) {
        frame.push_back(static_cast<char>(len));
    } else if (len <= 65535) {
        frame.push_back(static_cast<char>(126));
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
    } else {
        frame.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; i--) {
            frame.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
        }
    }

    frame.append(payload);
    return frame;
}

std::string WebSocketFrame::encodeTextFrame(const std::string& text, bool fin) {
    return encodeFrame(OPCODE_TEXT, text, fin);
}

std::string WebSocketFrame::encodePongFrame(const std::string& payload) {
    return encodeFrame(OPCODE_PONG, payload, true);
}

std::string WebSocketFrame::encodeCloseFrame(uint16_t statusCode, const std::string& reason) {
    std::string payload;
    payload.push_back(static_cast<char>((statusCode >> 8) & 0xFF));
    payload.push_back(static_cast<char>(statusCode & 0xFF));
    payload.append(reason);
    return encodeFrame(OPCODE_CLOSE, payload, true);
}

bool WebSocketFrame::isValidUtf8(const std::string& str) {
    size_t i = 0;
    size_t len = str.size();

    while (i < len) {
        uint8_t c = static_cast<uint8_t>(str[i]);
        if (c <= 0x7F) {
            i += 1;
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (i + 1 >= len) return false;
            uint8_t c1 = static_cast<uint8_t>(str[i + 1]);
            if ((c1 & 0xC0) != 0x80) return false;
            i += 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            if (i + 2 >= len) return false;
            uint8_t c1 = static_cast<uint8_t>(str[i + 1]);
            uint8_t c2 = static_cast<uint8_t>(str[i + 2]);
            if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) return false;
            if (c == 0xE0 && c1 < 0xA0) return false; // overlong
            if (c == 0xED && c1 >= 0xA0) return false; // surrogate 0xD800-0xDFFF
            i += 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            if (i + 3 >= len) return false;
            uint8_t c1 = static_cast<uint8_t>(str[i + 1]);
            uint8_t c2 = static_cast<uint8_t>(str[i + 2]);
            uint8_t c3 = static_cast<uint8_t>(str[i + 3]);
            if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) return false;
            if (c == 0xF0 && c1 < 0x90) return false; // overlong
            if (c == 0xF4 && c1 > 0x8F) return false; // > 0x10FFFF
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}
