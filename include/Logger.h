#pragma once

#include <string>

enum class LogLevel {
    DEBUG = 0,
    INFO  = 1,
    WARN  = 2,
    ERROR = 3,
    NONE  = 4
};

class Logger {
public:
    static void init(LogLevel level = LogLevel::INFO, bool toConsole = true, const std::string& logFilePath = "server.log");
    static void setLevel(LogLevel level);
    static LogLevel getLevel();
    static LogLevel parseLevel(const std::string& levelStr);
    static std::string levelToString(LogLevel level);

    static void log(const std::string& message);
    static void log(LogLevel level, const std::string& message);

    static void debug(const std::string& message) { log(LogLevel::DEBUG, message); }
    static void info(const std::string& message)  { log(LogLevel::INFO, message); }
    static void warn(const std::string& message)  { log(LogLevel::WARN, message); }
    static void error(const std::string& message) { log(LogLevel::ERROR, message); }

    static std::string getTime();
};
