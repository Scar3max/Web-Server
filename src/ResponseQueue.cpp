#include "ResponseQueue.h"
#include <unistd.h>
#include <sys/eventfd.h>

void ResponseQueue::push(HttpResponseMessage response) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        queue.push(std::move(response));
    }
    if (wakeupFd >= 0) {
        uint64_t val = 1;
        ssize_t s = write(wakeupFd, &val, sizeof(val));
        (void)s;
    }
}

std::vector<HttpResponseMessage> ResponseQueue::popAll() {
    std::vector<HttpResponseMessage> results;
    std::lock_guard<std::mutex> lock(queueMutex);
    while (!queue.empty()) {
        results.push_back(std::move(queue.front()));
        queue.pop();
    }
    return results;
}
