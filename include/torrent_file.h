#pragma once

#include <string>
#include <vector>
#include <variant>

struct TorrentFile {
    std::string announce;
    std::string comment;
    std::vector<std::string> pieceHashes;
    size_t pieceLength;
    size_t length = 0;
    std::string name;
    std::string infoHash;
};


TorrentFile LoadTorrentFile(const std::string &filename);
