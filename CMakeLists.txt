cmake_minimum_required(VERSION 3.28)
project(FastTorrentClient)

set(CMAKE_CXX_STANDARD 20)

add_executable(FastTorrentClient src/main.cpp
        src/torrent_tracker.cpp
        src/torrent_file.cpp
        src/bencode.cpp
        include/torrent_tracker.h
        include/torrent_file.h
        include/bencode.h
        include/peer.h)
