cmake_minimum_required(VERSION 3.17)
project (desktop_stream)

set(CMAKE_CXX_FLAGS "-O0")

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_executable(capture_server src/capture_server.c)
target_include_directories(capture_server PRIVATE /usr/include/schroedinger-1.0/)
target_link_libraries(capture_server X11 Xext Xi Xfixes Xdamage Threads::Threads schroedinger-1.0)

add_executable(display_client src/display_client.c)
target_link_libraries(display_client X11 Xext GL GLX Threads::Threads)
