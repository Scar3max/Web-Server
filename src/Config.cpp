#include "Config.h"
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <arpa/inet.h>
#include <vector>

using namespace std;

static bool isDirectory(const string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return false;
}

static string getExecutableDir() {
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        string p(buf);
        size_t slash = p.find_last_of('/');
        if (slash != string::npos) {
            return p.substr(0, slash);
        }
    }
    return "";
}

void ServerConfig::printUsage(const char* progName) {
    cout << "Multiplayer Racing Game HTTP/WebSocket Server\n"
         << "Usage: " << (progName ? progName : "server") << " [OPTIONS]\n\n"
         << "Options:\n"
         << "  -h, --host <ip>          Listen IP address (default: 0.0.0.0, env: HTTP_HOST)\n"
         << "  -p, --port <port>        Listen port 1-65535 (default: 8080, env: HTTP_PORT)\n"
         << "  -s, --static-dir <dir>   Path to static assets (default: static, env: HTTP_STATIC_DIR)\n"
         << "  -t, --threads <num>      Worker thread pool size 1-128 (default: 4, env: HTTP_WORKERS)\n"
         << "  -l, --log-level <level>  Log level: DEBUG, INFO, WARN, ERROR, NONE (default: INFO, env: HTTP_LOG_LEVEL)\n"
         << "      --help               Display this help message and exit\n\n";
}

int ServerConfig::parse(int argc, char* argv[], ServerConfig& outConfig, string& errMessage) {
    ServerConfig cfg;
    bool staticDirExplicit = false;

    // 1. Load from environment variables first
    const char* envHost = getenv("HTTP_HOST");
    if (envHost && *envHost) {
        string h(envHost);
        if (h != "0.0.0.0" && h != "127.0.0.1") {
            struct in_addr addr{};
            if (inet_pton(AF_INET, h.c_str(), &addr) <= 0) {
                errMessage = "Invalid HTTP_HOST environment variable: '" + h + "' (must be a valid IPv4 address)";
                return -1;
            }
        }
        cfg.host = h;
    }

    const char* envPort = getenv("HTTP_PORT");
    if (envPort && *envPort) {
        try {
            size_t idx = 0;
            string s(envPort);
            int p = stoi(s, &idx);
            if (idx != s.size() || p < 1 || p > 65535) {
                errMessage = "Invalid HTTP_PORT environment variable: '" + s + "' (must be between 1 and 65535)";
                return -1;
            }
            cfg.port = p;
        } catch (...) {
            errMessage = "Invalid HTTP_PORT environment variable: '" + string(envPort) + "'";
            return -1;
        }
    }

    const char* envStatic = getenv("HTTP_STATIC_DIR");
    if (envStatic && *envStatic) {
        cfg.staticDir = envStatic;
        staticDirExplicit = true;
    }

    const char* envWorkers = getenv("HTTP_WORKERS");
    if (envWorkers && *envWorkers) {
        try {
            size_t idx = 0;
            string s(envWorkers);
            int w = stoi(s, &idx);
            if (idx != s.size() || w < 1 || w > 128) {
                errMessage = "Invalid HTTP_WORKERS environment variable: '" + s + "' (must be between 1 and 128)";
                return -1;
            }
            cfg.threadPoolWorkers = static_cast<size_t>(w);
        } catch (...) {
            errMessage = "Invalid HTTP_WORKERS environment variable: '" + string(envWorkers) + "'";
            return -1;
        }
    }

    const char* envLogLevel = getenv("HTTP_LOG_LEVEL");
    if (envLogLevel && *envLogLevel) {
        try {
            cfg.logLevel = Logger::parseLevel(envLogLevel);
        } catch (const exception& e) {
            errMessage = "Invalid HTTP_LOG_LEVEL environment variable: '" + string(envLogLevel) + "'. " + e.what();
            return -1;
        }
    }

    // 2. Parse command line arguments (CLI flags override environment variables)
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];

        if (arg == "--help") {
            printUsage(argv[0]);
            return 1;
        } else if (arg == "-h" || arg == "--host") {
            if (i + 1 >= argc) {
                errMessage = "Missing argument for " + arg;
                return -1;
            }
            string h = argv[++i];
            if (h.empty()) {
                errMessage = "Host address cannot be empty";
                return -1;
            }
            if (h != "0.0.0.0" && h != "127.0.0.1") {
                struct in_addr addr{};
                if (inet_pton(AF_INET, h.c_str(), &addr) <= 0) {
                    errMessage = "Invalid host address: '" + h + "' (must be a valid IPv4 address)";
                    return -1;
                }
            }
            cfg.host = h;
        } else if (arg == "-p" || arg == "--port") {
            if (i + 1 >= argc) {
                errMessage = "Missing argument for " + arg;
                return -1;
            }
            string pStr = argv[++i];
            try {
                size_t idx = 0;
                int p = stoi(pStr, &idx);
                if (idx != pStr.size() || p < 1 || p > 65535) {
                    errMessage = "Invalid port number: '" + pStr + "' (must be between 1 and 65535)";
                    return -1;
                }
                cfg.port = p;
            } catch (...) {
                errMessage = "Invalid port number: '" + pStr + "'";
                return -1;
            }
        } else if (arg == "-s" || arg == "--static-dir") {
            if (i + 1 >= argc) {
                errMessage = "Missing argument for " + arg;
                return -1;
            }
            cfg.staticDir = argv[++i];
            staticDirExplicit = true;
        } else if (arg == "-t" || arg == "--threads") {
            if (i + 1 >= argc) {
                errMessage = "Missing argument for " + arg;
                return -1;
            }
            string tStr = argv[++i];
            try {
                size_t idx = 0;
                int t = stoi(tStr, &idx);
                if (idx != tStr.size() || t < 1 || t > 128) {
                    errMessage = "Invalid worker thread count: '" + tStr + "' (must be between 1 and 128)";
                    return -1;
                }
                cfg.threadPoolWorkers = static_cast<size_t>(t);
            } catch (...) {
                errMessage = "Invalid worker thread count: '" + tStr + "'";
                return -1;
            }
        } else if (arg == "-l" || arg == "--log-level") {
            if (i + 1 >= argc) {
                errMessage = "Missing argument for " + arg;
                return -1;
            }
            string lStr = argv[++i];
            try {
                cfg.logLevel = Logger::parseLevel(lStr);
            } catch (const exception& e) {
                errMessage = "Invalid log level '" + lStr + "'. " + e.what();
                return -1;
            }
        } else {
            errMessage = "Unknown command line option: '" + arg + "'";
            return -1;
        }
    }

    // 3. Validate and resolve staticDir
    if (staticDirExplicit) {
        if (!isDirectory(cfg.staticDir)) {
            errMessage = "Configured static directory does not exist or is not a directory: '" + cfg.staticDir + "'";
            return -1;
        }
    } else {
        // Try fallback locations if default "static" was not found in current working directory
        if (!isDirectory(cfg.staticDir)) {
            vector<string> candidates = {
                "../static",
                "./static"
            };
            string exeDir = getExecutableDir();
            if (!exeDir.empty()) {
                candidates.push_back(exeDir + "/static");
                candidates.push_back(exeDir + "/../static");
            }

            bool found = false;
            for (const auto& cand : candidates) {
                if (isDirectory(cand)) {
                    cfg.staticDir = cand;
                    found = true;
                    break;
                }
            }

            if (!found) {
                errMessage = "Static asset directory not found. Looked in './static', '../static', and relative to binary. Please specify via --static-dir or HTTP_STATIC_DIR.";
                return -1;
            }
        }
    }

    outConfig = cfg;
    return 0;
}
