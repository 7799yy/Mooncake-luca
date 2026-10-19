// Test-only typed value store. Does not implement or test a JSON wire parser.
#pragma once
#include <cassert>
#include <cstdint>
#include <map>
#include <string>
namespace Json {
using UInt64 = uint64_t;
class Value {
    enum Kind { Null, String, Unsigned, Object } kind_ = Null;
    std::string string_;
    UInt64 number_ = 0;
    std::map<std::string, Value> object_;

   public:
    Value() = default;
    Value(const char* text) : kind_(String), string_(text) {}
    Value(std::string text) : kind_(String), string_(std::move(text)) {}
    Value(UInt64 number) : kind_(Unsigned), number_(number) {}
    Value& operator[](const std::string& key) {
        kind_ = Object;
        return object_[key];
    }
    bool isObject() const { return kind_ == Object; }
    bool isString() const { return kind_ == String; }
    bool isUInt64() const { return kind_ == Unsigned; }
    std::string asString() const {
        assert(isString());
        return string_;
    }
    UInt64 asUInt64() const {
        assert(isUInt64());
        return number_;
    }
    std::string toStyledString() const;
};
inline std::map<std::string, Value> values;
inline size_t sequence = 0;
inline std::string Value::toStyledString() const {
    auto key = "mock-json-" + std::to_string(++sequence);
    values[key] = *this;
    return key;
}
class Reader {
   public:
    bool parse(const std::string& text, Value& out, bool) {
        auto found = values.find(text);
        if (found == values.end()) return false;
        out = found->second;
        return true;
    }
};
}  // namespace Json
