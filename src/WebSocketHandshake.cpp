#include "WebSocketHandshake.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace {

const char* WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// Standard RFC 3174 SHA-1 implementation
uint32_t sha1_rol(uint32_t value, size_t bits) {
    return (value << bits) | (value >> (32 - bits));
}

void sha1_transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    uint32_t w[80];

    for (size_t i = 0; i < 16; i++) {
        w[i] = (static_cast<uint32_t>(buffer[i * 4]) << 24)
             | (static_cast<uint32_t>(buffer[i * 4 + 1]) << 16)
             | (static_cast<uint32_t>(buffer[i * 4 + 2]) << 8)
             | (static_cast<uint32_t>(buffer[i * 4 + 3]));
    }
    for (size_t i = 16; i < 80; i++) {
        w[i] = sha1_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    for (size_t i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t temp = sha1_rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = sha1_rol(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

std::vector<uint8_t> sha1_digest(const std::string& input) {
    uint32_t state[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint64_t totalBits = input.size() * 8;

    size_t offset = 0;
    while (offset + 64 <= input.size()) {
        sha1_transform(state, reinterpret_cast<const uint8_t*>(&input[offset]));
        offset += 64;
    }

    uint8_t finalBlock[64] = {0};
    size_t remaining = input.size() - offset;
    std::memcpy(finalBlock, input.data() + offset, remaining);
    finalBlock[remaining] = 0x80;

    if (remaining >= 56) {
        sha1_transform(state, finalBlock);
        std::memset(finalBlock, 0, 64);
    }

    for (int i = 0; i < 8; i++) {
        finalBlock[63 - i] = static_cast<uint8_t>(totalBits >> (i * 8));
    }
    sha1_transform(state, finalBlock);

    std::vector<uint8_t> digest(20);
    for (int i = 0; i < 5; i++) {
        digest[i * 4]     = static_cast<uint8_t>(state[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(state[i]);
    }
    return digest;
}

// Standard RFC 4648 Base64 encoder
std::string base64_encode(const uint8_t* data, size_t len) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3) {
        uint32_t b0 = data[i];
        uint32_t b1 = (i + 1 < len) ? data[i + 1] : 0;
        uint32_t b2 = (i + 2 < len) ? data[i + 2] : 0;
        uint32_t triple = (b0 << 16) | (b1 << 8) | b2;

        out.push_back(tbl[(triple >> 18) & 0x3F]);
        out.push_back(tbl[(triple >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? tbl[(triple >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? tbl[triple & 0x3F] : '=');
    }
    return out;
}

} // namespace

std::string WebSocketHandshake::trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

std::string WebSocketHandshake::toLower(const std::string& str) {
    std::string res = str;
    std::transform(res.begin(), res.end(), res.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return res;
}

bool WebSocketHandshake::tokenListContains(const std::string& headerVal, const std::string& token) {
    std::string lowerVal = toLower(headerVal);
    std::string lowerToken = toLower(token);

    std::stringstream ss(lowerVal);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (trim(item) == lowerToken) {
            return true;
        }
    }
    return false;
}

bool WebSocketHandshake::isUpgradeRequest(const std::string& rawRequest) {
    std::string lower = toLower(rawRequest.substr(0, std::min<size_t>(rawRequest.size(), 4096)));
    return (lower.find("upgrade:") != std::string::npos || lower.find("upgrade :") != std::string::npos) &&
           (lower.find("websocket") != std::string::npos);
}

bool WebSocketHandshake::isValidKey(const std::string& key) {
    // RFC 6455 4.1: Sec-WebSocket-Key must be 16-byte nonce base64-encoded (24 chars ending in '==')
    if (key.size() != 24) return false;
    if (key[22] != '=' || key[23] != '=') return false;

    static const std::string b64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < 22; i++) {
        if (b64chars.find(key[i]) == std::string::npos) {
            return false;
        }
    }

    // Index 21 encodes the final 2 bits of the 16th byte, so its lower 4 bits must be zero.
    // In Base64, this restricts character 21 to: 'A' (0), 'Q' (16), 'g' (32), 'w' (48).
    char c = key[21];
    if (c != 'A' && c != 'Q' && c != 'g' && c != 'w') {
        return false;
    }

    return true;
}

std::string WebSocketHandshake::computeAcceptKey(const std::string& clientKey) {
    std::string combined = clientKey + WEBSOCKET_GUID;
    std::vector<uint8_t> digest = sha1_digest(combined);
    return base64_encode(digest.data(), digest.size());
}

WebSocketHandshake::HandshakeResult WebSocketHandshake::validateHandshake(
    const std::string& rawRequest,
    std::string& responseText,
    std::string& clientKey
) {
    std::istringstream stream(rawRequest);
    std::string requestLine;
    if (!std::getline(stream, requestLine)) {
        responseText = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
        return HandshakeResult::BAD_REQUEST;
    }

    if (!requestLine.empty() && requestLine.back() == '\r') {
        requestLine.pop_back();
    }

    std::istringstream lineStream(requestLine);
    std::string method, path, version;
    lineStream >> method >> path >> version;

    if (method != "GET") {
        responseText = "HTTP/1.1 405 Method Not Allowed\r\nAllow: GET\r\nConnection: close\r\n\r\n";
        return HandshakeResult::METHOD_NOT_ALLOWED;
    }

    // RFC 6455 requires HTTP/1.1
    if (version != "HTTP/1.1") {
        responseText = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
        return HandshakeResult::BAD_REQUEST;
    }

    // Strip query string for path check
    size_t qPos = path.find('?');
    std::string basePath = (qPos != std::string::npos) ? path.substr(0, qPos) : path;
    if (basePath != "/ws") {
        responseText = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
        return HandshakeResult::NOT_FOUND;
    }

    // Parse headers (case-insensitive keys; merge duplicate headers with comma per RFC 7230)
    std::unordered_map<std::string, std::string> headers;
    std::string headerLine;
    while (std::getline(stream, headerLine) && headerLine != "\r" && !headerLine.empty()) {
        if (headerLine.back() == '\r') {
            headerLine.pop_back();
        }
        size_t colon = headerLine.find(':');
        if (colon != std::string::npos) {
            std::string name = toLower(trim(headerLine.substr(0, colon)));
            std::string value = trim(headerLine.substr(colon + 1));
            if (headers.find(name) != headers.end()) {
                headers[name] += ", " + value;
            } else {
                headers[name] = value;
            }
        }
    }

    // Validate Upgrade: websocket
    auto upIt = headers.find("upgrade");
    if (upIt == headers.end() || toLower(upIt->second) != "websocket") {
        responseText = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nMissing or invalid Upgrade header";
        return HandshakeResult::BAD_REQUEST;
    }

    // Validate Connection: Upgrade token (handles comma-separated tokens)
    auto connIt = headers.find("connection");
    if (connIt == headers.end() || !tokenListContains(connIt->second, "upgrade")) {
        responseText = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nMissing or invalid Connection header";
        return HandshakeResult::BAD_REQUEST;
    }

    // Validate Sec-WebSocket-Version: 13
    auto verIt = headers.find("sec-websocket-version");
    if (verIt == headers.end() || verIt->second != "13") {
        responseText = "HTTP/1.1 426 Upgrade Required\r\n"
                       "Sec-WebSocket-Version: 13\r\n"
                       "Connection: close\r\n\r\n"
                       "Unsupported WebSocket Version";
        return HandshakeResult::UPGRADE_REQUIRED;
    }

    // Validate Sec-WebSocket-Key
    auto keyIt = headers.find("sec-websocket-key");
    if (keyIt == headers.end() || !isValidKey(keyIt->second)) {
        responseText = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nInvalid Sec-WebSocket-Key";
        return HandshakeResult::BAD_REQUEST;
    }

    clientKey = keyIt->second;
    std::string acceptKey = computeAcceptKey(clientKey);

    responseText = "HTTP/1.1 101 Switching Protocols\r\n"
                   "Upgrade: websocket\r\n"
                   "Connection: Upgrade\r\n"
                   "Sec-WebSocket-Accept: " + acceptKey + "\r\n\r\n";

    return HandshakeResult::SUCCESS;
}
