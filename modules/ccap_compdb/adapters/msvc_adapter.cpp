// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - MSVC adapter
//
// Only knows the option syntax of MSVC cl.exe.
//
// The key differences between MSVC and GCC/Clang:
//   - macro definitions are /D (not -D)
//   - includes are /I (not -I)
//   - the language standard is /std:c++17 (not -std=c++17)
//   - system includes are /external:I (not -isystem)
//   - the source file extension is .cpp (the same), but the output is .obj
//
// **Fully isolated from the other adapters** -- it never shares GCC's
// parsing code, because the option syntaxes are fundamentally different.

#include "ccap_compdb.h"

namespace ccap {
namespace compdb {
namespace adapters {

class MsvcAdapter : public CompilerAdapter {
public:
    CompilerKind kind() const override { return CompilerKind::Msvc; }

    const char* name() const override { return "msvc"; }

    bool matches(const CompileCommand& cmd) const override {
        return cmd.kind == CompilerKind::Msvc;
    }

    std::vector<std::string> translate(const CompileCommand& cmd,
                                       const AdapterContext& ctx) const override {
        std::vector<std::string> out;

        // ── System headers ──────────────────────────────────────────────
        //
        // MSVC's system includes (Windows SDK / CRT) are given via
        // /external:I. Translate them into the analyzer's --sys_include.
        for (const auto& d : cmd.systemDirs) {
            out.push_back("--sys_include");
            out.push_back(d);
        }
        for (const auto& d : ctx.extraSystemIncludes) {
            out.push_back("--sys_include");
            out.push_back(d);
        }

        // ── Include paths ───────────────────────────────────────────────
        //
        // Use /I (MSVC style) rather than -I -- if the analyzer interprets
        // the arguments with MSVC semantics, keeping them as is is safest.
        if (ctx.keepIncludeDirs) {
            for (const auto& d : cmd.includeDirs) {
                out.push_back("/I");
                out.push_back(d);
            }
        }

        // ── Macro definitions ───────────────────────────────────────────
        if (ctx.keepDefines) {
            for (const auto& kv : cmd.defines) {
                if (kv.second.empty()) {
                    out.push_back("/D" + kv.first);
                } else {
                    out.push_back("/D" + kv.first + "=" + kv.second);
                }
            }
        }

        // ── Language standard ───────────────────────────────────────────
        //
        // MSVC's /std:c++17 has already been parsed into
        // language_standard = "c++17"; here it is turned back into the
        // MSVC spelling.
        if (!cmd.languageStandard.empty()) {
            out.push_back("/std:" + cmd.languageStandard);
        }

        // ── Force-included headers ──────────────────────────────────────
        //
        // A forced include changes the meaning of the translation unit,
        // so it must reach the analyzer.
        for (const auto& f : cmd.preIncludes) {
            out.push_back("--preinclude");
            out.push_back(f);
        }

        // ── Source files ────────────────────────────────────────────────
        for (const auto& s : cmd.sourceFiles) {
            out.push_back(s);
        }

        return out;
    }
};

const CompilerAdapter* makeMsvcAdapter() {
    static MsvcAdapter inst;
    return &inst;
}

}  // namespace adapters
}  // namespace compdb
}  // namespace ccap
