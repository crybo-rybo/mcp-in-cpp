# A program that uses the installed package, as a project of its own would:
#
#   cmake --install build/release --prefix /tmp/mcp
#   cmake -S tests/install -B build/consumer -DCMAKE_PREFIX_PATH=/tmp/mcp
#   cmake --build build/consumer && build/consumer/consumer
cmake_minimum_required(VERSION 3.30)
project(mcp-consumer LANGUAGES CXX)

find_package(mcp-in-cpp 0.1 REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE mcp::mcp)
