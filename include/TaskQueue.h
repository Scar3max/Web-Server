#pragma once
#include<optional>
#include <string>
#include<queue>
#include<mutex>
#include<condition_variable>
#include "Router.h"
using namespace std;

struct Task{
    int clientSocket;
    string rawRequest;
    Router* router;
};

class TaskQueue{
    private:
        queue<Task> tasks;
        mutex queueMutex;
        condition_variable cv;
        bool stop =false;
    public:
        void push(Task task);
        optional<Task> pop();
        void notifyAll();
        void shutdown();
};
