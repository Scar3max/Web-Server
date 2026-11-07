#include "ThreadPool.h"
#include "HttpRequest.h"
#include "HttpParser.h"
#include "Router.h"
#include "ServerMetrics.h"
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
        HttpResponse httpResp = task->router->route(request);
        ServerMetrics::instance().recordHttpRequest(httpResp.status);

        string statusText = (httpResp.status == 200) ? "OK" :
                            (httpResp.status == 404 ? "Not Found" : "Error");

        string response = "HTTP/1.1 " + to_string(httpResp.status) + " " + statusText + "\r\n"
                        + "Content-Type: " + httpResp.contentType + "\r\n"
                        + "Content-Length: " + to_string(httpResp.body.size()) + "\r\n"
                        + "Connection: close\r\n\r\n"
                        + httpResp.body;

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
