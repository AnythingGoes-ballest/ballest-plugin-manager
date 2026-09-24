#include "json.hpp"

#include <cstdlib>

namespace json {
namespace {

struct Reader {
    const std::string& s;
    size_t i = 0;
    std::string error;

    void Space() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool Fail(const std::string& what) {
        if (error.empty()) error = what + " at byte " + std::to_string(i);
        return false;
    }
    bool Literal(const char* word) {
        const std::string w(word);
        if (s.compare(i, w.size(), w) != 0) return Fail("expected " + w);
        i += w.size();
        return true;
    }

    static void Utf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    bool Hex4(unsigned& code) {
        if (i + 4 > s.size()) return Fail("short \\u escape");
        code = static_cast<unsigned>(std::strtoul(s.substr(i, 4).c_str(), nullptr, 16));
        i += 4;
        return true;
    }

    bool String(std::string& out) {
        if (s[i] != '"') return Fail("expected a string");
        ++i;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) break;
            switch (c = s[i++]) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned code = 0;
                    if (!Hex4(code)) return false;
                    // A surrogate pair is one character.
                    if (code >= 0xD800 && code < 0xDC00 && s.compare(i, 2, "\\u") == 0) {
                        i += 2;
                        unsigned low = 0;
                        if (!Hex4(low)) return false;
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                    Utf8(out, code);
                    break;
                }
                default: out += c;              // \" \\ \/
            }
        }
        if (i >= s.size()) return Fail("unterminated string");
        ++i;
        return true;
    }

    bool Any(json::Value& v, int depth) {
        if (depth > 64) return Fail("nested too deeply");
        Space();
        if (i >= s.size()) return Fail("unexpected end");
        const char c = s[i];
        if (c == '{') {
            v.type = json::Value::Object;
            ++i;
            Space();
            if (i < s.size() && s[i] == '}') return ++i, true;
            for (;;) {
                Space();
                std::string key;
                if (i >= s.size() || !String(key)) return Fail("expected a key");
                Space();
                if (i >= s.size() || s[i] != ':') return Fail("expected ':'");
                ++i;
                json::Value member;
                if (!Any(member, depth + 1)) return false;
                v.members.emplace_back(std::move(key), std::move(member));
                Space();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') return ++i, true;
                return Fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.type = json::Value::Array;
            ++i;
            Space();
            if (i < s.size() && s[i] == ']') return ++i, true;
            for (;;) {
                json::Value item;
                if (!Any(item, depth + 1)) return false;
                v.items.push_back(std::move(item));
                Space();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') return ++i, true;
                return Fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            v.type = json::Value::String;
            return String(v.string);
        }
        if (c == 't') return v.type = json::Value::Bool, v.boolean = true, Literal("true");
        if (c == 'f') return v.type = json::Value::Bool, v.boolean = false, Literal("false");
        if (c == 'n') return v.type = json::Value::Null, Literal("null");
        char* end = nullptr;
        v.number = std::strtod(s.c_str() + i, &end);
        if (end == s.c_str() + i) return Fail("unexpected character");
        v.type = json::Value::Number;
        i = static_cast<size_t>(end - s.c_str());
        return true;
    }
};

}  // namespace

bool Parse(const std::string& text, Value& out, std::string& error) {
    Reader r{text, 0, {}};
    out = Value{};
    if (!r.Any(out, 0)) {
        error = r.error;
        return false;
    }
    r.Space();
    if (r.i != text.size()) {
        error = "extra text after the value at byte " + std::to_string(r.i);
        return false;
    }
    return true;
}

}  // namespace json
