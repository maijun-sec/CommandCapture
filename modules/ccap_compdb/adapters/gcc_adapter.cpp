// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - GCC adapter
//
// Only knows the option syntax of the GCC/G++ family.
//
// **Fully isolated from the other adapters** -- it references no other
// adapter, and no other adapter references it.
// Adding a new compiler = adding a new .cpp, nothing here changes.

#include "ccap_compdb.h"

namespace ccap {
namespace compdb {
namespace adapters {

class GccAdapter : public CompilerAdapter {
public:
    CompilerKind kind() const override { return CompilerKind::Gcc; }

    const char* name() const override { return "gcc"; }

    bool matches(const CompileCommand& cmd) const override {
        return cmd.kind == CompilerKind::Gcc;
    }

    std::vector<std::string> translate(const CompileCommand& cmd,
                                       const AdapterContext& ctx) const override {
        std::vector<std::string> out;

        // ── System headers ──────────────────────────────────────────────
        //
        // Use --sys_include (the argument name used by static-analysis
        // front ends) rather than gcc's own -isystem --
        // because the target is the **analyzer's** internal representation,
        // not gcc.
        //
        // The order matters: first the ones gcc ships with, then the ones
        // given via -isystem, and finally the ones added by the
        // configuration.
        for (const auto& d : cmd.systemDirs) {
            out.push_back("--sys_include");
            out.push_back(d);
        }
        for (const auto& d : ctx.extraSystemIncludes) {
            out.push_back("--sys_include");
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
        //
        // The semantics of gcc's -Dfoo=bar are **stringification** (foo
        // becomes "bar"), while the analyzer usually wants the raw token --
        // so the -Dfoo=bar form is kept as is, and the analyzer decides for
        // itself how to interpret it.
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

const CompilerAdapter* makeGccAdapter() {
    static GccAdapter inst;
    return &inst;
}

}  // namespace adapters
}  // namespace compdb
}  // namespace ccap
