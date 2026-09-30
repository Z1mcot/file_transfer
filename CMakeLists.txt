cmake_minimum_required(VERSION 3.20)
project(file_transfer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_definitions(_FILE_OFFSET_BITS=64)

find_package(Threads REQUIRED)
include(CTest)

add_library(project_options INTERFACE)
target_compile_options(project_options INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    $<$<CONFIG:Release>:-O2>
)

add_library(file_transfer_core
    src/checksum/crc32.cpp
    src/protocol/protocol.cpp
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
    src/application/server_workers.cpp
    src/application/server_application.cpp
    src/application/client_application.cpp
)
target_include_directories(file_transfer_core PUBLIC include)
target_link_libraries(file_transfer_core PUBLIC project_options Threads::Threads)

add_executable(file_transfer src/main.cpp)
target_link_libraries(file_transfer PRIVATE file_transfer_core)

if(BUILD_TESTING)
    add_executable(file_transfer_unit_tests tests/unit_tests.cpp)
    target_link_libraries(file_transfer_unit_tests PRIVATE file_transfer_core)
    add_test(NAME unit COMMAND file_transfer_unit_tests)

    add_executable(file_transfer_integration_tests tests/integration_tests.cpp)
    target_link_libraries(file_transfer_integration_tests PRIVATE project_options Threads::Threads)
    add_test(NAME integration
        COMMAND file_transfer_integration_tests $<TARGET_FILE:file_transfer>)
    set_tests_properties(integration PROPERTIES TIMEOUT 90)
endif()