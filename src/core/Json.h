#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace eflow::json {

// Minimal order-preserving JSON DOM, for editing user config files (settings.json, hooks.json)
// without reshuffling or reformatting the parts we do not touch. Numbers keep their source text.
struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    std::string text;                                        // number literal, or decoded UTF-8 string
    std::vector<Value> items;                                // array
    std::vector<std::pair<std::string, Value>> members;      // object, in source order

    static Value Object() { Value v; v.type = Type::Object; return v; }
    static Value Array() { Value v; v.type = Type::Array; return v; }
    static Value String(std::string s) { Value v; v.type = Type::String; v.text = std::move(s); return v; }
    static Value Number(std::string literal) { Value v; v.type = Type::Number; v.text = std::move(literal); return v; }

    Value* Find(const std::string& key);                     // object member, or null
    Value& Member(const std::string& key);                   // object member, created (null) if missing
};

bool Parse(const std::string& text, Value& out, std::string& error);
std::string Write(const Value& v, int indent = 2);          // pretty-printed, trailing newline

} // namespace eflow::json
