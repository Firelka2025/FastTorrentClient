#include "byte_tools.h"
#include <openssl/sha.h>
#include <sstream>

uint32_t BytesToInt(std::span<uint8_t> bytes) {
    if (bytes.size() != 4) throw std::runtime_error("Wrong BigEndian format");
    return (uint32_t(bytes[0]) << 24) |
           (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) |
           uint32_t(bytes[3]);
}

std::string IntToBytes(uint32_t integer) {
    return {
            char((integer >> 24) & 0xFF),
            char((integer >> 16) & 0xFF),
            char((integer >> 8) & 0xFF),
            char(integer & 0xFF)
    };
}

std::array<uint8_t, 20> CalculateSHA1(const std::vector<uint8_t> &msg) {
    std::array<uint8_t, 20> ans{};
    SHA1(msg.data(), msg.size(), ans.data());
    return ans;
}


std::string HexEncode(const std::string &input) {
    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string output;

    output.reserve(input.size() << 1);

    for (char byte: input) {
        output += hex_chars[byte >> 4];
        output += hex_chars[byte & 0x0F];
    }
    return output;
}