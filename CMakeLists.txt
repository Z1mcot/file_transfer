cmake_minimum_required(VERSION 3.20)
project(file_transfer LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "file_transfer currently supports Linux only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_definitions(_FILE_OFFSET_BITS=64)

find_package(Threads REQUIRED)

add_library(project_options INTERFACE)
target_compile_options(project_options INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    $<$<CONFIG:Release>:-O2>
)

add_executable(file_transfer
    src/main.cpp
    src/checksum/crc32.cpp
    src/protocol/protocol.cpp
    src/protocol/frame_parser.cpp
    src/storage/filename_generator.cpp
    src/storage/file_store.cpp
    src/storage/posix_staged_file.cpp
    src/transfer/transfer.cpp
    src/cli/cli.cpp
    src/transport/tcp_transport.cpp
    src/transport/tcp_listener.cpp
    src/transport/tcp_connector.cpp
    src/application/executable_path.cpp
    src/application/signal_waiter.cpp
    src/application/event_loop.cpp
    src/application/server_application.cpp
    src/application/client_application.cpp
)
target_include_directories(file_transfer PRIVATE include)
target_link_libraries(file_transfer PRIVATE project_options Threads::Threads)