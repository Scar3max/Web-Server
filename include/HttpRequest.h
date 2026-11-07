#pragma once 
#include <string>

class HttpRequest {
public:
    std::string method;
    std::string path;
    std::string version;
    std::string query;
    std::string accept;
    std::string userAgent;
    std::string rawRequest;
};