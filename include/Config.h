#pragma once

#include <string>
#include <cstdint>
#include <cstddef>
#include "Logger.h"

struct ServerConfig {
    std::string host = "0.0.0.0";
    int port = 8080;
    std::string staticDir = "static";
    size_t threadPoolWorkers = 4;
    LogLevel logLevel = LogLevel::INFO;
    int listenBacklog = 128;
    int maxEpollEvents = 64;

    // Parses CLI flags and environment variables with strict validation.
    // Returns:
    //   0 on success
    //   1 if --help was requested (normal exit)
    //  -1 on error (errMessage populated)
    static int parse(int argc, char* argv[], ServerConfig& outConfig, std::string& errMessage);
    static void printUsage(const char* progName);
};
