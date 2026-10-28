// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

#include "core/json_parser.h"

#include <cctype>
#include <cstdlib>

#include "core/normalize.h"

namespace ccap {

const JsonValue* JsonValue::get(const std::string& key) const {
    if (!isObject()) return nullptr;
    for (const auto& kv : object) {
        if (kv.first == key) return kv.second.get();
    }
    return nullptr;
}

std::vector<std::string> JsonValue::asStringArray() const {
    std::vector<std::string> out;
    if (isArray()) {
        for (const auto& v : array) {
            if (v && v->isString()) out.push_back(v->string);
            else if (v && v->isNumber()) {
                out.push_back(std::to_string(static_cast<long long>(v->number)));
            }
        }
    } else if (isString()) {
        // A single string -> split on whitespace (so a configuration file can
        // just say "gcc g++ clang")
        std::string cur;
        for (char c : string) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) out.push_back(cur);
    }
    return out;
}

namespace {

// Recursive descent parser
class Parser {
public:
    Parser(const std::string& text) : s_(text) {}

    JsonPtr parse(std::string* err) {
        skipWs();
        JsonPtr v = parseValue(err);
        if (!v) return nullptr;
        skipWs();
        if (i_ != s_.size()) {
            if (err) *err = "trailing content at offset " + std::to_string(i_);
            return nullptr;
        }
        return v;
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    void skipWs() {
        while (i_ < s_.size() &&
               std::isspace(static_cast<unsigned char>(s_[i_]))) {
            ++i_;
        }
    }

    bool eat(char c) {
        skipWs();
        if (i_ < s_.size() && s_[i_] == c) { ++i_; return true; }
        return false;
    }

    bool literal(const char* lit) {
        size_t n = std::char_traits<char>::length(lit);
        if (s_.compare(i_, n, lit) == 0) { i_ += n; return true; }
        return false;
    }

    bool parseStringRaw(std::string* out, std::string* err) {
        if (i_ >= s_.size() || s_[i_] != '"') {
            if (err) *err = "expected string at offset " + std::to_string(i_);
            return false;
        }
        ++i_;
        out->clear();
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (i_ >= s_.size()) break;
                char e = s_[i_++];
                switch (e) {
                    case 'n': *out += '\n'; break;
                    case 't': *out += '\t'; break;
                    case 'r': *out += '\r'; break;
                    case 'b': *out += '\b'; break;
                    case 'f': *out += '\f'; break;
                    case '/': *out += '/'; break;
                    case '"': *out += '"'; break;
                    case '\\': *out += '\\'; break;
                    case 'u': {
                        if (i_ + 4 > s_.size()) {
                            if (err) *err = "bad \\u escape";
                            return false;
                        }
                        unsigned cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = s_[i_ + k];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                            else {
                                if (err) *err = "bad \\u escape";
                                return false;
                            }
                        }
                        i_ += 4;
                        // UTF-8 encoding
                        if (cp < 0x80) {
                            *out += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            *out += static_cast<char>(0xC0 | (cp >> 6));
                            *out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            *out += static_cast<char>(0xE0 | (cp >> 12));
                            *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            *out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: *out += e; break;
                }
                continue;
            }
            *out += c;
        }
        if (err) *err = "unterminated string";
        return false;
    }

    JsonPtr parseValue(std::string* err) {
        skipWs();
        if (i_ >= s_.size()) {
            if (err) *err = "unexpected end of input";
            return nullptr;
        }

        char c = s_[i_];

        if (c == '"') {
            auto v = std::make_shared<JsonValue>();
            v->type = JsonValue::Type::String;
            if (!parseStringRaw(&v->string, err)) return nullptr;
            return v;
        }

        if (c == '{') {
            ++i_;
            auto v = std::make_shared<JsonValue>();
            v->type = JsonValue::Type::Object;
            skipWs();
            if (eat('}')) return v;
            for (;;) {
                skipWs();
                std::string key;
                if (!parseStringRaw(&key, err)) return nullptr;
                if (!eat(':')) {
                    if (err) *err = "expected ':' at offset " + std::to_string(i_);
                    return nullptr;
                }
                JsonPtr val = parseValue(err);
                if (!val) return nullptr;
                v->object.emplace_back(key, val);
                if (eat(',')) continue;
                if (eat('}')) break;
                if (err) *err = "expected ',' or '}' at offset " + std::to_string(i_);
                return nullptr;
            }
            return v;
        }

        if (c == '[') {
            ++i_;
            auto v = std::make_shared<JsonValue>();
            v->type = JsonValue::Type::Array;
            skipWs();
            if (eat(']')) return v;
            for (;;) {
                JsonPtr item = parseValue(err);
                if (!item) return nullptr;
                v->array.push_back(item);
                if (eat(',')) continue;
                if (eat(']')) break;
                if (err) *err = "expected ',' or ']' at offset " + std::to_string(i_);
                return nullptr;
            }
            return v;
        }

        if (literal("true")) {
            auto v = std::make_shared<JsonValue>();
            v->type = JsonValue::Type::Bool;
            v->boolean = true;
            return v;
        }
        if (literal("false")) {
            auto v = std::make_shared<JsonValue>();
            v->type = JsonValue::Type::Bool;
            v->boolean = false;
            return v;
        }
        if (literal("null")) {
            auto v = std::make_shared<JsonValue>();
            v->type = JsonValue::Type::Null;
            return v;
        }

        // number
        {
            size_t start = i_;
            if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
            bool any = false;
            while (i_ < s_.size() &&
                   (std::isdigit(static_cast<unsigned char>(s_[i_])) ||
                    s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E' ||
                    s_[i_] == '-' || s_[i_] == '+')) {
                any = true;
                ++i_;
            }
            if (any) {
                auto v = std::make_shared<JsonValue>();
                v->type = JsonValue::Type::Number;
                v->number = std::strtod(s_.substr(start, i_ - start).c_str(), nullptr);
                return v;
            }
        }

        if (err) *err = "unexpected character '" + std::string(1, c) +
                        "' at offset " + std::to_string(i_);
        return nullptr;
    }
};

}  // namespace

JsonPtr jsonParse(const std::string& text, std::string* err) {
    Parser p(text);
    return p.parse(err);
}

JsonPtr jsonParseFile(const std::string& path, std::string* err) {
    std::string content;
    if (!readFile(path, &content)) {
        if (err) *err = "cannot read " + path;
        return nullptr;
    }
    return jsonParse(content, err);
}

}  // namespace ccap
