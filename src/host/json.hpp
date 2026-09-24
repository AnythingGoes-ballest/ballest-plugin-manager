// A small JSON reader for registry.json: objects, arrays, strings (with \uXXXX escapes), numbers, true, false, null.
// Reading only; anything malformed gives an error message instead of a value.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> items;                                   // Array
    std::vector<std::pair<std::string, Value>> members;         // Object, in file order

    const Value* Get(const std::string& key) const {
        for (const auto& [k, v] : members)
            if (k == key) return &v;
        return nullptr;
    }
    std::string Str(const std::string& key, const std::string& fallback = "") const {
        const Value* v = Get(key);
        return v && v->type == String ? v->string : fallback;
    }
};

// False with `error` set if the text is not valid JSON.
bool Parse(const std::string& text, Value& out, std::string& error);

}  // namespace json
