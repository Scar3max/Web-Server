#include<cstring>
#include<cctype>
#include<iostream>
#include<netinet/in.h>
#include<sys/socket.h>
#include<arpa/inet.h>
#include<unistd.h>
#include "HttpParser.h"
#include "Router.h"
#include "FileManager.h"
#include "Logger.h"
#include <fcntl.h>
#include <cerrno>
#include<sys/epoll.h>
#include "TaskQueue.h"
#include "ThreadPool.h"
using namespace std;

constexpr int SERVER_PORT       = 8080;
constexpr int THREAD_POOL_SIZE  = 8;
constexpr int RECV_BUFFER_SIZE  = 4096;
constexpr int MAX_EPOLL_EVENTS  = 64;
constexpr int LISTEN_BACKLOG    = 128;

string HomeHandler(){
    return FileManager::readFile("../static/index.html");
}
string AboutHandler(){
    return FileManager::readFile("../static/about.html");
}

int main(){
    Router router;
    router.addRoute("/",HomeHandler);
    router.addRoute("/about",AboutHandler);
    
    TaskQueue  taskQueue;
    ThreadPool pool(THREAD_POOL_SIZE,taskQueue);

    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if(serverSocket<0){
        perror("Socket Failed!!");
        exit(EXIT_FAILURE);
    }

    int serverFlags=fcntl(serverSocket,F_GETFL,0);
    fcntl(serverSocket,F_SETFL,serverFlags|O_NONBLOCK);

    struct sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(SERVER_PORT);
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    int opt = 1;

    if (setsockopt(serverSocket,
            SOL_SOCKET,
            SO_REUSEADDR,
            &opt,
            sizeof(opt)) < 0)
    {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
    if(bind(serverSocket,(struct sockaddr*)&serverAddr,sizeof(serverAddr))<0)
    {
        perror("Bind Failed");
        exit(EXIT_FAILURE);
    }	
    if(listen(serverSocket,LISTEN_BACKLOG)<0){
        perror("Listen Failed");
        exit(EXIT_FAILURE);
    }
    int epollFd= epoll_create1(0);
    if(epollFd < 0){
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = serverSocket;
    if(epoll_ctl(epollFd,EPOLL_CTL_ADD,serverSocket,&event) < 0){
        perror("epoll_ctl");
        exit(EXIT_FAILURE);
    }
    epoll_event events[MAX_EPOLL_EVENTS];
    sockaddr_in clientAddr{};
    socklen_t clientLen = sizeof(clientAddr);
    Logger::log("Server Started on port " + to_string(SERVER_PORT));
    while(true){
        int readyCount = epoll_wait(epollFd,events,MAX_EPOLL_EVENTS,-1);
        if(readyCount < 0){
            perror("epoll_wait");
            continue;
        }

        for(int i=0;i<readyCount;i++){
            int currentFd = events[i].data.fd;
            if(currentFd == serverSocket){
                while(true){
                    int clientSocket = accept(serverSocket,(struct sockaddr *)&clientAddr, &clientLen);
                    if(clientSocket < 0){
                        if(errno == EAGAIN||errno == EWOULDBLOCK){
                            break;
                        }
                        perror("accept");
                        break;
                    }
                    int flags = fcntl(clientSocket,F_GETFL,0);
                    if(flags==-1){
                        close(clientSocket);
                        continue;
                    }
                    fcntl(clientSocket,F_SETFL,flags|O_NONBLOCK);
                    epoll_event clientEvent{};
                    clientEvent.events = EPOLLIN;
                    clientEvent.data.fd = clientSocket;
                    if(epoll_ctl(epollFd,EPOLL_CTL_ADD,clientSocket,&clientEvent) < 0){
                        perror("epoll_ctl");
                        close(clientSocket);
                        continue;
                    }
                }
            }
            else{
                char buffer[RECV_BUFFER_SIZE];

                int bytes = recv(currentFd, buffer, sizeof(buffer)-1,0);
                if(bytes < 0){
                    if(errno == EAGAIN ||errno == EWOULDBLOCK){
                        continue;
                    }
                    epoll_ctl(epollFd,EPOLL_CTL_DEL,currentFd,nullptr);
                    close(currentFd);
                    continue;
                }
                if(bytes == 0){
                    epoll_ctl(epollFd,EPOLL_CTL_DEL,currentFd,nullptr);
                    close(currentFd);
                    continue;
                }

                buffer[bytes]='\0';

                string rawRequest(buffer);
                if(rawRequest.find("\r\n\r\n") == string::npos){
                    epoll_ctl(epollFd,EPOLL_CTL_DEL,currentFd,nullptr);
                    close(currentFd);
                    continue;
                }

                Task task;
                task.clientSocket = currentFd;
                task.rawRequest = rawRequest;
                task.router=&router;
                epoll_ctl(epollFd,EPOLL_CTL_DEL,currentFd,nullptr);
                taskQueue.push(task);
            }
        }
    
    }
        
        
    close(serverSocket);
    return 0;
} 
