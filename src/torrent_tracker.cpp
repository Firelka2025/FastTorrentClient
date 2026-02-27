#include "../include/torrent_tracker.h"
#include "../include/bencode.h"
#include "../include/peer.h"
#include <cpr/cpr.h>

#include <utility>

const std::vector<Peer> &TorrentTracker::GetPeers() const { return peers_; }

TorrentTracker::TorrentTracker(std::string url) : url_(std::move(url)) {}

int64_t TorrentTracker::UpdatePeers(const TorrentFile &tf, const std::string &peerId, int port, size_t downloaded) {
    peers_.clear();
    cpr::Parameters params{
            {"info_hash",  tf.infoHash},
            {"peer_id",    peerId},
            {"port",       std::to_string(port)},
            {"uploaded",   std::to_string(0)},
            {"downloaded", std::to_string(downloaded)},
            {"left",       std::to_string(tf.length - downloaded)},
            {"compact",    std::to_string(1)}
    };
    if (!downloaded) params.Add({"event", "started"});
    else if (downloaded == tf.length) params.Add({"event", "completed"});

    cpr::Response res = cpr::Get(
            cpr::Url{tf.announce},
            params,
            cpr::Timeout{5000}
    );


    if (res.status_code != 200)
        throw std::runtime_error("error getting response: " + res.status_line);


    size_t pos = 0;
    Bencode::Token x(res.text, pos);
    if (x.GetDict().at("peers").GetValueType() == Bencode::Token::string) {
        std::string h = x.GetDict().at("peers").GetString();
        if (h.size() % 6) throw std::runtime_error("Invalid peers format");

        for (size_t i = 0; i < h.length(); i += 6) {
            std::string ip = std::to_string(uint8_t(h[i])) + '.' +
                             std::to_string(uint8_t(h[i + 1])) + '.' +
                             std::to_string(uint8_t(h[i + 2])) + '.' +
                             std::to_string(uint8_t(h[i + 3]));

            peers_.emplace_back(ip, uint16_t((uint16_t(uint8_t(h[i + 4])) << 8) + uint16_t(uint8_t(h[i + 5]))));
        }
    } else {
        for (Bencode::Token &i: x.GetDict().at("peers").GetList()) {
            peers_.emplace_back(i.GetDict().at("ip").GetString(),
                                static_cast<int>(i.GetDict().at("port").GetInteger()));
        }
    }

    if (x.GetDict().at("interval").GetValueType() == Bencode::Token::integer)
        return x.GetDict().at("interval").GetInteger();
    return 600;
}
