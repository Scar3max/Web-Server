#pragma once

#include <vector>
#include <thread>
#include <atomic>
#include "TaskQueue.h"
#include "ResponseQueue.h"

class ThreadPool {
private:
    std::vector<std::thread> workers;
    TaskQueue& taskQueue;
    ResponseQueue& responseQueue;
    void worker();
    std::atomic<bool> stop;

public:
    ThreadPool(int numThreads, TaskQueue& tQueue, ResponseQueue& rQueue);
    ~ThreadPool();
};
