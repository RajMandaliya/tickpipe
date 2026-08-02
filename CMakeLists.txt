cmake_minimum_required(VERSION 3.20)
project(exchange_core VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-O2 -Wall -Wextra -march=native)

# Main library
add_library(exchange_core_lib STATIC
    src/itch_parser.cpp
    src/pipeline.cpp
    src/software_engine.cpp
    src/risk_engine.cpp
    src/wal.cpp
    src/ouch.cpp
    src/numa.cpp
    src/visualizer.cpp
    src/itch_replay.cpp
    src/udp_server.cpp
    src/udp_receiver.cpp
    src/book_state.cpp
    src/symbol_directory.cpp
)

if(WIN32)
    target_link_libraries(exchange_core_lib PUBLIC ws2_32)
endif()

target_include_directories(exchange_core_lib PUBLIC include)

# Main executable
add_executable(exchange_main src/main.cpp)
target_link_libraries(exchange_main PRIVATE exchange_core_lib)

# Benchmark executable
add_executable(benchmark bench/benchmark.cpp)
target_link_libraries(benchmark PRIVATE exchange_core_lib)

# UDP replay server — streams ITCH file over UDP
add_executable(replay_server src/replay_server_main.cpp)
target_link_libraries(replay_server PRIVATE exchange_core_lib)

# Live exchange — receives UDP, runs full pipeline
add_executable(live_exchange src/live_exchange_main.cpp)
target_link_libraries(live_exchange PRIVATE exchange_core_lib)