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

using namespace std;

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
    
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(serverSocket,F_SETFL,O_NONBLOCK);
    
    if(serverSocket<0){
        perror("Socket Failed!!");
        exit(EXIT_FAILURE);
    }
    struct sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(8080);
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    int addrlen = sizeof(serverAddr);
    int opt = 1;

    if (setsockopt(serverSocket,
            SOL_SOCKET,
            SO_REUSEADDR,
            &opt,
            sizeof(opt)) < 0)
    {
    perror("setsockopt");
}
    if(bind(serverSocket,(struct sockaddr*)&serverAddr,sizeof(serverAddr))<0)
    {
        perror("Bind Failed");
        exit(EXIT_FAILURE);
    }	
    if(listen(serverSocket,5)<0){
        perror("Listen Failed");
        exit(EXIT_FAILURE);
    }
    sockaddr_in clientAddr{};
    socklen_t clientLen = sizeof(clientAddr);
    Logger::log("Server Started");
    while(true){
        int clientSocket = accept(serverSocket,(struct sockaddr *)&clientAddr, &clientLen);
        fcntl(clientSocket,F_SETFL,O_NONBLOCK);
        if(clientSocket < 0){
            perror("accept");
            continue;
        }

        char buffer[4096];

        int bytes = recv(clientSocket, buffer, sizeof(buffer)-1,0);
        if(bytes <= 0){
                close(clientSocket);
                continue;
            }

        buffer[bytes] = '\0';
        string rawRequest(buffer);
        HttpRequest request =parseRequest(rawRequest);
        Logger::log(request.method + " "  + request.path);
        string body = router.route(request.path);

        string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: " + to_string(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" +
        body;
        std::cout
            << "Method: "
            << request.method
            << std::endl;

        std::cout
            << "Path: "
            << request.path
            << std::endl;

        std::cout
            << "Version: "
            << request.version
            << std::endl;
        send(clientSocket,response.c_str(),response.size(),0);
        close(clientSocket);

    }
    close(serverSocket);
    return 0;
} 
