// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

#include "core/filter.h"

#include "core/normalize.h"

namespace ccap {

namespace {

#if defined(_WIN32)
// Windows file names are case-insensitive
bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}
#else
bool iequals(const std::string& a, const std::string& b) { return a == b; }
#endif

bool containsCi(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return false;
    if (needle.size() > hay.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            char ca = hay[i + j], cb = needle[j];
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
            if (ca != cb) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

}  // namespace

void EventFilter::excludeName(const std::string& name) {
    if (!name.empty()) excludes_.push_back(name);
}

void EventFilter::excludeSubstr(const std::string& needle) {
    if (!needle.empty()) substrExcludes_.push_back(needle);
}

void EventFilter::includeName(const std::string& name) {
    if (!name.empty()) includes_.push_back(name);
}

bool EventFilter::keep(const CommandEvent& e) const {
    // Allowlist takes priority: when non-empty, only the listed names stay
    if (!includes_.empty()) {
        bool hit = false;
        for (const auto& inc : includes_) {
            if (iequals(e.executable, inc) ||
                iequals(baseName(e.executable), inc)) {
                hit = true;
                break;
            }
        }
        if (!hit) return false;
    }

    // Exclude list
    for (const auto& ex : excludes_) {
        if (iequals(e.executable, ex) || iequals(baseName(e.executable), ex)) {
            return false;
        }
    }

    // Substring exclusion (applied to the full command line)
    for (const auto& needle : substrExcludes_) {
        if (containsCi(e.rawCommandLine, needle)) return false;
    }

    return true;
}

std::vector<CommandEvent> EventFilter::apply(const std::vector<CommandEvent>& events) {
    if (empty()) return events;

    std::vector<CommandEvent> out;
    out.reserve(events.size());
    excludedCount_ = 0;
    includedCount_ = 0;

    for (const auto& e : events) {
        if (keep(e)) {
            out.push_back(e);
            ++includedCount_;
        } else {
            ++excludedCount_;
        }
    }
    return out;
}

}  // namespace ccap
