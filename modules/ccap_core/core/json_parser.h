// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// CommandCapture - minimal JSON parsing
//
// The counterpart of json.h (serialization).
//
// Why no third-party library:
//   The project insists on zero third-party dependencies. This parser only
//   supports the features we need:
//   objects, arrays, strings, numbers, true/false/null.
//
// Why it lives in core rather than compdb:
//   Both the compdb configuration files and the compilation database
//   parsing need it ---
//   keeping it in core lets the two modules share one implementation.
//
// Limitations (written out explicitly, to avoid misuse):
//   - no comment support (the JSON spec does not support them either)
//   - numbers are stored as double (enough precision for configuration)
//   - for duplicate keys the last one wins
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ccap {

class JsonValue;
using JsonPtr = std::shared_ptr<JsonValue>;

// A JSON value
class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<JsonPtr> array;
    // Object: insertion order is preserved (a vector instead of a map ---
    // field order in a configuration file has readability value)
    std::vector<std::pair<std::string, JsonPtr>> object;

    // Type checks
    bool isNull()   const { return type == Type::Null; }
    bool isBool()   const { return type == Type::Bool; }
    bool isNumber() const { return type == Type::Number; }
    bool isString() const { return type == Type::String; }
    bool isArray()  const { return type == Type::Array; }
    bool isObject() const { return type == Type::Object; }

    // Value accessors (return the default when the type does not match, no
    // exceptions are thrown)
    std::string asString(const std::string& def = "") const {
        return isString() ? string : def;
    }
    double asNumber(double def = 0) const {
        return isNumber() ? number : def;
    }
    bool asBool(bool def = false) const {
        return isBool() ? boolean : def;
    }

    // Object field lookup (returns nullptr when it does not exist)
    const JsonValue* get(const std::string& key) const;

    // Object field lookup converted into a string array (accepts ["a","b"] or
    // "a b")
    std::vector<std::string> asStringArray() const;
};

// Parse JSON text
//
// Returns the root value on success; on failure returns nullptr and fills
// err with the reason.
JsonPtr jsonParse(const std::string& text, std::string* err = nullptr);

// Parse a JSON file
JsonPtr jsonParseFile(const std::string& path, std::string* err = nullptr);

}  // namespace ccap
