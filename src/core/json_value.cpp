#include "core/json_value.hpp"

#include <cerrno>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace kagami {

const JsonValue* JsonValue::find(const std::string& key) const {
    if (!is_object()) {
        return nullptr;
    }
    const auto it = object_value.find(key);
    return it == object_value.end() ? nullptr : &it->second;
}

std::string JsonValue::string_or(const std::string& fallback) const {
    return is_string() ? string_value : fallback;
}

bool JsonValue::bool_or(bool fallback) const {
    return is_bool() ? bool_value : fallback;
}

std::uint32_t JsonValue::u32_or(std::uint32_t fallback) const {
    if (!is_number() || number_value < 0 || number_value > std::numeric_limits<std::uint32_t>::max()) {
        return fallback;
    }
    return static_cast<std::uint32_t>(number_value);
}

class JsonParser {
public:
    explicit JsonParser(const std::string& input) : input_(input) {}

    bool parse(JsonValue& value, std::string& error) {
        skip_ws();
        if (!parse_value(value, error)) {
            return false;
        }
        skip_ws();
        if (pos_ != input_.size()) {
            error = "unexpected trailing data at byte " + std::to_string(pos_);
            return false;
        }
        return true;
    }

private:
    const std::string& input_;
    std::size_t pos_ = 0;

    void skip_ws() {
        while (pos_ < input_.size()) {
            const char c = input_[pos_];
            if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
                break;
            }
            ++pos_;
        }
    }

    bool consume(char expected) {
        skip_ws();
        if (pos_ < input_.size() && input_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool consume_literal(const char* literal) {
        const std::size_t start = pos_;
        for (const char* p = literal; *p; ++p) {
            if (pos_ >= input_.size() || input_[pos_] != *p) {
                pos_ = start;
                return false;
            }
            ++pos_;
        }
        return true;
    }

    bool parse_value(JsonValue& value, std::string& error) {
        skip_ws();
        if (pos_ >= input_.size()) {
            error = "unexpected end of JSON";
            return false;
        }
        const char c = input_[pos_];
        if (c == '{') {
            return parse_object(value, error);
        }
        if (c == '[') {
            return parse_array(value, error);
        }
        if (c == '"') {
            value.type = JsonValue::Type::String;
            return parse_string(value.string_value, error);
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return parse_number(value, error);
        }
        if (consume_literal("true")) {
            value.type = JsonValue::Type::Bool;
            value.bool_value = true;
            return true;
        }
        if (consume_literal("false")) {
            value.type = JsonValue::Type::Bool;
            value.bool_value = false;
            return true;
        }
        if (consume_literal("null")) {
            value.type = JsonValue::Type::Null;
            return true;
        }
        error = "unexpected token at byte " + std::to_string(pos_);
        return false;
    }

    bool parse_object(JsonValue& value, std::string& error) {
        if (!consume('{')) {
            error = "expected object";
            return false;
        }
        value = JsonValue{};
        value.type = JsonValue::Type::Object;
        skip_ws();
        if (consume('}')) {
            return true;
        }
        for (;;) {
            std::string key;
            if (!parse_string(key, error)) {
                return false;
            }
            if (!consume(':')) {
                error = "expected ':' after object key";
                return false;
            }
            JsonValue child;
            if (!parse_value(child, error)) {
                return false;
            }
            value.object_value[key] = child;
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                error = "expected ',' or '}' in object";
                return false;
            }
        }
    }

    bool parse_array(JsonValue& value, std::string& error) {
        if (!consume('[')) {
            error = "expected array";
            return false;
        }
        value = JsonValue{};
        value.type = JsonValue::Type::Array;
        skip_ws();
        if (consume(']')) {
            return true;
        }
        for (;;) {
            JsonValue child;
            if (!parse_value(child, error)) {
                return false;
            }
            value.array_value.push_back(child);
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                error = "expected ',' or ']' in array";
                return false;
            }
        }
    }

    bool parse_string(std::string& value, std::string& error) {
        if (!consume('"')) {
            error = "expected string at byte " + std::to_string(pos_);
            return false;
        }
        value.clear();
        while (pos_ < input_.size()) {
            const char c = input_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                value.push_back(c);
                continue;
            }
            if (pos_ >= input_.size()) {
                error = "unterminated string escape";
                return false;
            }
            const char e = input_[pos_++];
            switch (e) {
            case '"':
            case '\\':
            case '/':
                value.push_back(e);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case 'u':
                if (!parse_unicode_escape(value, error)) {
                    return false;
                }
                break;
            default:
                error = "invalid string escape";
                return false;
            }
        }
        error = "unterminated string";
        return false;
    }

    bool parse_unicode_escape(std::string& value, std::string& error) {
        if (pos_ + 4 > input_.size()) {
            error = "short unicode escape";
            return false;
        }
        unsigned code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = input_[pos_++];
            code <<= 4;
            if (c >= '0' && c <= '9') {
                code += static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                code += static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                code += static_cast<unsigned>(c - 'A' + 10);
            } else {
                error = "invalid unicode escape";
                return false;
            }
        }
        if (code <= 0x7f) {
            value.push_back(static_cast<char>(code));
        } else if (code <= 0x7ff) {
            value.push_back(static_cast<char>(0xc0 | ((code >> 6) & 0x1f)));
            value.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            value.push_back(static_cast<char>(0xe0 | ((code >> 12) & 0x0f)));
            value.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            value.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
        return true;
    }

    bool parse_number(JsonValue& value, std::string& error) {
        const std::size_t start = pos_;
        if (input_[pos_] == '-') {
            ++pos_;
        }
        while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') {
            ++pos_;
        }
        if (pos_ < input_.size() && input_[pos_] == '.') {
            ++pos_;
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') {
                ++pos_;
            }
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) {
                ++pos_;
            }
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') {
                ++pos_;
            }
        }
        const std::string token = input_.substr(start, pos_ - start);
        char* end = nullptr;
        errno = 0;
        const double parsed = std::strtod(token.c_str(), &end);
        if (errno != 0 || end == token.c_str() || *end != '\0') {
            error = "invalid number at byte " + std::to_string(start);
            return false;
        }
        value.type = JsonValue::Type::Number;
        value.number_value = parsed;
        return true;
    }
};

bool parse_json(const std::string& input, JsonValue& out, std::string& error) {
    JsonParser parser(input);
    return parser.parse(out, error);
}

} // namespace kagami
