cmake_minimum_required(VERSION 3.16)
project(redis_clone LANGUAGES CXX)

add_library(redis-resp
    resp/resp.cpp
)
target_include_directories(redis-resp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(redis-resp PUBLIC cxx_std_17)
target_compile_options(redis-resp PRIVATE -Wall -Wextra -Wpedantic)

add_executable(redis-server
    main.cpp
    net/socket.cpp
)

target_compile_features(redis-server PRIVATE cxx_std_17)
target_compile_options(redis-server PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(redis-server PRIVATE redis-resp)

include(CTest)
if(BUILD_TESTING)
    add_executable(resp-test
        tests/resp_test.cpp
    )
    target_compile_options(resp-test PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(resp-test PRIVATE redis-resp)
    add_test(NAME resp-test COMMAND resp-test)
endif()