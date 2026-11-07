#pragma once

#include <string>
#include <queue>
#include <vector>
#include <mutex>
#include <cstdint>

struct HttpResponseMessage {
    uint64_t connectionId = 0;
    int clientSocket = -1;
    std::string rawResponse;
    bool closeConnection = true;
};

class ResponseQueue {
private:
    std::queue<HttpResponseMessage> queue;
    std::mutex queueMutex;
    int wakeupFd = -1;

public:
    ResponseQueue() = default;

    void setWakeupFd(int fd) {
        wakeupFd = fd;
    }

    void push(HttpResponseMessage response);
    std::vector<HttpResponseMessage> popAll();
};
