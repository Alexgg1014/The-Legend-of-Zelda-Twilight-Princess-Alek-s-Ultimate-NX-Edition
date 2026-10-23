#pragma once

// A small JSON value with a strict parser (depth and size limits) and a serializer. Enough for the
// Firestore and Firebase Auth REST payloads and the mod's own records; kept here instead of a
// third-party header so the package imports nothing beyond libc/libstdc++ basics.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace crosssave::json {

class Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool value) : m_type{Type::Bool}, m_bool{value} {}
    Value(int value) : Value(static_cast<double>(value)) {}
    Value(int64_t value) : Value(static_cast<double>(value)) {}
    Value(double value) : m_type{Type::Number}, m_number{value} {}
    Value(const char* value) : m_type{Type::String}, m_string{value} {}
    Value(std::string value) : m_type{Type::String}, m_string{std::move(value)} {}
    Value(std::string_view value) : m_type{Type::String}, m_string{value} {}
    Value(Array value);
    Value(Object value);

    Type type() const { return m_type; }
    bool is_null() const { return m_type == Type::Null; }
    bool is_bool() const { return m_type == Type::Bool; }
    bool is_number() const { return m_type == Type::Number; }
    bool is_string() const { return m_type == Type::String; }
    bool is_array() const { return m_type == Type::Array; }
    bool is_object() const { return m_type == Type::Object; }

    bool as_bool(bool fallback = false) const { return is_bool() ? m_bool : fallback; }
    double as_number(double fallback = 0) const { return is_number() ? m_number : fallback; }
    const std::string& as_string() const;
    const Array& as_array() const;
    const Object& as_object() const;
    Array& array();
    Object& object();

    // Object member or a shared null.
    const Value& operator[](std::string_view key) const;
    // Object member, created (the value becomes an object if it was null).
    Value& operator[](const std::string& key);
    bool contains(std::string_view key) const;

    std::string string_or(std::string_view key, std::string fallback = {}) const;

private:
    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0;
    std::string m_string;
    std::shared_ptr<Array> m_array;
    std::shared_ptr<Object> m_object;
};

// nullopt on any syntax error, trailing data, depth > 64 or input > maxBytes.
std::optional<Value> parse(std::string_view text, size_t maxBytes = 8u * 1024u * 1024u);
std::string dump(const Value& value);

}  // namespace crosssave::json
