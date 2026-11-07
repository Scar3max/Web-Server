#pragma once

#include <string>

class FileManager {
public:
    // Reads a binary or text file safely into outData
    static bool readFileBinary(const std::string& path, std::string& outData);

    // Legacy string reader for existing route compatibility
    static std::string readFile(const std::string& path);

    // Determines MIME type based on file extension
    static std::string getMimeType(const std::string& path);

    // Safely resolves requestPath within baseDir, preventing directory traversal
    static bool resolveSafePath(const std::string& baseDir, const std::string& requestPath, std::string& resolvedPath);
};
