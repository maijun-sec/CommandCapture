// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - Clang adapter
//
// Only knows the option syntax of the Clang family.
// Differences from the GCC adapter:
//   - Clang has -isystemafter, -iframework, -iquote
//   - The set of -std values differs from GCC's (but the format is the same)
//
// **Fully isolated from the other adapters**.

#include "ccap_compdb.h"

namespace ccap {
namespace compdb {
namespace adapters {

class ClangAdapter : public CompilerAdapter {
public:
    CompilerKind kind() const override { return CompilerKind::Clang; }

    const char* name() const override { return "clang"; }

    bool matches(const CompileCommand& cmd) const override {
        return cmd.kind == CompilerKind::Clang;
    }

    std::vector<std::string> translate(const CompileCommand& cmd,
                                       const AdapterContext& ctx) const override {
        std::vector<std::string> out;

        // ── System headers ──────────────────────────────────────────────
        //
        // The semantics of Clang's -isystem differ slightly from GCC's
        // (higher priority), but there is no need to distinguish them when
        // translating to the analyzer's --sys_include --
        // the analyzer only has one notion of "system include".
        for (const auto& d : cmd.systemDirs) {
            out.push_back("--sys_include");
            out.push_back(d);
        }
        for (const auto& d : ctx.extraSystemIncludes) {
            out.push_back("--sys_include");
            out.push_back(d);
        }

        // ── Framework (macOS) ──────────────────────────────────────────
        for (const auto& d : cmd.frameworkDirs) {
            out.push_back("-F");
            out.push_back(d);
        }

        // ── Include paths ───────────────────────────────────────────────
        if (ctx.keepIncludeDirs) {
            for (const auto& d : cmd.includeDirs) {
                out.push_back("-I");
                out.push_back(d);
            }
        }

        // ── Macro definitions ───────────────────────────────────────────
        if (ctx.keepDefines) {
            for (const auto& kv : cmd.defines) {
                if (kv.second.empty()) {
                    out.push_back("-D" + kv.first);
                } else {
                    out.push_back("-D" + kv.first + "=" + kv.second);
                }
            }
        }

        // ── Language standard ───────────────────────────────────────────
        if (!cmd.languageStandard.empty()) {
            out.push_back("-std=" + cmd.languageStandard);
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

const CompilerAdapter* makeClangAdapter() {
    static ClangAdapter inst;
    return &inst;
}

}  // namespace adapters
}  // namespace compdb
}  // namespace ccap
