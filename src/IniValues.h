#pragma once

#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

// Parsing of INI values, kept apart from the Win32 reader so it can be tested without the game.
namespace IniValues
{
    inline bool EqualsNoCase(std::wstring_view value, std::wstring_view lowerWord)
    {
        if (value.size() != lowerWord.size()) {
            return false;
        }
        for (std::size_t i = 0; i < value.size(); ++i) {
            const wchar_t c = value[i] >= L'A' && value[i] <= L'Z' ? static_cast<wchar_t>(value[i] - L'A' + L'a') : value[i];
            if (c != lowerWord[i]) {
                return false;
            }
        }
        return true;
    }

    // true/yes/on/1 and false/no/off/0 in any letter case. Anything else is not a switch value.
    inline std::optional<bool> ParseSwitch(std::wstring_view value)
    {
        constexpr std::array<std::wstring_view, 4> on{ L"true", L"yes", L"on", L"1" };
        constexpr std::array<std::wstring_view, 4> off{ L"false", L"no", L"off", L"0" };
        for (const auto word : on) {
            if (EqualsNoCase(value, word)) {
                return true;
            }
        }
        for (const auto word : off) {
            if (EqualsNoCase(value, word)) {
                return false;
            }
        }
        return std::nullopt;
    }

    // A decimal number that fills the whole value. Parsed without the process locale, which another
    // plugin may have changed: "2.0" must read the same on a system whose decimal separator is a comma.
    inline std::optional<double> ParseNumber(std::wstring_view value)
    {
        std::string text;
        text.reserve(value.size());
        for (const wchar_t c : value) {
            if (c > 0x7F) {
                return std::nullopt;
            }
            text.push_back(static_cast<char>(c));
        }
        const char* first = text.data();
        const char* const last = first + text.size();
        if (first != last && *first == '+') {
            ++first;  // from_chars takes a minus sign only
            if (first != last && *first == '-') {
                return std::nullopt;  // "+-5" is not a number
            }
        }
        double number{};
        const auto parsed = std::from_chars(first, last, number);
        if (parsed.ec != std::errc{} || parsed.ptr != last) {
            return std::nullopt;
        }
        return number;
    }
}
