// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

#include "core/json.h"

#include <cstdio>

namespace ccap {

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);

    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    // Emit UTF-8 bytes as-is (JSON allows this)
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string jsonQuote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    out += jsonEscape(s);
    out += '"';
    return out;
}

std::string jsonStringArray(const std::vector<std::string>& items) {
    std::string out;
    out += '[';
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += ',';
        out += jsonQuote(items[i]);
    }
    out += ']';
    return out;
}

std::string jsonKv(const std::string& key, const std::string& rawValue) {
    return jsonQuote(key) + ":" + rawValue;
}

std::string jsonKvStr(const std::string& key, const std::string& value) {
    return jsonKv(key, jsonQuote(value));
}

std::string jsonKvNum(const std::string& key, int64_t value) {
    return jsonKv(key, std::to_string(value));
}

std::string jsonKvBool(const std::string& key, bool value) {
    return jsonKv(key, value ? "true" : "false");
}

}  // namespace ccap
