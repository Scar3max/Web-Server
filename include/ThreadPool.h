#pragma once
#include<vector>
#include<thread>
#include "TaskQueue.h"
#include<atomic>
class ThreadPool{
    private:
        vector<thread> workers;
        TaskQueue& taskQueue;
        void worker();
        atomic<bool> stop;
    public:
        ThreadPool(int numThreads,TaskQueue& queue);
        ~ThreadPool();
};