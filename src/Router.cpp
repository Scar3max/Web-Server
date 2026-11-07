#include "Router.h"
#include "FileManager.h"

using namespace std;

Router::Router(const string& staticDir) : staticDirectory(staticDir) {}

void Router::addRoute(const string& path, function<HttpResponse()> handler) {
    routes[path] = handler;
}

void Router::addRoute(const string& path, function<HttpResponse(const HttpRequest&)> handler) {
    requestRoutes[path] = handler;
}

void Router::addRoute(const string& path, function<string()> legacyHandler) {
    routes[path] = [legacyHandler]() -> HttpResponse {
        string body = legacyHandler();
        return HttpResponse{200, "text/html; charset=utf-8", body};
    };
}

HttpResponse Router::route(const string& path) {
    auto reqIt = requestRoutes.find(path);
    if (reqIt != requestRoutes.end()) {
        HttpRequest req;
        req.path = path;
        return reqIt->second(req);
    }

    auto it = routes.find(path);
    if (it != routes.end()) {
        return it->second();
    }

    string resolvedPath;
    if (FileManager::resolveSafePath(staticDirectory, path, resolvedPath)) {
        string fileData;
        if (FileManager::readFileBinary(resolvedPath, fileData)) {
            string mime = FileManager::getMimeType(resolvedPath);
            return HttpResponse{200, mime, std::move(fileData)};
        }
    }

    return HttpResponse{404, "text/html; charset=utf-8", "<h1>404 Not Found</h1>"};
}

HttpResponse Router::route(const HttpRequest& request) {
    auto reqIt = requestRoutes.find(request.path);
    if (reqIt != requestRoutes.end()) {
        return reqIt->second(request);
    }

    auto it = routes.find(request.path);
    if (it != routes.end()) {
        return it->second();
    }

    string resolvedPath;
    if (FileManager::resolveSafePath(staticDirectory, request.path, resolvedPath)) {
        string fileData;
        if (FileManager::readFileBinary(resolvedPath, fileData)) {
            string mime = FileManager::getMimeType(resolvedPath);
            return HttpResponse{200, mime, std::move(fileData)};
        }
    }

    return HttpResponse{404, "text/html; charset=utf-8", "<h1>404 Not Found</h1>"};
}
