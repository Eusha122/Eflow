#include "core/Json.h"

#include <cstdio>
#include <cstdlib>

namespace eflow::json {
namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    std::string error;

    void Skip() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool Fail(const char* what) {
        if (error.empty()) error = std::string(what) + " at offset " + std::to_string(i);
        return false;
    }
    bool Literal(const char* word) {
        const size_t n = std::char_traits<char>::length(word);
        if (s.compare(i, n, word) != 0) return Fail("unexpected token");
        i += n;
        return true;
    }
    static void AppendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
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
    bool Hex4(unsigned& cp) {
        if (i + 4 > s.size()) return Fail("bad \\u escape");
        cp = static_cast<unsigned>(std::strtoul(s.substr(i, 4).c_str(), nullptr, 16));
        i += 4;
        return true;
    }
    bool String(std::string& out) {
        if (s[i] != '"') return Fail("expected string");
        ++i;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) return Fail("bad escape");
            c = s[i++];
            switch (c) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && s.compare(i, 2, "\\u") == 0) {   // surrogate pair
                    i += 2;
                    unsigned lo;
                    if (!Hex4(lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                AppendUtf8(out, cp);
                break;
            }
            default: return Fail("bad escape");
            }
        }
        if (i >= s.size()) return Fail("unterminated string");
        ++i;
        return true;
    }
    bool Parse(Value& v, int depth = 0) {
        if (depth > 128) return Fail("nesting too deep");
        Skip();
        if (i >= s.size()) return Fail("unexpected end");
        const char c = s[i];
        if (c == '{') {
            v = Value::Object();
            ++i;
            Skip();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            for (;;) {
                Skip();
                std::string key;
                if (!String(key)) return false;
                Skip();
                if (i >= s.size() || s[i] != ':') return Fail("expected ':'");
                ++i;
                Value child;
                if (!Parse(child, depth + 1)) return false;
                v.members.emplace_back(std::move(key), std::move(child));
                Skip();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return Fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v = Value::Array();
            ++i;
            Skip();
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            for (;;) {
                Value child;
                if (!Parse(child, depth + 1)) return false;
                v.items.push_back(std::move(child));
                Skip();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return Fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v = Value::String(""); return String(v.text); }
        if (c == 't') { v.type = Value::Type::Bool; v.boolean = true; return Literal("true"); }
        if (c == 'f') { v.type = Value::Type::Bool; v.boolean = false; return Literal("false"); }
        if (c == 'n') { v.type = Value::Type::Null; return Literal("null"); }
        const size_t start = i;
        while (i < s.size() && (isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '+' || s[i] == '.' ||
                                s[i] == 'e' || s[i] == 'E'))
            ++i;
        if (i == start) return Fail("unexpected character");
        v = Value::Number(s.substr(start, i - start));
        return true;
    }
};

void Escape(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

void WriteValue(std::string& out, const Value& v, int indent, int level) {
    const std::string pad(static_cast<size_t>(indent) * (level + 1), ' '), close(static_cast<size_t>(indent) * level, ' ');
    switch (v.type) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.boolean ? "true" : "false"; break;
    case Value::Type::Number: out += v.text; break;
    case Value::Type::String: Escape(out, v.text); break;
    case Value::Type::Array:
        if (v.items.empty()) { out += "[]"; break; }
        out += "[\n";
        for (size_t k = 0; k < v.items.size(); ++k) {
            out += pad;
            WriteValue(out, v.items[k], indent, level + 1);
            out += k + 1 < v.items.size() ? ",\n" : "\n";
        }
        out += close + "]";
        break;
    case Value::Type::Object:
        if (v.members.empty()) { out += "{}"; break; }
        out += "{\n";
        for (size_t k = 0; k < v.members.size(); ++k) {
            out += pad;
            Escape(out, v.members[k].first);
            out += ": ";
            WriteValue(out, v.members[k].second, indent, level + 1);
            out += k + 1 < v.members.size() ? ",\n" : "\n";
        }
        out += close + "}";
        break;
    }
}

} // namespace

Value* Value::Find(const std::string& key) {
    for (auto& [k, v] : members)
        if (k == key) return &v;
    return nullptr;
}

Value& Value::Member(const std::string& key) {
    if (Value* v = Find(key)) return *v;
    members.emplace_back(key, Value{});
    return members.back().second;
}

bool Parse(const std::string& text, Value& out, std::string& error) {
    Parser p{text};
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) p.i = 3;   // UTF-8 BOM
    if (!p.Parse(out)) {
        error = p.error;
        return false;
    }
    p.Skip();
    if (p.i != text.size()) {
        error = "trailing content at offset " + std::to_string(p.i);
        return false;
    }
    return true;
}

std::string Write(const Value& v, int indent) {
    std::string out;
    WriteValue(out, v, indent, 0);
    out += '\n';
    return out;
}

} // namespace eflow::json
