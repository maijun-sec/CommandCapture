// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - filter rules
//
// Goal: **keep only the real compile commands**.
//
// What has to be filtered out:
//   - scripts (sh / bash / cmd.exe / powershell / .bat / .sh / .ps1)
//   - linkers (ld / link / collect2 / ld.lld)
//   - assemblers (as / ml64)
//   - archivers (ar / lib)
//   - wrappers (ccache / sccache / distcc) -- these **must be kept**,
//     because the real compiler sits behind the wrapper
//
// Decision order (strongest to weakest):
//   1. allowlist hit -> keep
//   2. explicitly excluded -> drop
//   3. is a script -> drop
//   4. kind matches a compiler -> keep
//   5. has -c and has source files -> keep
//   6. anything else -> drop
//
// Rule 5 is the **fallback**: we may not recognize the compiler's name
// (user-defined ones), but the combination of "-c + source files" is a
// strong characteristic of a compile command.

#include "ccap_compdb.h"

#include <algorithm>
#include <cctype>

namespace ccap {
namespace compdb {

namespace {

std::string lower(const std::string& s) {
    std::string r = s;
    for (char& c : r) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return r;
}

// Strip .exe
std::string stripExe(const std::string& s) {
    std::string l = lower(s);
    if (l.size() > 4 && l.substr(l.size() - 4) == ".exe") {
        return s.substr(0, s.size() - 4);
    }
    return s;
}

// Names of scripts (always excluded)
//
// Note `cc` / `c++` are not in the list -- they are compiler aliases.
bool isScriptName(const std::string& base) {
    static const char* kScripts[] = {
        // POSIX shell
        "sh", "bash", "dash", "zsh", "ksh", "csh", "tcsh", "fish",
        // Windows
        "cmd", "powershell", "pwsh", "command",
        // Build script interpreters
        "python", "python3", "perl", "ruby", "node", "deno", "bun",
        "make", "gmake", "cmake", "ninja", "msbuild", "devenv",
        "msbuild.exe", "xbuild",
        // Other tools (not compilers, but easily mistaken for one)
        "git", "svn", "hg",
    };
    std::string b = lower(stripExe(base));
    for (const char* s : kScripts) {
        if (b == s) return true;
    }
    return false;
}

}  // namespace

bool FilterConfig::isScript(const std::string& baseName) {
    return isScriptName(baseName);
}

FilterConfig FilterConfig::builtin() {
    FilterConfig f;

    // Linkers
    f.excludeNames = {
        "ld", "ld.bfd", "ld.gold", "ld.lld", "lld", "lld-link",
        "link", "link.exe",
        "collect2",
        "gcc-ar", "gcc-ranlib", "gcc-nm",
        "ar", "ranlib", "nm", "objcopy", "objdump", "strip",
        "as", "ml", "ml64", "nasm", "yasm", "masm",
        "lib", "lib.exe",
    };

    // Wrapper prefixes -- these are **cache/distribution** wrappers,
    // the real compiler sits behind them.
    //
    // But we do **not** exclude them -- instead the prefix is stripped
    // during parsing so that the real compiler is recognized. See the
    // handling in command_parser.
    //
    // What is excluded here are prefixes that are "definitely not a
    // compiler".
    f.excludePrefixes = {
        "cmake-",       // things like cmake -E
        "cpack-",
        "ctest-",
    };

    return f;
}

FilterRules::FilterRules(const FilterConfig& config) : config_(config) {}

bool FilterRules::keep(const CompileCommand& cmd) const {
    // 1) Allowlist
    if (!config_.includeNames.empty()) {
        for (const auto& inc : config_.includeNames) {
            if (lower(stripExe(cmd.compilerBase)) == lower(stripExe(inc))) {
                return true;
            }
        }
        return false;
    }

    // 2) Explicitly excluded
    for (const auto& ex : config_.excludeNames) {
        if (lower(stripExe(cmd.compilerBase)) == lower(stripExe(ex))) {
            return false;
        }
    }
    for (const auto& p : config_.excludePrefixes) {
        if (cmd.compilerBase.size() >= p.size() &&
            lower(cmd.compilerBase).compare(0, p.size(), lower(p)) == 0) {
            return false;
        }
    }

    // 3) Scripts
    //
    // Excluded by default -- the user explicitly asked for "only the real
    // compile commands".
    // Kept when keep_scripts is set (to view the full picture).
    if (!config_.keepScripts && isScriptName(cmd.compilerBase)) {
        return false;
    }

    // 4) Compiler kind match
    if (cmd.kind != CompilerKind::Unknown &&
        cmd.kind != CompilerKind::Generic) {
        return true;
    }

    // 5) Fallback: -c + source files
    //
    // We may not recognize the compiler's name (user-defined, vendor
    // specific), but "-c + source files" is a strong characteristic of a
    // compile command.
    if (!cmd.sourceFiles.empty()) {
        for (const auto& a : cmd.argv) {
            if (a == "-c") return true;
        }
    }

    return false;
}

std::string FilterRules::explain(const CompileCommand& cmd) const {
    if (!config_.includeNames.empty()) {
        for (const auto& inc : config_.includeNames) {
            if (lower(stripExe(cmd.compilerBase)) == lower(stripExe(inc))) {
                return "kept: whitelist match (" + inc + ")";
            }
        }
        return "dropped: not in whitelist";
    }

    for (const auto& ex : config_.excludeNames) {
        if (lower(stripExe(cmd.compilerBase)) == lower(stripExe(ex))) {
            return "dropped: excluded (" + ex + ")";
        }
    }
    for (const auto& p : config_.excludePrefixes) {
        if (cmd.compilerBase.size() >= p.size() &&
            lower(cmd.compilerBase).compare(0, p.size(), lower(p)) == 0) {
            return "dropped: excluded prefix (" + p + ")";
        }
    }

    if (!config_.keepScripts && isScriptName(cmd.compilerBase)) {
        return "dropped: script (no compilation happens)";
    }
    if (cmd.kind != CompilerKind::Unknown &&
        cmd.kind != CompilerKind::Generic) {
        return std::string("kept: compiler kind = ") +
               compilerKindName(cmd.kind);
    }
    if (!cmd.sourceFiles.empty()) {
        for (const auto& a : cmd.argv) {
            if (a == "-c") return "kept: has -c and source files";
        }
    }
    return "dropped: not recognized as a compilation";
}

}  // namespace compdb
}  // namespace ccap
