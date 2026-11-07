#pragma once

#include <string>
#include <unordered_map>
#include <cstdint>

class WebSocketHandshake {
public:
    enum class HandshakeResult {
        SUCCESS,
        NOT_UPGRADE_REQUEST,
        BAD_REQUEST,
        UPGRADE_REQUIRED,
        METHOD_NOT_ALLOWED,
        NOT_FOUND
    };

    // Quick check if the raw HTTP request indicates a WebSocket upgrade
    static bool isUpgradeRequest(const std::string& rawRequest);

    // Validates that Sec-WebSocket-Key is 24 base64 chars ending in '==' and decodes to exactly 16 bytes
    static bool isValidKey(const std::string& key);

    // Validates the handshake request for endpoint /ws (or custom path).
    // If successful, populates responseText with the HTTP 101 response and returns SUCCESS.
    // If invalid, populates responseText with an appropriate HTTP error response.
    static HandshakeResult validateHandshake(
        const std::string& rawRequest,
        std::string& responseText,
        std::string& clientKey
    );

    // Computes Sec-WebSocket-Accept according to RFC 6455
    static std::string computeAcceptKey(const std::string& clientKey);

private:
    static std::string trim(const std::string& str);
    static std::string toLower(const std::string& str);
    static bool tokenListContains(const std::string& headerVal, const std::string& token);
};
