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

// Split a path into its segments.
//
// Both separators are accepted, because a captured command line can
// carry either: the Go toolchain writes the path the way the platform
// does, and a build log written on Windows and read on Linux (or the
// other way round) mixes them.
std::vector<std::string> splitPathSegments(const std::string& path) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Match one path segment against one segment pattern.
//
// '*' matches any run of characters, '?' matches exactly one.
//
// WHY THE MATCH FOLLOWS THE PLATFORM'S CASE RULES
//
//   The repository's own glob matcher (globMatch in registry.cpp) is
//   case-insensitive, which is right for an executable **name** -- the
//   tools spell their names in one case only, and Windows is
//   case-insensitive anyway.
//
//   A path is a different thing: on Linux "/opt/Go/pkg/tool" and
//   "/opt/go/pkg/tool" are two different directories, and only one of
//   them is the toolchain. So the segment comparison follows the
//   platform: case-insensitive on Windows, case-sensitive elsewhere.
bool matchSegment(const std::string& segment, const std::string& pattern) {
    // A pattern without a wildcard is a plain comparison.
    if (pattern.find('*') == std::string::npos &&
        pattern.find('?') == std::string::npos) {
#if defined(_WIN32)
        return lower(segment) == lower(pattern);
#else
        return segment == pattern;
#endif
    }

    // The wildcard match: the classic two-pointer scan with one
    // backtrack point (no recursion, so no exponential blow-up).
    size_t p = 0, t = 0;
    size_t starP = std::string::npos, starT = 0;

    auto sameChar = [](char a, char b) {
#if defined(_WIN32)
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
#else
        return a == b;
#endif
    };

    while (t < segment.size()) {
        // The wildcard is tested first, so that a '*' inside the
        // segment is never taken for a literal.
        if (p < pattern.size() && pattern[p] == '*') {
            starP = p++;
            starT = t;
        } else if (p < pattern.size() &&
                   (pattern[p] == '?' || sameChar(pattern[p], segment[t]))) {
            ++p;
            ++t;
        } else if (starP != std::string::npos) {
            p = starP + 1;
            t = ++starT;
        } else {
            return false;
        }
    }

    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

}  // namespace

const char* compilerKindName(CompilerKind kind) {
    switch (kind) {
        case CompilerKind::Gcc:     return "gcc";
        case CompilerKind::Clang:   return "clang";
        case CompilerKind::Msvc:    return "msvc";
        case CompilerKind::Iar:     return "iar";
        case CompilerKind::Generic: return "generic";
        case CompilerKind::Javac:   return "javac";
        case CompilerKind::Ecj:     return "ecj";
        case CompilerKind::Kotlin:  return "kotlin";
        case CompilerKind::Scala3:  return "scala3";
        case CompilerKind::Scala2:  return "scala2";
        case CompilerKind::Csharp:  return "csharp";
        case CompilerKind::VbNet:   return "vbnet";
        case CompilerKind::FSharp:  return "fsharp";
        case CompilerKind::Go:      return "go";
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
    // The JVM languages. The names match the ones the capture agent
    // records, so the two can be joined without a mapping table.
    if (l == "javac" || l == "sjavac") { *out = CompilerKind::Javac; return true; }
    if (l == "ecj")   { *out = CompilerKind::Ecj;     return true; }
    if (l == "kotlin" || l == "kotlinc") { *out = CompilerKind::Kotlin; return true; }
    if (l == "scala3" || l == "scala") { *out = CompilerKind::Scala3; return true; }
    if (l == "scala2") { *out = CompilerKind::Scala2; return true; }
    // The .NET languages. The names match the driver the .NET backend
    // records, so the two can be joined without a mapping table.
    if (l == "csharp" || l == "csc") { *out = CompilerKind::Csharp; return true; }
    if (l == "vbnet" || l == "vbc")   { *out = CompilerKind::VbNet;  return true; }
    if (l == "fsharp" || l == "fsc")  { *out = CompilerKind::FSharp; return true; }
    // The Go toolchain. The name is the language, not the tool: the
    // tools themselves ("compile", "link") are recognized by their
    // path -- see the "go" rule in CompilerTable::builtin().
    if (l == "go" || l == "golang")   { *out = CompilerKind::Go;      return true; }
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

bool CompilerRule::matchesPath(const std::string& path) const {
    if (pathShapes.empty()) return false;

    const std::vector<std::string> segs = splitPathSegments(path);
    if (segs.size() < 2) return false;   // a shape needs a directory and a name

    const std::string& file = segs.back();

    for (const auto& shape : pathShapes) {
        const std::vector<std::string> pat = splitPathSegments(shape);
        if (pat.size() < 2) continue;    // a shape needs a directory and a name

        // The last segment of the shape is the executable name.
        if (!matchSegment(stripExe(file), pat.back())) continue;

        // The directory segments of the shape must appear, in order and
        // without a gap, somewhere in the path.
        //
        // The file name is not part of that search: it is the segment
        // after the ones the shape names.
        const size_t dirs = pat.size() - 1;
        if (segs.size() - 1 < dirs) continue;

        for (size_t start = 0; start + dirs <= segs.size() - 1; ++start) {
            bool ok = true;
            for (size_t i = 0; i < dirs; ++i) {
                if (!matchSegment(segs[start + i], pat[i])) {
                    ok = false;
                    break;
                }
            }
            if (ok) return true;
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

    // ── The JVM languages ─────────────────────────────────────────────
    //
    // These are matched by executable name, because that is how a build
    // invokes them. The rules are here so that a command line captured
    // from a build log is recognized as a compile command rather than
    // being dropped as an unknown tool.
    {
        CompilerRule r;
        r.kind = CompilerKind::Javac;
        r.id = "javac";
        r.exactNames = { "javac", "javac.exe", "sjavac" };
        t.addRule(r);
    }
    {
        CompilerRule r;
        r.kind = CompilerKind::Ecj;
        r.id = "ecj";
        r.exactNames = { "ecj", "ecj.exe" };
        t.addRule(r);
    }
    {
        CompilerRule r;
        r.kind = CompilerKind::Kotlin;
        r.id = "kotlin";
        r.exactNames = { "kotlinc", "kotlinc.bat", "kotlin", "kotlin.bat" };
        t.addRule(r);
    }
    {
        CompilerRule r;
        r.kind = CompilerKind::Scala3;
        r.id = "scala3";
        r.exactNames = { "scalac", "scalac.bat", "scala3", "scala3.bat", "dotc" };
        t.addRule(r);
    }
    {
        CompilerRule r;
        r.kind = CompilerKind::Scala2;
        r.id = "scala2";
        r.exactNames = { "scalac2", "scala" };
        t.addRule(r);
    }

    // ── The .NET languages ─────────────────────────────────────────────
    //
    // The Roslyn drivers are **never** invoked directly on the command
    // line: the build runs "dotnet exec <sdk>/Roslyn/bincore/csc.dll".
    // So the executable name alone never identifies them -- the .dll
    // name does.
    //
    // That is why the .NET backend does not rely on this table to
    // recognize a compile: it knows what it extracted. The rules are
    // here so that a command line captured from a build log -- where
    // the driver may appear as "csc.exe" or as "csc" -- is still
    // recognized as a compile command.
    {
        CompilerRule r;
        r.kind = CompilerKind::Csharp;
        r.id = "csharp";
        r.exactNames = { "csc", "csc.exe" };
        // The dll is matched as a suffix: the command line spells it
        // with a full path ("<sdk>/Roslyn/bincore/csc.dll"), and the
        // suffix rule matches the tail of the name.
        r.suffixRules = { "csc.dll" };
        t.addRule(r);
    }    {
        CompilerRule r;
        r.kind = CompilerKind::VbNet;
        r.id = "vbnet";
        r.exactNames = { "vbc", "vbc.exe" };
        r.suffixRules = { "vbc.dll" };
        t.addRule(r);
    }
    {
        CompilerRule r;
        r.kind = CompilerKind::FSharp;
        r.id = "fsharp";
        r.exactNames = { "fsc", "fsc.exe" };
        r.suffixRules = { "fsc.dll" };
        t.addRule(r);
    }

    // ── The Go toolchain ──────────────────────────────────────────────
    //
    // The two tools that matter are "compile" (one package -> one
    // archive) and "link" (the archives -> the binary).
    //
    // WHY THE RULE HAS NO NAMES
    //
    //   "compile" and "link" are ordinary program names. A name rule
    //   would recognize an unrelated tool that happens to be called
    //   "link" -- and the consequence is not cosmetic: the command
    //   would be handed to the Go parser, where "-o" and "-p" mean
    //   something completely different, and the build record would be
    //   wrong in a way nobody could see.
    //
    //   So the rule matches the **path**: the Go tools always live in
    //   the "pkg/tool/<goos>_<goarch>" directory of the toolchain.
    //
    // WHY ONLY THESE TWO
    //
    //   The same directory holds other tools -- "asm" (the assembler),
    //   "cgo", "pack", "vet", "asmhdr". They are deliberately **not**
    //   recognized here:
    //
    //     * the assembler is not a compilation of the project's own
    //       sources, and the default filter is supposed to drop it;
    //     * "cgo" generates code, it does not compile the package;
    //     * "pack" archives, "vet" analyses.
    //
    //   Recognizing them would make the filter keep commands that have
    //   nothing to do with the source tree.
    {
        CompilerRule r;
        r.kind = CompilerKind::Go;
        r.id = "go";
        r.exactNames = {};
        // The toolchain directory is named "<goos>_<goarch>": two
        // non-empty parts joined by an underscore ("linux_amd64",
        // "windows_amd64", "darwin_arm64", "js_wasm").
        //
        // The pattern spells that as "?*_*?": at least one character,
        // then an underscore, then at least one character. The '?'s
        // are what keep an empty side ("_") from matching.
        r.pathShapes = {
            "pkg/tool/?*_*?/compile",
            "pkg/tool/?*_*?/link",
        };
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
    // The path-shape rules first.
    //
    // They are the only rules that can tell two executables with the
    // same name apart, so they have to be tried before the name rules:
    // a name rule that matched first would decide the question the
    // path shape exists to answer.
    for (const auto& r : rules_) {
        if (r.matchesPath(path)) return r.kind;
    }

    // Otherwise the base name decides -- which is what this always did.
    std::string base = path;
    size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    return matchBaseName(base);
}

}  // namespace compdb
}  // namespace ccap
