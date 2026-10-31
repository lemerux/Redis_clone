cmake_minimum_required(VERSION 3.16)
project(redis_clone LANGUAGES CXX)

add_executable(redis-server
    src/main.cpp
    src/net/socket.cpp
)

target_compile_features(redis-server PRIVATE cxx_std_17)
target_compile_options(redis-server PRIVATE -Wall -Wextra -Wpedantic)