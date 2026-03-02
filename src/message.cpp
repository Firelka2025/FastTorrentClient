#include "message.h"

Message::Message(uint8_t id) : id_(id) {}

Message::Message(uint8_t id, std::vector<uint8_t> &&payload) : id_(id), payload_(std::move(payload)) {}

Message::Message(uint8_t id, std::span<uint8_t> payload) : id_(id), payload_(payload.size()) {
    std::copy(payload.begin(), payload.end(), payload_.begin());
}

void Message::PrepareMemToSend(std::span<uint8_t> &mem) const {
    if (id_ == KeepAlive) {
        for (int i = 0; i < 4; ++i) mem[i] = 0;
        return;
    }
    size_t message_length = payload_.size() + 1;
    mem[0] = uint8_t((message_length >> 24) & 0xFF);
    mem[1] = uint8_t((message_length >> 16) & 0xFF);
    mem[2] = uint8_t((message_length >> 8) & 0xFF);
    mem[3] = uint8_t(message_length & 0xFF);
    mem[4] = id_;
    std::copy(payload_.begin(), payload_.end(), mem.begin() + 5);
}

size_t Message::GetMessageLength() const {
    return 4 + ((id_ == KeepAlive) ? 0 : 1 + payload_.size());
}

