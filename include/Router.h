#pragma once

#include <string>
#include <unordered_map>
#include <functional>

struct HttpResponse {
    int status = 200;
    std::string contentType = "text/html; charset=utf-8";
    std::string body;
};

class Router {
private:
    std::unordered_map<std::string, std::function<HttpResponse()>> routes;
    std::string staticDirectory;

public:
    Router(const std::string& staticDir = "static");

    void addRoute(const std::string& path, std::function<HttpResponse()> handler);
    void addRoute(const std::string& path, std::function<std::string()> legacyHandler);

    HttpResponse route(const std::string& path);
};
