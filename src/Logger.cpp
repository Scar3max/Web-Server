#include "Logger.h"

#include <fstream>
#include <iostream>
#include <chrono>
#include <ctime>
#include <mutex>
#include <algorithm>

using namespace std;

static LogLevel g_logLevel = LogLevel::INFO;
static bool g_logToConsole = true;
static string g_logFilePath = "server.log";
static mutex g_logMutex;

void Logger::init(LogLevel level, bool toConsole, const string& logFilePath) {
    lock_guard<mutex> lock(g_logMutex);
    g_logLevel = level;
    g_logToConsole = toConsole;
    g_logFilePath = logFilePath;
}

void Logger::setLevel(LogLevel level) {
    lock_guard<mutex> lock(g_logMutex);
    g_logLevel = level;
}

LogLevel Logger::getLevel() {
    lock_guard<mutex> lock(g_logMutex);
    return g_logLevel;
}

LogLevel Logger::parseLevel(const string& str) {
    string s = str;
    transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return toupper(c); });
    if (s == "DEBUG") return LogLevel::DEBUG;
    if (s == "INFO") return LogLevel::INFO;
    if (s == "WARN" || s == "WARNING") return LogLevel::WARN;
    if (s == "ERROR") return LogLevel::ERROR;
    if (s == "NONE" || s == "OFF") return LogLevel::NONE;
    throw invalid_argument("Unrecognized log level: " + str);
}

string Logger::levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::NONE:  return "NONE";
    }
    return "INFO";
}

string Logger::getTime() {
    auto now = chrono::system_clock::now();
    time_t current = chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
#if defined(_WIN32)
    localtime_s(&tm_buf, &current);
#else
    localtime_r(&current, &tm_buf);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return string(buf);
}

void Logger::log(const string& message) {
    log(LogLevel::INFO, message);
}

void Logger::log(LogLevel level, const string& message) {
    if (level == LogLevel::NONE) return;

    lock_guard<mutex> lock(g_logMutex);
    if (static_cast<int>(level) < static_cast<int>(g_logLevel)) {
        return;
    }

    string ts = getTime();
    string lvl = levelToString(level);
    string formatted = "[" + ts + "] [" + lvl + "] " + message;

    if (g_logToConsole) {
        if (level >= LogLevel::WARN) {
            cerr << formatted << "\n";
        } else {
            cout << formatted << "\n";
        }
    }

    if (!g_logFilePath.empty()) {
        ofstream logFile(g_logFilePath, ios::app);
        if (logFile.is_open()) {
            logFile << formatted << "\n";
        }
    }
}
