#pragma once
#include <string>
#include <string_view>

namespace pubg_vision::core {
// Values emitted by this project are strings, integers, booleans and null only.
inline std::string json_string(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}
} // namespace pubg_vision::core
