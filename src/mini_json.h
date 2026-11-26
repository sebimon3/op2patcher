#pragma once
#include <string>
#include <utility>
#include <vector>

struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    const JsonValue* find(const std::string& key) const {
        if (type != Object) return nullptr;
        for (const auto& kv : object) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string str(const std::string& key, const std::string& fallback = "") const {
        const JsonValue* v = find(key);
        return (v && v->type == String) ? v->text : fallback;
    }
    bool boolean_value(const std::string& key, bool fallback = false) const {
        const JsonValue* v = find(key);
        return (v && v->type == Bool) ? v->boolean : fallback;
    }
};

bool json_parse(const std::string& input, JsonValue& out, std::string& error);
std::string json_quote(const std::string& s);
