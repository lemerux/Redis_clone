cmake_minimum_required(VERSION 3.16)
project(redis_clone LANGUAGES CXX)

add_library(redis-resp
    resp/resp.cpp
)
target_include_directories(redis-resp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(redis-resp PUBLIC cxx_std_17)
target_compile_options(redis-resp PRIVATE -Wall -Wextra -Wpedantic)

add_library(redis-store
    store/key_table.cpp
    store/database.cpp
)
target_include_directories(redis-store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(redis-store PUBLIC cxx_std_17)
target_compile_options(redis-store PRIVATE -Wall -Wextra -Wpedantic)

add_library(redis-net
    command/command.cpp
    command/cmd_string.cpp
    command/cmd_keys.cpp
    command/cmd_server.cpp
    net/event_loop.cpp
    net/socket.cpp
)
target_include_directories(redis-net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(redis-net PUBLIC cxx_std_17)
target_compile_options(redis-net PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(redis-net PUBLIC redis-resp redis-store)

add_executable(redis-server
    main.cpp
)

target_compile_features(redis-server PRIVATE cxx_std_17)
target_compile_options(redis-server PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(redis-server PRIVATE redis-net)

include(CTest)
if(BUILD_TESTING)
    add_executable(resp-test
        tests/resp_test.cpp
    )
    target_compile_options(resp-test PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(resp-test PRIVATE redis-resp)
    add_test(NAME resp-test COMMAND resp-test)

    add_executable(event-loop-test
        tests/event_loop_test.cpp
    )
    target_compile_options(event-loop-test PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(event-loop-test PRIVATE redis-net)
    add_test(NAME event-loop-test COMMAND event-loop-test)

    add_executable(database-test
        tests/database_test.cpp
    )
    target_compile_options(database-test PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(database-test PRIVATE redis-store)
    add_test(NAME database-test COMMAND database-test)
endif()