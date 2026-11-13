cmake_minimum_required(VERSION 3.16)
project(logpipe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(logpipe_core STATIC
  src/line_splitter.cpp
  src/file_sink.cpp
  src/http.cpp
  src/metrics.cpp
)

target_include_directories(logpipe_core PUBLIC src)

if(WIN32)
  target_link_libraries(logpipe_core PUBLIC ws2_32)
endif()

add_executable(logpipe src/main.cpp src/server.cpp)
target_link_libraries(logpipe PRIVATE logpipe_core)

add_executable(logpipe_tests tests/test_main.cpp)
target_link_libraries(logpipe_tests PRIVATE logpipe_core)

add_executable(logpipe_bench bench/bench.cpp)
target_link_libraries(logpipe_bench PRIVATE logpipe_core)

enable_testing()
add_test(NAME unit_tests COMMAND logpipe_tests)
