cmake_minimum_required(VERSION 3.28)
project(FastTorrentClient)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -funroll-loops -O3)
endif ()

include(FetchContent)
FetchContent_Declare(
        cpr
        GIT_REPOSITORY https://github.com/libcpr/cpr.git
        GIT_TAG 1.10.5
)
FetchContent_MakeAvailable(cpr)

add_executable(FastTorrentClient src/main.cpp
        src/torrent_tracker.cpp
        src/torrent_file.cpp
        src/bencode.cpp
        include/torrent_tracker.h
        include/torrent_file.h
        include/bencode.h
        include/peer.h
        src/peer.cpp
        include/byte_tools.h
        src/byte_tools.cpp
        include/message.h
        src/message.cpp
        include/piece_manager.h
        src/piece_manager.cpp
        include/thread_pool.h
        src/thread_pool.cpp
        include/config.h
        src/config.cpp)

target_include_directories(FastTorrentClient PRIVATE include)

find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED liburing)

target_link_libraries(FastTorrentClient
        PRIVATE
        cpr::cpr
        OpenSSL::SSL
        OpenSSL::Crypto
        pthread
        ${LIBURING_LIBRARIES}
)
target_include_directories(FastTorrentClient
        PRIVATE
        ${LIBURING_INCLUDE_DIRS}
)