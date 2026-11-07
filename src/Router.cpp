#include "Router.h"
#include "FileManager.h"

using namespace std;

Router::Router(const string& staticDir) : staticDirectory(staticDir) {}

void Router::addRoute(const string& path, function<HttpResponse()> handler) {
    routes[path] = handler;
}

void Router::addRoute(const string& path, function<string()> legacyHandler) {
    routes[path] = [legacyHandler]() -> HttpResponse {
        string body = legacyHandler();
        return HttpResponse{200, "text/html; charset=utf-8", body};
    };
}

HttpResponse Router::route(const string& path) {
    // 1. Check explicit registered routes
    auto it = routes.find(path);
    if (it != routes.end()) {
        return it->second();
    }

    // 2. Try resolving as a static asset from staticDirectory
    string resolvedPath;
    if (FileManager::resolveSafePath(staticDirectory, path, resolvedPath)) {
        string fileData;
        if (FileManager::readFileBinary(resolvedPath, fileData)) {
            string mime = FileManager::getMimeType(resolvedPath);
            return HttpResponse{200, mime, std::move(fileData)};
        }
    }

    // 3. Fallback: 404 Not Found
    return HttpResponse{404, "text/html; charset=utf-8", "<h1>404 Not Found</h1>"};
}
