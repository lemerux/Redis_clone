cmake_minimum_required(VERSION 3.16)
project(redis_clone LANGUAGES CXX)

option(BUILD_EXPIRY_BENCHMARKS "Build the expiry performance benchmark" OFF)
option(BUILD_ZSET_BENCHMARKS "Build the sorted-set performance benchmark" OFF)
option(BUILD_REDIS_DIFFERENTIAL_TESTS
       "Enable differential tests against a local real Redis server" OFF)

add_library(redis-resp
    resp/resp.cpp
)
target_include_directories(redis-resp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(redis-resp PUBLIC cxx_std_17)
target_compile_options(redis-resp PRIVATE -Wall -Wextra -Wpedantic)

add_library(redis-store
    store/expiry_index.cpp
    store/key_table.cpp
    store/database.cpp
    store/skip_list.cpp
)
target_include_directories(redis-store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(redis-store PUBLIC cxx_std_17)
target_compile_options(redis-store PRIVATE -Wall -Wextra -Wpedantic)

add_library(redis-net
    alloc/alloc_stats.cpp
    command/command.cpp
    command/cmd_auth.cpp
    command/cmd_string.cpp
    command/cmd_hash.cpp
    command/cmd_list.cpp
    command/cmd_set.cpp
    command/cmd_zset.cpp
    command/cmd_keys.cpp
    command/cmd_server.cpp
    net/event_loop.cpp
    net/socket.cpp
    user/user_registry.cpp
)
target_include_directories(redis-net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(redis-net PUBLIC cxx_std_17)
target_compile_options(redis-net PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(redis-net PUBLIC redis-resp redis-store)
target_link_libraries(redis-net PRIVATE ${CMAKE_DL_LIBS})

add_executable(redis-server
    main.cpp
)

target_compile_features(redis-server PRIVATE cxx_std_17)
target_compile_options(redis-server PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(redis-server PRIVATE redis-net)

if(BUILD_EXPIRY_BENCHMARKS)
    add_executable(expiry-benchmark
        benchmarks/expiry_benchmark.cpp
    )
    target_compile_options(expiry-benchmark PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(expiry-benchmark PRIVATE redis-net)
endif()

if(BUILD_ZSET_BENCHMARKS)
    add_executable(zset-benchmark
        benchmarks/zset_benchmark.cpp
    )
    target_compile_options(zset-benchmark PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(zset-benchmark PRIVATE redis-store)
endif()

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

    add_executable(container-command-test
        tests/container_command_test.cpp
    )
    target_compile_options(container-command-test PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(container-command-test PRIVATE redis-net)
    add_test(NAME container-command-test COMMAND container-command-test)

    if(BUILD_REDIS_DIFFERENTIAL_TESTS)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        find_program(REDIS_SERVER_EXECUTABLE redis-server)
        if(NOT REDIS_SERVER_EXECUTABLE)
            message(FATAL_ERROR
                    "BUILD_REDIS_DIFFERENTIAL_TESTS requires redis-server")
        endif()
        add_test(
            NAME redis-differential-test
            COMMAND
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/redis_differential_test.py
                --clone $<TARGET_FILE:redis-server>
                --reference ${REDIS_SERVER_EXECUTABLE}
        )
        set_tests_properties(redis-differential-test PROPERTIES TIMEOUT 60)
    endif()
endif()