#include "Connection.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>

Connection::Connection(int socketFd, uint64_t connId)
    : fd(socketFd), id(connId), state(ConnectionState::READING_REQUEST),
      bytesSent(0), shouldClose(true) {}

Connection::ParseResult Connection::checkRequest(size_t& requestLen) const {
    // 1. Look for standard header terminator \r\n\r\n (or \n\n fallback)
    size_t headerEnd = inBuffer.find("\r\n\r\n");
    size_t delimLen = 4;
    if (headerEnd == std::string::npos) {
        headerEnd = inBuffer.find("\n\n");
        delimLen = 2;
    }

    if (headerEnd == std::string::npos) {
        if (inBuffer.size() > MAX_HEADER_SIZE) {
            return ParseResult::TOO_LARGE;
        }
        return ParseResult::INCOMPLETE;
    }

    size_t headerTotalBytes = headerEnd + delimLen;
    if (headerTotalBytes > MAX_HEADER_SIZE) {
        return ParseResult::TOO_LARGE;
    }

    // 2. Search for Content-Length (case-insensitive) in the header section
    std::string headerSection = inBuffer.substr(0, headerTotalBytes);
    std::string lowerHeaders = headerSection;
    std::transform(lowerHeaders.begin(), lowerHeaders.end(), lowerHeaders.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    const std::string clKey = "content-length:";
    size_t clPos = lowerHeaders.find(clKey);

    size_t contentLength = 0;
    if (clPos != std::string::npos) {
        size_t valStart = clPos + clKey.length();
        while (valStart < headerTotalBytes && (lowerHeaders[valStart] == ' ' || lowerHeaders[valStart] == '\t')) {
            valStart++;
        }
        size_t valEnd = valStart;
        while (valEnd < headerTotalBytes && std::isdigit(static_cast<unsigned char>(lowerHeaders[valEnd]))) {
            valEnd++;
        }
        if (valStart == valEnd) {
            return ParseResult::BAD_REQUEST;
        }

        char* endPtr = nullptr;
        unsigned long long parsed = std::strtoull(&lowerHeaders[valStart], &endPtr, 10);
        if (parsed > MAX_BODY_SIZE) {
            return ParseResult::TOO_LARGE;
        }
        contentLength = static_cast<size_t>(parsed);
    }

    size_t totalExpected = headerTotalBytes + contentLength;
    if (inBuffer.size() < totalExpected) {
        return ParseResult::INCOMPLETE;
    }

    requestLen = totalExpected;
    return ParseResult::COMPLETE;
}

void Connection::resetForNextRequest() {
    state = ConnectionState::READING_REQUEST;
    outBuffer.clear();
    bytesSent = 0;
    shouldClose = true;
}
