#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <variant>
#include <list>
#include <map>
#include <sstream>

namespace Bencode {
    class Token {
    public:

        enum ContentType {
            integer = 0,
            string = 1,
            list = 2,
            dict = 3
        };

        Token(const std::string &text, size_t &start_pos);

        [[nodiscard]] std::string GetRawText() const;

        [[nodiscard]] size_t GetValueType() const;

        [[nodiscard]] int64_t GetInteger() const;

        [[nodiscard]] std::string GetString() const;

        [[nodiscard]] std::vector<Token> GetList() const;

        [[nodiscard]] std::map<std::string, Token> GetDict() const;

    private:
        size_t _state;
        std::variant<int64_t, std::string, std::vector<Token>, std::map<std::string, Token>> _data;

        void DecodeInteger(const std::string &text, size_t &start_pos);

        void DecodeString(const std::string &text, size_t &start_pos);

        void DecodeList(const std::string &text, size_t &start_pos);

        void DecodeDict(const std::string &text, size_t &start_pos);
    };
}
