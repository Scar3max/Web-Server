#include "TaskQueue.h"
#include <optional>
using namespace std;
void TaskQueue::push(Task task){
    unique_lock<mutex> lock(queueMutex);
    tasks.push(task);
    lock.unlock();
    cv.notify_one();
}   

optional<Task> TaskQueue::pop(){
    unique_lock<mutex> lock(queueMutex);

    cv.wait(lock,[this]{return !tasks.empty() || stop;});
    if(stop && tasks.empty())
        return nullopt;
    Task task = tasks.front();
    tasks.pop();
    return task;
}
void TaskQueue::notifyAll(){
    cv.notify_all();
}
void TaskQueue::shutdown(){
    unique_lock<mutex>lock(queueMutex);
    stop=true;
    lock.unlock();
    cv.notify_all();
}
