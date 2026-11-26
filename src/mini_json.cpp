#include "mini_json.h"
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace {
    struct Parser {
        const std::string& s;
        size_t i = 0;
        std::string err;

        void ws() { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; }
        bool take(char c) { ws(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }
        void fail(const char* m) { if (err.empty()) err = std::string(m) + " at byte " + std::to_string(i); }

        bool parse(JsonValue& v) {
            ws();
            if (i >= s.size()) { fail("unexpected end"); return false; }
            char c = s[i];
            if (c == '{') return object(v);
            if (c == '[') return array(v);
            if (c == '"') { v.type = JsonValue::String; return string(v.text); }
            if (c == 't') { v.type = JsonValue::Bool; v.boolean = true; return literal("true"); }
            if (c == 'f') { v.type = JsonValue::Bool; v.boolean = false; return literal("false"); }
            if (c == 'n') { v.type = JsonValue::Null; return literal("null"); }
            if (c == '-' || std::isdigit((unsigned char)c)) return number(v);
            fail("invalid value"); return false;
        }

        bool literal(const char* word) {
            size_t n = std::strlen(word);
            if (s.compare(i, n, word) != 0) { fail("invalid literal"); return false; }
            i += n; return true;
        }

        bool number(JsonValue& v) {
            size_t start = i;
            if (i < s.size() && s[i] == '-') ++i;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
            if (i < s.size() && s[i] == '.') {
                ++i;
                while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
            }
            if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
                ++i;
                if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
                while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
            }
            if (i == start) { fail("invalid number"); return false; }
            v.type = JsonValue::Number;
            v.number = std::strtod(s.substr(start, i - start).c_str(), nullptr);
            return true;
        }

        static void append_utf8(std::string& out, unsigned cp) {
            if (cp < 0x80) out.push_back((char)cp);
            else if (cp < 0x800) {
                out.push_back((char)(0xC0 | (cp >> 6)));
                out.push_back((char)(0x80 | (cp & 0x3F)));
            } else {
                out.push_back((char)(0xE0 | (cp >> 12)));
                out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back((char)(0x80 | (cp & 0x3F)));
            }
        }

        bool hex4(unsigned& value) {
            value = 0;
            for (int k = 0; k < 4; ++k) {
                if (i >= s.size()) return false;
                char c = s[i++];
                unsigned d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else return false;
                value = (value << 4) | d;
            }
            return true;
        }

        bool string(std::string& out) {
            if (!take('"')) { fail("expected string"); return false; }
            out.clear();
            while (i < s.size()) {
                unsigned char c = (unsigned char)s[i++];
                if (c == '"') return true;
                if (c == '\\') {
                    if (i >= s.size()) break;
                    char e = s[i++];
                    switch (e) {
                        case '"': out.push_back('"'); break;
                        case '\\': out.push_back('\\'); break;
                        case '/': out.push_back('/'); break;
                        case 'b': out.push_back('\b'); break;
                        case 'f': out.push_back('\f'); break;
                        case 'n': out.push_back('\n'); break;
                        case 'r': out.push_back('\r'); break;
                        case 't': out.push_back('\t'); break;
                        case 'u': {
                            unsigned cp;
                            if (!hex4(cp)) { fail("bad unicode escape"); return false; }
                            append_utf8(out, cp);
                            break;
                        }
                        default: fail("bad escape"); return false;
                    }
                } else {
                    out.push_back((char)c);
                }
            }
            fail("unterminated string"); return false;
        }

        bool array(JsonValue& v) {
            if (!take('[')) return false;
            v.type = JsonValue::Array;
            ws();
            if (take(']')) return true;
            while (true) {
                JsonValue child;
                if (!parse(child)) return false;
                v.array.push_back(std::move(child));
                if (take(']')) return true;
                if (!take(',')) { fail("expected ',' or ']'"); return false; }
            }
        }

        bool object(JsonValue& v) {
            if (!take('{')) return false;
            v.type = JsonValue::Object;
            ws();
            if (take('}')) return true;
            while (true) {
                std::string key;
                if (!string(key)) return false;
                if (!take(':')) { fail("expected ':'"); return false; }
                JsonValue child;
                if (!parse(child)) return false;
                v.object.emplace_back(std::move(key), std::move(child));
                if (take('}')) return true;
                if (!take(',')) { fail("expected ',' or '}'"); return false; }
            }
        }
    };
}

bool json_parse(const std::string& input, JsonValue& out, std::string& error) {
    Parser p{input, 0, ""};
    if (!p.parse(out)) { error = p.err; return false; }
    p.ws();
    if (p.i != input.size()) { error = "trailing data at byte " + std::to_string(p.i); return false; }
    return true;
}

std::string json_quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 0xF]);
                } else out.push_back((char)c);
        }
    }
    out.push_back('"');
    return out;
}
