#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace crosssave::json {
namespace {

const Value& null_value() {
    static const Value value;
    return value;
}

const std::string& empty_string() {
    static const std::string value;
    return value;
}

class Parser {
public:
    explicit Parser(std::string_view text) : m_text{text} {}

    std::optional<Value> document() {
        auto value = parse_value(0);
        skip_space();
        if (!value || m_pos != m_text.size()) {
            return std::nullopt;
        }
        return value;
    }

private:
    static constexpr int kMaxDepth = 64;

    void skip_space() {
        while (m_pos < m_text.size() &&
               (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' || m_text[m_pos] == '\n' ||
                   m_text[m_pos] == '\r'))
        {
            ++m_pos;
        }
    }

    bool literal(std::string_view word) {
        if (m_text.substr(m_pos, word.size()) != word) {
            return false;
        }
        m_pos += word.size();
        return true;
    }

    std::optional<Value> parse_value(int depth) {
        if (depth > kMaxDepth) {
            return std::nullopt;
        }
        skip_space();
        if (m_pos >= m_text.size()) {
            return std::nullopt;
        }
        switch (m_text[m_pos]) {
        case 'n':
            return literal("null") ? std::optional<Value>{Value{}} : std::nullopt;
        case 't':
            return literal("true") ? std::optional<Value>{Value{true}} : std::nullopt;
        case 'f':
            return literal("false") ? std::optional<Value>{Value{false}} : std::nullopt;
        case '"': {
            auto text = parse_string();
            return text ? std::optional<Value>{Value{std::move(*text)}} : std::nullopt;
        }
        case '[':
            return parse_array(depth);
        case '{':
            return parse_object(depth);
        default:
            return parse_number();
        }
    }

    static void append_utf8(std::string& out, uint32_t code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }

    std::optional<uint32_t> hex4() {
        if (m_pos + 4 > m_text.size()) {
            return std::nullopt;
        }
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_text[m_pos++];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<uint32_t>(c - 'A' + 10);
            } else {
                return std::nullopt;
            }
        }
        return value;
    }

    std::optional<std::string> parse_string() {
        ++m_pos;  // opening quote
        std::string out;
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos++];
            if (c == '"') {
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return std::nullopt;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (m_pos >= m_text.size()) {
                return std::nullopt;
            }
            switch (m_text[m_pos++]) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                auto code = hex4();
                if (!code) {
                    return std::nullopt;
                }
                if (*code >= 0xd800 && *code < 0xdc00) {
                    if (!literal("\\u")) {
                        return std::nullopt;
                    }
                    auto low = hex4();
                    if (!low || *low < 0xdc00 || *low >= 0xe000) {
                        return std::nullopt;
                    }
                    *code = 0x10000 + ((*code - 0xd800) << 10) + (*low - 0xdc00);
                } else if (*code >= 0xdc00 && *code < 0xe000) {
                    return std::nullopt;
                }
                append_utf8(out, *code);
                break;
            }
            default:
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<Value> parse_number() {
        const size_t start = m_pos;
        if (m_pos < m_text.size() && m_text[m_pos] == '-') {
            ++m_pos;
        }
        const auto digits = [this] {
            const size_t begin = m_pos;
            while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9') {
                ++m_pos;
            }
            return m_pos - begin;
        };
        const size_t intStart = m_pos;
        const size_t intDigits = digits();
        if (intDigits == 0 || (intDigits > 1 && m_text[intStart] == '0')) {
            return std::nullopt;
        }
        if (m_pos < m_text.size() && m_text[m_pos] == '.') {
            ++m_pos;
            if (digits() == 0) {
                return std::nullopt;
            }
        }
        if (m_pos < m_text.size() && (m_text[m_pos] == 'e' || m_text[m_pos] == 'E')) {
            ++m_pos;
            if (m_pos < m_text.size() && (m_text[m_pos] == '+' || m_text[m_pos] == '-')) {
                ++m_pos;
            }
            if (digits() == 0) {
                return std::nullopt;
            }
        }
        const std::string number{m_text.substr(start, m_pos - start)};
        char* end = nullptr;
        const double value = std::strtod(number.c_str(), &end);
        if (end != number.c_str() + number.size() || !std::isfinite(value)) {
            return std::nullopt;
        }
        return Value{value};
    }

    std::optional<Value> parse_array(int depth) {
        ++m_pos;
        Array items;
        skip_space();
        if (m_pos < m_text.size() && m_text[m_pos] == ']') {
            ++m_pos;
            return Value{std::move(items)};
        }
        while (true) {
            auto item = parse_value(depth + 1);
            if (!item) {
                return std::nullopt;
            }
            items.push_back(std::move(*item));
            skip_space();
            if (m_pos >= m_text.size()) {
                return std::nullopt;
            }
            if (m_text[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_text[m_pos] == ']') {
                ++m_pos;
                return Value{std::move(items)};
            }
            return std::nullopt;
        }
    }

    std::optional<Value> parse_object(int depth) {
        ++m_pos;
        Object members;
        skip_space();
        if (m_pos < m_text.size() && m_text[m_pos] == '}') {
            ++m_pos;
            return Value{std::move(members)};
        }
        while (true) {
            skip_space();
            if (m_pos >= m_text.size() || m_text[m_pos] != '"') {
                return std::nullopt;
            }
            auto key = parse_string();
            skip_space();
            if (!key || m_pos >= m_text.size() || m_text[m_pos] != ':') {
                return std::nullopt;
            }
            ++m_pos;
            auto item = parse_value(depth + 1);
            if (!item) {
                return std::nullopt;
            }
            members[std::move(*key)] = std::move(*item);
            skip_space();
            if (m_pos >= m_text.size()) {
                return std::nullopt;
            }
            if (m_text[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_text[m_pos] == '}') {
                ++m_pos;
                return Value{std::move(members)};
            }
            return std::nullopt;
        }
    }

    std::string_view m_text;
    size_t m_pos = 0;
};

void dump_string(std::string& out, const std::string& text) {
    static constexpr char kDigits[] = "0123456789abcdef";
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += "\\u00";
                out.push_back(kDigits[(c >> 4) & 15]);
                out.push_back(kDigits[c & 15]);
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

void dump_value(std::string& out, const Value& value) {
    switch (value.type()) {
    case Value::Type::Null:
        out += "null";
        return;
    case Value::Type::Bool:
        out += value.as_bool() ? "true" : "false";
        return;
    case Value::Type::Number: {
        const double number = value.as_number();
        char buffer[64];
        if (std::trunc(number) == number && std::fabs(number) < 9007199254740992.0) {
            std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(number));
        } else {
            std::snprintf(buffer, sizeof(buffer), "%.17g", number);
        }
        out += buffer;
        return;
    }
    case Value::Type::String:
        dump_string(out, value.as_string());
        return;
    case Value::Type::Array: {
        out.push_back('[');
        bool first = true;
        for (const auto& item : value.as_array()) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            dump_value(out, item);
        }
        out.push_back(']');
        return;
    }
    case Value::Type::Object: {
        out.push_back('{');
        bool first = true;
        for (const auto& [key, item] : value.as_object()) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            dump_string(out, key);
            out.push_back(':');
            dump_value(out, item);
        }
        out.push_back('}');
        return;
    }
    }
}

}  // namespace

Value::Value(Array value)
    : m_type{Type::Array}, m_array{std::make_shared<Array>(std::move(value))} {}

Value::Value(Object value)
    : m_type{Type::Object}, m_object{std::make_shared<Object>(std::move(value))} {}

const std::string& Value::as_string() const {
    return is_string() ? m_string : empty_string();
}

const Array& Value::as_array() const {
    static const Array empty;
    return is_array() ? *m_array : empty;
}

const Object& Value::as_object() const {
    static const Object empty;
    return is_object() ? *m_object : empty;
}

Array& Value::array() {
    if (!is_array()) {
        *this = Value{Array{}};
    } else if (m_array.use_count() > 1) {
        m_array = std::make_shared<Array>(*m_array);
    }
    return *m_array;
}

Object& Value::object() {
    if (!is_object()) {
        *this = Value{Object{}};
    } else if (m_object.use_count() > 1) {
        m_object = std::make_shared<Object>(*m_object);
    }
    return *m_object;
}

const Value& Value::operator[](std::string_view key) const {
    if (!is_object()) {
        return null_value();
    }
    const auto it = m_object->find(std::string{key});
    return it != m_object->end() ? it->second : null_value();
}

Value& Value::operator[](const std::string& key) {
    return object()[key];
}

bool Value::contains(std::string_view key) const {
    return is_object() && m_object->contains(std::string{key});
}

std::string Value::string_or(std::string_view key, std::string fallback) const {
    const auto& item = (*this)[key];
    return item.is_string() ? item.as_string() : std::move(fallback);
}

std::optional<Value> parse(std::string_view text, size_t maxBytes) {
    if (text.size() > maxBytes) {
        return std::nullopt;
    }
    return Parser{text}.document();
}

std::string dump(const Value& value) {
    std::string out;
    dump_value(out, value);
    return out;
}

}  // namespace crosssave::json
