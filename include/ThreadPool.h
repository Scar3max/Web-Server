#pragma once
#include<vector>
#include<thread>
#include "TaskQueue.h"
#include<atomic>

class ThreadPool{
    private:
        std::vector<std::thread> workers;
        TaskQueue& taskQueue;
        void worker();
        std::atomic<bool> stop;
    public:
        ThreadPool(int numThreads,TaskQueue& queue);
        ~ThreadPool();
};