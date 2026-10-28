// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - the full compiler table
//
// The established pattern in compiler-configuration tools:
// a table of "compiler types", where each type describes
//   - the matching rules for compiler executable names
//   - the argument translation rules
//   - the system header search paths
//
// We keep the same idea: **one extensible table**, instead of if-else
// branches scattered through the code.
//
// Why matching is split into three categories:
//   exact   -- exact names (gcc / g++ / clang / cl)
//   prefix  -- prefixes (toolchain prefixes such as arm-none-eabi-)
//   suffix  -- suffixes (version suffixes such as gcc-11)
//
// Why we do not simply strip the suffix and look at the base name:
//   the **base name** of `arm-none-eabi-g++` is g++,
//   but it is a **different compiler** from the local g++ (different target,
//   different sysroot).
//   Confusing the two makes the analyzer use the wrong system headers.
//
//   So: prefix rules take precedence over exact matching.

#include "ccap_compdb.h"

#include <algorithm>
#include <cctype>

namespace ccap {
namespace compdb {

namespace {

// Lowercase (ASCII)
std::string lower(const std::string& s) {
    std::string r = s;
    for (char& c : r) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return r;
}

// Strip the Windows .exe suffix
std::string stripExe(const std::string& s) {
    if (s.size() > 4) {
        std::string tail = lower(s.substr(s.size() - 4));
        if (tail == ".exe") return s.substr(0, s.size() - 4);
    }
    return s;
}

}  // namespace

const char* compilerKindName(CompilerKind kind) {
    switch (kind) {
        case CompilerKind::Gcc:     return "gcc";
        case CompilerKind::Clang:   return "clang";
        case CompilerKind::Msvc:    return "msvc";
        case CompilerKind::Iar:     return "iar";
        case CompilerKind::Generic: return "generic";
        default:                    return "unknown";
    }
}

bool compilerKindFromName(const std::string& name, CompilerKind* out) {
    if (!out) return false;
    std::string l = lower(name);
    if (l == "gcc")     { *out = CompilerKind::Gcc;     return true; }
    if (l == "clang")   { *out = CompilerKind::Clang;   return true; }
    if (l == "msvc" || l == "cl" || l == "visualstudio") {
        *out = CompilerKind::Msvc;
        return true;
    }
    if (l == "iar")     { *out = CompilerKind::Iar;     return true; }
    if (l == "generic") { *out = CompilerKind::Generic; return true; }
    if (l == "unknown") { *out = CompilerKind::Unknown; return true; }
    return false;
}

bool CompilerRule::matchesBaseName(const std::string& base) const {
    std::string b = stripExe(base);

    // 1) Exact match (case-sensitive -- the classic tools match
    //    exactly and case-sensitively too)
    //
    //    But Windows is case-insensitive, so there we compare in lowercase.
#if defined(_WIN32)
    for (const auto& n : exactNames) {
        if (lower(stripExe(n)) == lower(b)) return true;
    }
#else
    for (const auto& n : exactNames) {
        if (stripExe(n) == b) return true;
    }
#endif

    // 2) Prefix rules -- cross toolchains
    //    Ahead of exact, because arm-none-eabi-g++ must not be treated as
    //    an ordinary g++
    for (const auto& p : prefixRules) {
        if (b.size() >= p.size() && b.compare(0, p.size(), p) == 0) return true;
    }

    // 3) Suffix rules -- version suffixes
    for (const auto& s : suffixRules) {
        if (b.size() > s.size() && b.compare(b.size() - s.size(), s.size(), s) == 0) {
            return true;
        }
    }

    return false;
}

CompilerTable::CompilerTable() = default;

CompilerTable CompilerTable::builtin() {
    CompilerTable t;

    // ── GCC family ─────────────────────────────────────────────────────
    {
        CompilerRule r;
        r.kind = CompilerKind::Gcc;
        r.id = "gcc";
        r.exactNames = {
            "gcc", "g++", "cc", "c++",
            "cpp",           // sometimes an alias for g++
            "gcc-cc",
            "gpp",
        };
        // Version suffixes: gcc-11, g++-12, cc-9 ...
        r.suffixRules = {};
        t.addRule(r);
    }

    // GCC's **version suffixes** get their own rule.
    //
    // Why not use a wildcard suffix?
    //   The suffix of "gcc-11" is "-11", but the suffix of
    //   "arm-none-eabi-gcc" is "-gcc" -- the latter is handled by a prefix
    //   rule.
    //   What is really needed is a "numeric version suffix", and pure string
    //   matching cannot express that.
    //
    //   So here we use a **prefix check in regex style**: the name starts
    //   with gcc-/g++-/cc-/c++- followed by digits. The matching logic lives
    //   outside matches_base_name -- see the implementation below.
    {
        CompilerRule r;
        r.kind = CompilerKind::Gcc;
        r.id = "gcc-versioned";
        r.exactNames = {};
        r.prefixRules = {};
        t.addRule(r);
    }

    // ── Clang family ───────────────────────────────────────────────────
    {
        CompilerRule r;
        r.kind = CompilerKind::Clang;
        r.id = "clang";
        r.exactNames = {
            "clang", "clang++", "clang-cl", "clang-cpp",
        };
        t.addRule(r);
    }

    // ── MSVC ───────────────────────────────────────────────────────────
    {
        CompilerRule r;
        r.kind = CompilerKind::Msvc;
        r.id = "msvc";
        r.exactNames = { "cl" };
        t.addRule(r);
    }

    // ── IAR ────────────────────────────────────────────────────────────
    {
        CompilerRule r;
        r.kind = CompilerKind::Iar;
        r.id = "iar";
        r.exactNames = { "iccarm", "iccarm.exe", "iarc", "ilink" };
        t.addRule(r);
    }

    // ── Fallback ───────────────────────────────────────────────────────
    {
        CompilerRule r;
        r.kind = CompilerKind::Generic;
        r.id = "generic";
        r.exactNames = {};
        t.addRule(r);
    }

    return t;
}

void CompilerTable::addRule(const CompilerRule& rule) {
    rules_.push_back(rule);
}

const CompilerRule* CompilerTable::findRule(const std::string& base) const {
    for (const auto& r : rules_) {
        if (r.matchesBaseName(base)) return &r;
    }
    return nullptr;
}

CompilerKind CompilerTable::matchBaseName(const std::string& base) const {
    // Special handling: version suffixes (gcc-11 / g++-12 / clang-15 /
    // clang++-17)
    //
    // Why handle it separately:
    //   "gcc-11" becomes "gcc" once the suffix is stripped, but a **suffix
    //   matching rule** cannot express the condition "followed by digits" in
    //   pure string terms.
    //   Rather than using a fragile regex, list the prefixes explicitly plus
    //   a digit check.
    {
        std::string b = stripExe(base);
        static const char* kVersioned[] = {
            "gcc-", "g++-", "cc-", "c++-",
            "clang-", "clang++-", "clang-cl-",
        };
        for (const char* p : kVersioned) {
            size_t n = std::char_traits<char>::length(p);
            if (b.size() > n && b.compare(0, n, p) == 0) {
                bool allDigits = true;
                for (size_t i = n; i < b.size(); ++i) {
                    if (!std::isdigit(static_cast<unsigned char>(b[i]))) {
                        allDigits = false;
                        break;
                    }
                }
                if (allDigits) {
                    // gcc-11 / clang-15 are gcc / clang
                    //
                    // Note the use of rfind instead of compare(0, 4, "clang") --
                    // the latter compares only 4 characters ("clan" vs
                    // "clang"), so it is never equal and clang-15 is never
                    // recognized.
                    if (b.rfind("clang", 0) == 0) return CompilerKind::Clang;
                    return CompilerKind::Gcc;
                }
            }
        }
    }

    for (const auto& r : rules_) {
        if (r.matchesBaseName(base)) return r.kind;
    }
    return CompilerKind::Unknown;
}

CompilerKind CompilerTable::matchPath(const std::string& path) const {
    std::string base = path;
    size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    return matchBaseName(base);
}

}  // namespace compdb
}  // namespace ccap
