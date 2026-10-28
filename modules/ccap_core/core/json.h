// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// CommandCapture - minimal JSON serialization
//
// Does only what we need: safely write strings/numbers/arrays/objects as
// JSON. No third-party dependency is introduced.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ccap {

// JSON string escaping (without the surrounding quotes)
std::string jsonEscape(const std::string& s);

// Escape and add the surrounding double quotes
std::string jsonQuote(const std::string& s);

// Write an array of JSON strings as ["a","b"]
std::string jsonStringArray(const std::vector<std::string>& items);

// Key-value pair helpers: return the form "key": <value> (value is already
// formatted)
std::string jsonKv(const std::string& key, const std::string& rawValue);
std::string jsonKvStr(const std::string& key, const std::string& value);
std::string jsonKvNum(const std::string& key, int64_t value);
std::string jsonKvBool(const std::string& key, bool value);

}  // namespace ccap
