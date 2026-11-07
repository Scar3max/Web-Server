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
    src/Track.cpp
    src/ServerMetrics.cpp
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
    src/Track.cpp
    src/ServerMetrics.cpp
)
target_compile_options(server_tsan PRIVATE -fsanitize=thread -g -O1)
target_link_options(server_tsan PRIVATE -fsanitize=thread)
target_link_libraries(server_tsan ${CMAKE_DL_LIBS})

# AddressSanitizer & LeakSanitizer build of the full server for comprehensive memory safety verification
add_executable(server_asan
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
    src/Track.cpp
    src/ServerMetrics.cpp
)
target_compile_options(server_asan PRIVATE -fsanitize=address,undefined -g -O1)
target_link_options(server_asan PRIVATE -fsanitize=address,undefined)
target_link_libraries(server_asan ${CMAKE_DL_LIBS})

# Phase 6.1 Collision Stability and Penetration Verification Binary
add_executable(test_collision_stability
    tests/test_collision_stability.cpp
    src/Physics.cpp
    src/Track.cpp
)
target_link_libraries(test_collision_stability ${CMAKE_DL_LIBS})

# Phase 6.1 Server Track Parity Binary
add_executable(track_parity_cpp
    tests/track_parity_cpp.cpp
    src/Track.cpp
)
target_link_libraries(track_parity_cpp ${CMAKE_DL_LIBS})


