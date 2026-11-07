cmake_minimum_required(VERSION 3.10)

project(HttpServer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

add_executable(server
    src/main.cpp
    src/Connection.cpp
    src/HttpParser.cpp
    src/Router.cpp
    src/FileManager.cpp
    src/Logger.cpp
    src/TaskQueue.cpp
    src/ResponseQueue.cpp
    src/ThreadPool.cpp
    src/WebSocketHandshake.cpp
    src/WebSocketFrame.cpp
    src/Json.cpp
    src/Physics.cpp
    src/SnapshotQueue.cpp
    src/SimulationThread.cpp
    src/RoomManager.cpp
)

target_link_libraries(server ${CMAKE_DL_LIBS})

# ThreadSanitizer build of the full server for comprehensive concurrency verification
add_executable(server_tsan
    src/main.cpp
    src/Connection.cpp
    src/HttpParser.cpp
    src/Router.cpp
    src/FileManager.cpp
    src/Logger.cpp
    src/TaskQueue.cpp
    src/ResponseQueue.cpp
    src/ThreadPool.cpp
    src/WebSocketHandshake.cpp
    src/WebSocketFrame.cpp
    src/Json.cpp
    src/Physics.cpp
    src/SnapshotQueue.cpp
    src/SimulationThread.cpp
    src/RoomManager.cpp
)
target_compile_options(server_tsan PRIVATE -fsanitize=thread -g -O1)
target_link_options(server_tsan PRIVATE -fsanitize=thread)
target_link_libraries(server_tsan ${CMAKE_DL_LIBS})
