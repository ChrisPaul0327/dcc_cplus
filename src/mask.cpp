#include "mask.h"

namespace dcc {
namespace {

std::size_t utf8_char_len(unsigned char lead) {
    if ((lead & 0x80U) == 0) return 1;
    if ((lead & 0xe0U) == 0xc0U) return 2;
    if ((lead & 0xf0U) == 0xe0U) return 3;
    if ((lead & 0xf8U) == 0xf0U) return 4;
    return 1;
}

std::size_t codepoint_count(std::string_view s) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < s.size();) {
        i += utf8_char_len(static_cast<unsigned char>(s[i]));
        ++count;
    }
    return count;
}

std::string_view first_codepoint(std::string_view s) {
    const std::size_t len = utf8_char_len(static_cast<unsigned char>(s[0]));
    return s.substr(0, len <= s.size() ? len : 1);
}

std::string_view last_codepoint(std::string_view s) {
    std::size_t last = 0;
    for (std::size_t i = 0; i < s.size();) {
        last = i;
        i += utf8_char_len(static_cast<unsigned char>(s[i]));
    }
    return s.substr(last);
}

} // namespace

std::string mask_value(std::string_view value) {
    if (value.empty()) {
        return std::string();
    }
    std::string out;
    out.reserve(value.size() + 5);
    out.append(first_codepoint(value));
    if (codepoint_count(value) <= 6) {
        out.append("#####");
    } else {
        out.append("####");
        out.append(last_codepoint(value));
    }
    return out;
}

} // namespace dcc
