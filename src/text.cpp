#include "text.hpp"

namespace zchat::text {

namespace {

    bool is_continuation(unsigned char c) {
        return (c & 0xC0) == 0x80;
    }

    // Decodes the code point at s[i], returning its byte length, or 0 if the sequence is invalid.
    std::size_t decode(std::string_view s, std::size_t i, char32_t& cp) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t len = 0;
        if (c < 0x80) {
            cp = c;
            return 1;
        }
        if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07;
        } else {
            return 0;
        }
        if (i + len > s.size()) {
            return 0;
        }
        for (std::size_t k = 1; k < len; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if (!is_continuation(cc)) {
                return 0;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        // Reject overlong encodings, surrogates and out-of-range values.
        static constexpr char32_t min_for_len[] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < min_for_len[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return 0;
        }
        return len;
    }

    bool is_control(char32_t cp) {
        return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F);
    }

} // namespace

std::string sanitize(std::string_view in, std::size_t max_bytes) {
    std::string out;
    out.reserve(in.size() < max_bytes ? in.size() : max_bytes);
    std::size_t i = 0;
    while (i < in.size()) {
        char32_t cp = 0;
        std::size_t len = decode(in, i, cp);
        std::string_view piece;
        if (len == 0) {
            piece = "?";
            len = 1;
        } else if (cp == '\t') {
            piece = " ";
        } else if (is_control(cp)) {
            i += len;
            continue;
        } else {
            piece = in.substr(i, len);
        }
        if (out.size() + piece.size() > max_bytes) {
            break;
        }
        out += piece;
        i += len;
    }
    return out;
}

std::string_view tail(std::string_view s, std::size_t max_chars) {
    std::size_t chars = 0;
    std::size_t i = s.size();
    while (i > 0) {
        std::size_t start = i - 1;
        while (start > 0 && is_continuation(static_cast<unsigned char>(s[start]))) {
            --start;
        }
        if (chars == max_chars) {
            break;
        }
        ++chars;
        i = start;
    }
    return s.substr(i);
}

void pop_back(std::string& s) {
    while (!s.empty()) {
        const auto c = static_cast<unsigned char>(s.back());
        s.pop_back();
        if (!is_continuation(c)) {
            break;
        }
    }
}

void append(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

} // namespace zchat::text
