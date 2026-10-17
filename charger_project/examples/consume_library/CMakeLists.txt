cmake_minimum_required(VERSION 3.16)
project(charger_monitor_consumer_example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Consumes the installed package exactly as a third-party project would:
#   cmake --install build --prefix /some/prefix
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=/some/prefix
find_package(charger_monitor REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE charger::core)

enable_testing()
add_test(NAME consumer_ledger COMMAND consumer)