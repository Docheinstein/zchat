#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zchat::json {

// A JSON value, as parsed by parse(). Reading it never fails: asking an object for a member it does not have, an
// array for an index past its end, or a string for its number gives a null value, an empty string or a fallback, so
// that untrusted JSON can be read without checking every step.
struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    // The elements of an array, or the values of an object's members, in order.
    std::vector<Value> items;
    // The names of an object's members, one for each of items.
    std::vector<std::string> keys;

    bool is_null() const {
        return type == Type::Null;
    }
    bool is_bool() const {
        return type == Type::Bool;
    }
    bool is_number() const {
        return type == Type::Number;
    }
    bool is_string() const {
        return type == Type::String;
    }
    bool is_array() const {
        return type == Type::Array;
    }
    bool is_object() const {
        return type == Type::Object;
    }

    // An object's member (the last one, if the name is repeated), or nullptr.
    const Value* find(std::string_view key) const;
    // An object's member, or a null value.
    const Value& operator[](std::string_view key) const;
    // An array's element, or a null value.
    const Value& operator[](std::size_t index) const;
    // The number of elements of an array or members of an object; 0 for anything else.
    std::size_t size() const {
        return items.size();
    }

    // A string's text; empty for anything else.
    std::string_view str() const;
    // A number, rounded to the nearest int and clamped to int's range; fallback for anything else.
    int integer(int fallback = 0) const;
    // Whether it is true as JavaScript sees it: not null, false, 0, nor "".
    bool truthy() const;
};

// Parses a JSON text (RFC 8259; \u escapes, surrogate pairs included, become UTF-8). Returns nullopt when it is not
// valid JSON, or nests deeper than max_depth arrays and objects.
std::optional<Value> parse(std::string_view text, int max_depth = 64);

// A JSON string literal for text, quotes included: quotes, backslashes and control characters are escaped, and so
// are U+2028 and U+2029 (which JavaScript source does not allow in strings). Invalid UTF-8 becomes U+FFFD.
std::string quote(std::string_view text);

// What follows is the implementation, kept in this header so that using it needs no other source file.

namespace detail {

    inline const Value null_value {};

    inline void append_utf8(std::string& out, char32_t cp) {
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

    // Decodes the UTF-8 sequence at s[i] into cp, returning its length, or 0 when it is not valid UTF-8 (overlong
    // encodings and surrogates included).
    inline std::size_t decode_utf8(std::string_view s, std::size_t i, char32_t& cp) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            cp = c;
            return 1;
        }
        std::size_t len = 0;
        char32_t min = 0;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1F;
            min = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0F;
            min = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07;
            min = 0x10000;
        } else {
            return 0;
        }
        if (i + len > s.size()) {
            return 0;
        }
        for (std::size_t k = 1; k < len; ++k) {
            const auto next = static_cast<unsigned char>(s[i + k]);
            if ((next & 0xC0) != 0x80) {
                return 0;
            }
            cp = (cp << 6) | (next & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return 0;
        }
        return len;
    }

    // A recursive descent parser over the whole text, which fails at the first thing that is not JSON.
    class Parser {
    public:
        Parser(std::string_view text, int max_depth) :
            text_(text),
            max_depth_(max_depth) {
        }

        std::optional<Value> document() {
            Value value;
            if (!parse_value(value, 0)) {
                return std::nullopt;
            }
            skip_space();
            if (pos_ != text_.size()) {
                return std::nullopt;
            }
            return value;
        }

    private:
        void skip_space() {
            while (pos_ < text_.size() &&
                   (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) {
                ++pos_;
            }
        }

        bool literal(std::string_view word) {
            if (text_.substr(pos_, word.size()) != word) {
                return false;
            }
            pos_ += word.size();
            return true;
        }

        bool parse_value(Value& value, int depth) {
            skip_space();
            if (pos_ >= text_.size()) {
                return false;
            }
            switch (text_[pos_]) {
            case '{':
                return parse_object(value, depth + 1);
            case '[':
                return parse_array(value, depth + 1);
            case '"':
                value.type = Value::Type::String;
                return parse_string(value.string);
            case 't':
                value.type = Value::Type::Bool;
                value.boolean = true;
                return literal("true");
            case 'f':
                value.type = Value::Type::Bool;
                return literal("false");
            case 'n':
                return literal("null");
            default:
                return parse_number(value);
            }
        }

        bool parse_object(Value& value, int depth) {
            if (depth > max_depth_) {
                return false;
            }
            value.type = Value::Type::Object;
            ++pos_;
            skip_space();
            if (pos_ < text_.size() && text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            while (true) {
                skip_space();
                std::string key;
                if (pos_ >= text_.size() || text_[pos_] != '"' || !parse_string(key)) {
                    return false;
                }
                skip_space();
                if (pos_ >= text_.size() || text_[pos_] != ':') {
                    return false;
                }
                ++pos_;
                Value member;
                if (!parse_value(member, depth)) {
                    return false;
                }
                value.keys.push_back(std::move(key));
                value.items.push_back(std::move(member));
                skip_space();
                if (pos_ < text_.size() && text_[pos_] == ',') {
                    ++pos_;
                } else if (pos_ < text_.size() && text_[pos_] == '}') {
                    ++pos_;
                    return true;
                } else {
                    return false;
                }
            }
        }

        bool parse_array(Value& value, int depth) {
            if (depth > max_depth_) {
                return false;
            }
            value.type = Value::Type::Array;
            ++pos_;
            skip_space();
            if (pos_ < text_.size() && text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            while (true) {
                Value element;
                if (!parse_value(element, depth)) {
                    return false;
                }
                value.items.push_back(std::move(element));
                skip_space();
                if (pos_ < text_.size() && text_[pos_] == ',') {
                    ++pos_;
                } else if (pos_ < text_.size() && text_[pos_] == ']') {
                    ++pos_;
                    return true;
                } else {
                    return false;
                }
            }
        }

        // Four hex digits after a \u.
        bool parse_hex4(char32_t& unit) {
            if (pos_ + 4 > text_.size()) {
                return false;
            }
            unsigned value = 0;
            const auto [ptr, ec] = std::from_chars(text_.data() + pos_, text_.data() + pos_ + 4, value, 16);
            if (ec != std::errc {} || ptr != text_.data() + pos_ + 4) {
                return false;
            }
            pos_ += 4;
            unit = value;
            return true;
        }

        bool parse_string(std::string& out) {
            ++pos_;
            while (pos_ < text_.size()) {
                const char c = text_[pos_];
                if (c == '"') {
                    ++pos_;
                    return true;
                }
                if (static_cast<unsigned char>(c) < 0x20) {
                    return false;
                }
                if (c != '\\') {
                    // Bytes are kept as they are; quote() deals with invalid UTF-8 when writing them out.
                    out += c;
                    ++pos_;
                    continue;
                }
                if (++pos_ >= text_.size()) {
                    return false;
                }
                const char escape = text_[pos_++];
                switch (escape) {
                case '"':
                case '\\':
                case '/':
                    out += escape;
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    char32_t unit = 0;
                    if (!parse_hex4(unit)) {
                        return false;
                    }
                    if (unit >= 0xD800 && unit <= 0xDBFF) {
                        // A high surrogate makes a code point with the low surrogate after it; alone, it is not one.
                        char32_t low = 0;
                        const std::size_t before = pos_;
                        if (text_.substr(pos_, 2) == "\\u" && (pos_ += 2, parse_hex4(low)) && low >= 0xDC00 &&
                            low <= 0xDFFF) {
                            unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            pos_ = before;
                            unit = 0xFFFD;
                        }
                    } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
                        unit = 0xFFFD;
                    }
                    append_utf8(out, unit);
                    break;
                }
                default:
                    return false;
                }
            }
            return false;
        }

        bool parse_number(Value& value) {
            // The grammar: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
            const std::size_t start = pos_;
            const auto digits = [this] {
                const std::size_t from = pos_;
                while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                    ++pos_;
                }
                return pos_ - from;
            };
            if (pos_ < text_.size() && text_[pos_] == '-') {
                ++pos_;
            }
            if (pos_ < text_.size() && text_[pos_] == '0') {
                ++pos_;
            } else if (digits() == 0) {
                return false;
            }
            if (pos_ < text_.size() && text_[pos_] == '.') {
                ++pos_;
                if (digits() == 0) {
                    return false;
                }
            }
            if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
                ++pos_;
                if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
                    ++pos_;
                }
                if (digits() == 0) {
                    return false;
                }
            }
            double number = 0;
            const auto [ptr, ec] = std::from_chars(text_.data() + start, text_.data() + pos_, number);
            if (ptr != text_.data() + pos_ || (ec != std::errc {} && ec != std::errc::result_out_of_range)) {
                return false;
            }
            value.type = Value::Type::Number;
            value.number = std::isfinite(number) ? number : 0;
            return true;
        }

        std::string_view text_;
        int max_depth_;
        std::size_t pos_ = 0;
    };

} // namespace detail

inline const Value* Value::find(std::string_view key) const {
    if (type != Type::Object) {
        return nullptr;
    }
    for (std::size_t i = keys.size(); i-- > 0;) {
        if (keys[i] == key) {
            return &items[i];
        }
    }
    return nullptr;
}

inline const Value& Value::operator[](std::string_view key) const {
    const Value* member = find(key);
    return member ? *member : detail::null_value;
}

inline const Value& Value::operator[](std::size_t index) const {
    return type == Type::Array && index < items.size() ? items[index] : detail::null_value;
}

inline std::string_view Value::str() const {
    return type == Type::String ? std::string_view(string) : std::string_view();
}

inline int Value::integer(int fallback) const {
    if (type != Type::Number) {
        return fallback;
    }
    const double rounded = std::round(number);
    if (rounded >= static_cast<double>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    if (rounded <= static_cast<double>(std::numeric_limits<int>::min())) {
        return std::numeric_limits<int>::min();
    }
    return static_cast<int>(rounded);
}

inline bool Value::truthy() const {
    switch (type) {
    case Type::Null:
        return false;
    case Type::Bool:
        return boolean;
    case Type::Number:
        return number != 0;
    case Type::String:
        return !string.empty();
    default:
        return true;
    }
}

inline std::optional<Value> parse(std::string_view text, int max_depth) {
    return detail::Parser(text, max_depth).document();
}

inline std::string quote(std::string_view text) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(text.size() + 2);
    out += '"';
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
            ++i;
        } else if (c == '\n') {
            out += "\\n";
            ++i;
        } else if (c == '\r') {
            out += "\\r";
            ++i;
        } else if (c == '\t') {
            out += "\\t";
            ++i;
        } else if (c < 0x20 || c == 0x7F) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 0xF];
            ++i;
        } else if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
        } else {
            char32_t cp = 0;
            const std::size_t len = detail::decode_utf8(text, i, cp);
            if (len == 0) {
                out += "\xEF\xBF\xBD";
                ++i;
            } else if (cp == 0x2028 || cp == 0x2029) {
                out += cp == 0x2028 ? "\\u2028" : "\\u2029";
                i += len;
            } else {
                out.append(text.substr(i, len));
                i += len;
            }
        }
    }
    out += '"';
    return out;
}

} // namespace zchat::json
