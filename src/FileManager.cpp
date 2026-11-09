#include "FileManager.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <climits>
#include <sys/stat.h>

using namespace std;

static bool fileExistsAndIsRegular(const string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) {
        return S_ISREG(st.st_mode);
    }
    return false;
}

bool FileManager::readFileBinary(const string& path, string& outData) {
    ifstream file(path, ios::binary | ios::ate);
    if (!file.is_open()) {
        return false;
    }

    streamsize size = file.tellg();
    if (size < 0) {
        return false;
    }

    file.seekg(0, ios::beg);
    outData.resize(static_cast<size_t>(size));
    if (size > 0) {
        file.read(&outData[0], size);
    }
    return file.good() || file.eof();
}

string FileManager::readFile(const string& path) {
    string data;
    if (readFileBinary(path, data)) {
        return data;
    }
    return "";
}

string FileManager::getMimeType(const string& path) {
    size_t dotPos = path.find_last_of('.');
    if (dotPos == string::npos) {
        return "application/octet-stream";
    }

    string ext = path.substr(dotPos);
    transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return tolower(c); });

    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css")                   return "text/css";
    if (ext == ".js" || ext == ".mjs")   return "application/javascript";
    if (ext == ".json")                  return "application/json";
    if (ext == ".png")                   return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".svg")                   return "image/svg+xml";
    if (ext == ".ico")                   return "image/x-icon";
    if (ext == ".wasm")                  return "application/wasm";
    if (ext == ".txt")                   return "text/plain; charset=utf-8";

    return "application/octet-stream";
}

bool FileManager::resolveSafePath(const string& baseDir, const string& requestPath, string& resolvedPath) {
    // Prevent directory traversal
    if (requestPath.find("..") != string::npos) {
        return false;
    }

    // Reject URL-encoded traversals (%2e%2e, %2f)
    string lowerReq = requestPath;
    transform(lowerReq.begin(), lowerReq.end(), lowerReq.begin(), [](unsigned char c) { return tolower(c); });
    if (lowerReq.find("%2e") != string::npos || lowerReq.find("%2f") != string::npos) {
        return false;
    }

    // Strip query string and fragment
    string clean = requestPath;
    size_t q = clean.find_first_of("?#");
    if (q != string::npos) {
        clean = clean.substr(0, q);
    }

    // Strip leading slashes
    size_t start = clean.find_first_not_of('/');
    if (start != string::npos) {
        clean = clean.substr(start);
    } else {
        clean = "";
    }

    // Default to index.html if empty
    if (clean.empty()) {
        clean = "index.html";
    }

    auto checkCandidate = [](const string& base, const string& candidate, string& out) -> bool {
        if (!fileExistsAndIsRegular(candidate)) return false;
        char realBase[PATH_MAX];
        char realTarget[PATH_MAX];
        if (realpath(base.c_str(), realBase) != nullptr && realpath(candidate.c_str(), realTarget) != nullptr) {
            string baseStr(realBase);
            string targetStr(realTarget);
            if (targetStr.rfind(baseStr + "/", 0) == 0 || targetStr == baseStr) {
                out = candidate;
                return true;
            }
        }
        return false;
    };

    // Check candidate paths: current working directory or parent directory (e.g. running from build/)
    string candidate1 = baseDir + "/" + clean;
    if (checkCandidate(baseDir, candidate1, resolvedPath)) {
        return true;
    }

    string candidate2 = "../" + baseDir + "/" + clean;
    if (checkCandidate("../" + baseDir, candidate2, resolvedPath)) {
        return true;
    }

    return false;
}
