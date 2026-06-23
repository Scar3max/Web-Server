cmake_minimum_required(VERSION 3.10)

project(HttpServer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

add_executable(server
    src/main.cpp
    src/HttpParser.cpp
    src/Router.cpp
    src/FileManager.cpp
    src/Logger.cpp
    src/TaskQueue.cpp
    src/ThreadPool.cpp
)