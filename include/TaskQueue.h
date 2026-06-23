#pragma once
#include<optional>
#include <string>
#include<queue>
#include<mutex>
#include<condition_variable>
#include "Router.h"

struct Task{
    int clientSocket;
    std::string rawRequest;
    Router* router;
};

class TaskQueue{
    private:
        std::queue<Task> tasks;
        std::mutex queueMutex;
        std::condition_variable cv;
        bool stop =false;
    public:
        void push(Task task);
        std::optional<Task> pop();
        void notifyAll();
        void shutdown();
};
