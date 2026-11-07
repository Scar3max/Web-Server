#include "ThreadPool.h"
#include "HttpRequest.h"
#include "HttpParser.h"
#include <string>

using namespace std;

ThreadPool::ThreadPool(int numThreads, TaskQueue& tQueue, ResponseQueue& rQueue)
    : taskQueue(tQueue), responseQueue(rQueue), stop(false) {
    for (int i = 0; i < numThreads; i++) {
        workers.emplace_back(&ThreadPool::worker, this);
    }
}

void ThreadPool::worker() {
    while (true) {
        auto task = taskQueue.pop();
        if (!task) {
            break;
        }

        HttpRequest request = parseRequest(task->rawRequest);
        string body = task->router->route(request.path);
        int status = 200;
        if (body.empty()) {
            body = "<h1>404 Not Found</h1>";
            status = 404;
        }
        string statusText = (status == 200) ? "OK" : "Not Found";

        string response = "HTTP/1.1 " + to_string(status) + " " + statusText + "\r\n"
                        + "Content-Type: text/html\r\n"
                        + "Content-Length: " + to_string(body.size()) + "\r\n"
                        + "Connection: close\r\n\r\n"
                        + body;

        HttpResponseMessage respMsg;
        respMsg.connectionId = task->connectionId;
        respMsg.clientSocket = task->clientSocket;
        respMsg.rawResponse = std::move(response);
        respMsg.closeConnection = true;

        responseQueue.push(std::move(respMsg));
    }
}

ThreadPool::~ThreadPool() {
    taskQueue.shutdown();
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}
