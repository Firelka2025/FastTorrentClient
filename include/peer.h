#pragma once

#include <string>

struct Peer {
    std::string ip;
    int port;

    inline bool operator==(const Peer &b) const { return ip == b.ip && port == b.port; }
};

