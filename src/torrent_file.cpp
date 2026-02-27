#include "../include/torrent_file.h"
#include "../include/bencode.h"
#include <openssl/sha.h>
#include <fstream>
#include <sstream>

TorrentFile LoadTorrentFile(const std::string &filename) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open file");

    long size = file.tellg();
    std::string data(size, '\0');
    file.seekg(0), file.read(&data[0], size);

    size_t pos = 0;
    Bencode::Token token(data, pos);
    auto info = token.GetDict().at("info");

    TorrentFile ans;

    ans.length = 0;
    if (token.GetDict().contains("announce-list"))
        ans.announce = token.GetDict().at("announce-list").GetList()[0].GetList()[0].GetString();
    else
        ans.announce = token.GetDict().at("announce").GetString();
    ans.name = info.GetDict().at("name").GetString();
    ans.pieceLength = info.GetDict().at("piece length").GetInteger();

    if (token.GetDict().contains("comment"))
        ans.comment = token.GetDict().at("comment").GetString();

    if (info.GetDict().contains("length"))
        ans.length = info.GetDict().at("length").GetInteger();

    std::string pieces = info.GetDict().at("pieces").GetString();
    if (pieces.size() % 20) throw std::runtime_error("Wrong pieces size");
    for (size_t i = 0; i < pieces.size(); i += 20)
        ans.pieceHashes.emplace_back(pieces.substr(i, 20));

    {
        data = info.GetRawText();
        unsigned char info_hash[20];
        SHA1(reinterpret_cast<const unsigned char *>(data.data()), data.size(), info_hash);
        for (auto i: info_hash) ans.infoHash += i;
    }

    return ans;
}



