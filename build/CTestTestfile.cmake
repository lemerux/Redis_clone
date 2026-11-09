# CMake generated Testfile for 
# Source directory: /home/lemerux/Documents/GitHub/Redis_clone/src
# Build directory: /home/lemerux/Documents/GitHub/Redis_clone/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(resp-test "/home/lemerux/Documents/GitHub/Redis_clone/build/resp-test")
set_tests_properties(resp-test PROPERTIES  _BACKTRACE_TRIPLES "/home/lemerux/Documents/GitHub/Redis_clone/src/CMakeLists.txt;78;add_test;/home/lemerux/Documents/GitHub/Redis_clone/src/CMakeLists.txt;0;")
add_test(event-loop-test "/home/lemerux/Documents/GitHub/Redis_clone/build/event-loop-test")
set_tests_properties(event-loop-test PROPERTIES  _BACKTRACE_TRIPLES "/home/lemerux/Documents/GitHub/Redis_clone/src/CMakeLists.txt;85;add_test;/home/lemerux/Documents/GitHub/Redis_clone/src/CMakeLists.txt;0;")
add_test(database-test "/home/lemerux/Documents/GitHub/Redis_clone/build/database-test")
set_tests_properties(database-test PROPERTIES  _BACKTRACE_TRIPLES "/home/lemerux/Documents/GitHub/Redis_clone/src/CMakeLists.txt;92;add_test;/home/lemerux/Documents/GitHub/Redis_clone/src/CMakeLists.txt;0;")
add_test(container-command-test "/home/lemerux/Documents/GitHub/Redis_clone/build/container-command-test")
set_tests_properties(container-command-test PROPERTIES  _BACKTRACE_TRIPLES "/home/lemerux/Documents/GitHub/Redis_clone/src/CMakeLists.txt;99;add_test;/home/lemerux/Documents/GitHub/Redis_clone/src/CMakeLists.txt;0;")
add_test(redis-differential-test "/usr/bin/python3" "/home/lemerux/Documents/GitHub/Redis_clone/src/tests/redis_differential_test.py" "--clone" "/home/lemerux/Documents/GitHub/Redis_clone/build/redis-server" "--reference" "/usr/bin/redis-server")
set_tests_properties(redis-differential-test PROPERTIES  TIMEOUT "60" _BACKTRACE_TRIPLES "/home/lemerux/Documents/GitHub/Redis_clone/src/CMakeLists.txt;108;add_test;/home/lemerux/Documents/GitHub/Redis_clone/src/CMakeLists.txt;0;")
