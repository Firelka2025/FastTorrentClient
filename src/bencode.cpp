#include "../include/bencode.h"

#define data_int get<0>(_data)
#define data_string get<1>(_data)
#define data_list get<2>(_data)
#define data_dict get<3>(_data)

namespace Bencode {
    Token::Token(const std::string &text, size_t &start_pos) : _state(10) {
        if (text[start_pos] == 'i') DecodeInteger(text, start_pos), _state = 0;
        else if (isdigit(text[start_pos])) DecodeString(text, start_pos), _state = 1;
        else if (text[start_pos] == 'l') DecodeList(text, start_pos), _state = 2;
        else if (text[start_pos] == 'd') DecodeDict(text, start_pos), _state = 3;
        else throw std::runtime_error("Cannot find correct type while parsing!");
    }

    std::string Token::GetRawText() const {
        switch (_state) {
            case 0:
                return "i" + std::to_string(data_int) + "e";
            case 1:
                return std::to_string(data_string.size()) + ":" + data_string;
            case 2: {
                std::string ans = "l";
                for (auto &i: data_list) ans += i.GetRawText();
                return ans + "e";
            }
            case 3: {
                std::string ans = "d";
                for (auto &i: data_dict)
                    ans += std::to_string(i.first.size()) + ":" + i.first + i.second.GetRawText();
                return ans + "e";
            }
            default:
                throw std::runtime_error("Get raw text with no initial data!");
        }
    }

    size_t Token::GetValueType() const { return _state; }

    int64_t Token::GetInteger() const { return data_int; }

    std::string Token::GetString() const { return data_string; }

    std::vector<Token> Token::GetList() const { return data_list; }

    std::map<std::string, Token> Token::GetDict() const { return data_dict; }

    void Token::DecodeInteger(const std::string &text, size_t &start_pos) {
        size_t cur_pos = start_pos;

        while (cur_pos < text.length() && text[cur_pos] != 'e') ++cur_pos;
        if (cur_pos == text.length()) throw std::runtime_error("Cannot parse integer");

        _data = int64_t(std::stoll(text.substr(start_pos + 1, cur_pos - start_pos - 1)));
        start_pos = cur_pos + 1;
    }

    void Token::DecodeString(const std::string &text, size_t &start_pos) {
        size_t cur_pos = start_pos;

        while (cur_pos < text.length() && std::isdigit(text[cur_pos])) ++cur_pos;
        if (cur_pos == text.length()) throw std::runtime_error("Cannot parse string(1)");

        int64_t cur_length = std::stoll(text.substr(start_pos, cur_pos - start_pos));
        if (cur_length < 0) throw std::runtime_error("Invalid string length in bencode!");

        _data = text.substr(cur_pos + 1, cur_length);
        if (data_string.length() != static_cast<size_t>(cur_length)) throw std::runtime_error("Cannot parse string(2)");

        start_pos = cur_pos + cur_length + 1;
    }

    void Token::DecodeList(const std::string &text, size_t &start_pos) {
        _data = std::vector<Token>();
        ++start_pos;
        while (start_pos < text.length() && text[start_pos] != 'e') data_list.emplace_back(text, start_pos);
        if (start_pos == text.length()) throw std::runtime_error("Cannot parse list");
        ++start_pos;
    }

    void Token::DecodeDict(const std::string &text, size_t &start_pos) {
        _data = std::map<std::string, Token>();
        ++start_pos;
        while (start_pos < text.length() && text[start_pos] != 'e') {
            std::string key = Token(text, start_pos).GetString();
            Token value(text, start_pos);
            data_dict.emplace(std::move(key), std::move(value));
        }
        if (start_pos == text.length()) throw std::runtime_error("Cannot parse dict");
        ++start_pos;
    }
}
