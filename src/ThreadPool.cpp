#include "ThreadPool.h"
#include "HttpRequest.h"
#include "HttpParser.h"
#include<netinet/in.h>
#include<sys/socket.h>
#include<arpa/inet.h>
#include<unistd.h>
#include<iostream>
#include<thread>
using namespace std;
ThreadPool::ThreadPool(int numThreads,TaskQueue& queue):taskQueue(queue),stop(false){
    for(int i{0};i<numThreads;i++){
        workers.emplace_back(&ThreadPool::worker,this);
    }
}
void ThreadPool::worker(){
    while(true){
        auto task = taskQueue.pop();
        if(!task){
            break;
        }
        HttpRequest request = parseRequest(task->rawRequest);
        string body = task->router->route(request.path);
        int status = 200;
        if(body.empty()){
            body ="<h1>404 Not Found</h1>";
            status = 404;
        }
        string statusText = (status == 200)? "OK": "Not Found";

        string response ="HTTP/1.1 " + to_string(status) +" " + statusText +"\r\n" +"Content-Type: text/html\r\n" +"Content-Length: " + to_string(body.size()) +"\r\n" +"Connection: close\r\n" +"\r\n" +body;
    //     cout
    // << "Worker "
    // << this_thread::get_id()
    // << " started task "
    // << endl;
    //     //sleep(5);
    //     cout
    // << "Worker "
    // << this_thread::get_id()
    // << " finished task "
    // << endl;
        send(task->clientSocket,response.c_str(),response.size(),0);
        close(task->clientSocket);
    }
}

ThreadPool::~ThreadPool(){
    taskQueue.shutdown();  
    for(auto& worker : workers){
        if(worker.joinable())
            worker.join();
    }
}