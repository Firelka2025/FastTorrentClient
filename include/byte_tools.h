#pragma once

#include <string>
#include <cstdint>
#include <span>
#include <vector>

/*
 * Преобразовать 4 байта в формате big endian в int
 */
uint32_t BytesToInt(std::span<uint8_t> bytes);

std::string IntToBytes(uint32_t integer);

/*
 * Расчет SHA1 хеш-суммы. Здесь в результате подразумевается не человеко-читаемая строка, а массив из 20 байтов
 * в том виде, в котором его генерирует библиотека OpenSSL
 */
std::array<uint8_t, 20> CalculateSHA1(const std::vector<uint8_t> &msg);

/*
 * Представить массив байтов в виде строки, содержащей только символы, соответствующие цифрам в шестнадцатеричном исчислении.
 * Конкретный формат выходной строки не важен. Важно то, чтобы выходная строка не содержала символов, которые нельзя
 * было бы представить в кодировке utf-8. Данная функция будет использована для вывода SHA1 хеш-суммы в лог.
 */
std::string HexEncode(const std::string &input);
