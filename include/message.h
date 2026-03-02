#pragma once

#include <cstdint>
#include <vector>
#include <span>

struct Message {
    enum Type {
        Choke,
        Unchoke,
        Interested,
        NotInterested,
        Have,
        BitField,
        Request,
        Piece,
        Cancel,
        Port,
        KeepAlive
    };

    explicit Message(uint8_t id);

    Message(uint8_t id, std::span<uint8_t> payload);

    Message(uint8_t id, std::vector<uint8_t> &&payload);

    void PrepareMemToSend(std::span<uint8_t> &mem) const;

    [[nodiscard]] size_t GetMessageLength() const;

    const uint8_t id_;
    std::vector<uint8_t> payload_;
};

const Message InterestedMessage(Message::Interested);
const Message KeepAliveMessage(Message::KeepAlive);