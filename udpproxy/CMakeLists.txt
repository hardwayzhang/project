cmake_minimum_required(VERSION 3.16)
project(udpproxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(udpproxy
  src/protocol.cpp
  src/session.cpp
  src/port_pool.cpp
  src/registry.cpp
  src/metrics.cpp
  src/proxy.cpp
  src/room_sim.cpp
)
target_include_directories(udpproxy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(udpproxy PRIVATE -Wall -Wextra -Wpedantic)

add_executable(proxy_demo cmd/proxy_demo.cpp)
target_link_libraries(proxy_demo PRIVATE udpproxy)

add_executable(proxy_test tests/proxy_test.cpp)
target_link_libraries(proxy_test PRIVATE udpproxy)

enable_testing()
add_test(NAME proxy_acceptance COMMAND proxy_test)
