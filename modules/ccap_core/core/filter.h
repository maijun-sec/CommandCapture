// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// CommandCapture - event filtering
//
// Why filtering is needed:
//   A full cmake build produces hundreds of events (as / cc1plus / collect2 /
//   ld / cmake -E ...), and the vast majority of them are internal steps of
//   the compiler. What the user really wants to see is "which external tools
//   did this script actually invoke".
//
// Design principles of the filter:
//   1. **No filtering by default** --- silently dropping events is the worst
//      possible failure mode.
//      Filtering must be explicitly enabled by the user.
//   2. Matching is done on the **executable file name** or the **full path**;
//      case sensitivity follows the platform
//      (case-insensitive on Windows, case-sensitive on Linux).
//   3. Filtered-out events **must be counted and reported** --- the user has
//      to know how many were missed.
#pragma once

#include <string>
#include <vector>

#include "core/event.h"

namespace ccap {

class EventFilter {
public:
    // Exclusion rules: match the executable file name (basename) or the
    // full command line (substring)
    void excludeName(const std::string& name);
    void excludeSubstr(const std::string& needle);

    // Keep only the events matching these names (allowlist). Only takes
    // effect when non-empty.
    void includeName(const std::string& name);

    bool empty() const { return excludes_.empty() && substrExcludes_.empty() && includes_.empty(); }

    // Should this event be kept
    bool keep(const CommandEvent& e) const;

    size_t excludedCount() const { return excludedCount_; }
    size_t includedCount() const { return includedCount_; }

    // Apply the filter to the event list (returns the ones that were kept)
    std::vector<CommandEvent> apply(const std::vector<CommandEvent>& events);

private:
    std::vector<std::string> excludes_;
    std::vector<std::string> substrExcludes_;
    std::vector<std::string> includes_;
    size_t excludedCount_ = 0;
    size_t includedCount_ = 0;
};

}  // namespace ccap
