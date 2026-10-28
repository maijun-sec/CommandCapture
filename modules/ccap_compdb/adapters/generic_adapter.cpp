// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - generic fallback adapter
//
// Meant for **unrecognized compilers**.
//
// It is important that it exists:
//   the list of compiler kinds can never be complete (vendor-specific ones,
//   cross toolchains, new toolchains).
//   Without a fallback, an unrecognized compiler would be lost entirely.
//
// It only does **generic** extraction:
//   -I / -D / -isystem (these are common to nearly all C-family compilers)
// It makes no compiler-specific assumptions.
//
// **Fully isolated from the other adapters**.

#include "ccap_compdb.h"

namespace ccap {
namespace compdb {
namespace adapters {

class GenericAdapter : public CompilerAdapter {
public:
    CompilerKind kind() const override { return CompilerKind::Generic; }

    const char* name() const override { return "generic"; }

    bool matches(const CompileCommand& cmd) const override {
        // Fallback: any command whose kind is unknown but that has source
        // files
        return cmd.kind == CompilerKind::Unknown ||
               cmd.kind == CompilerKind::Generic;
    }

    std::vector<std::string> translate(const CompileCommand& cmd,
                                       const AdapterContext& ctx) const override {
        std::vector<std::string> out;

        for (const auto& d : cmd.systemDirs) {
            out.push_back("--sys_include");
            out.push_back(d);
        }
        for (const auto& d : ctx.extraSystemIncludes) {
            out.push_back("--sys_include");
            out.push_back(d);
        }

        if (ctx.keepIncludeDirs) {
            for (const auto& d : cmd.includeDirs) {
                out.push_back("-I");
                out.push_back(d);
            }
        }

        if (ctx.keepDefines) {
            for (const auto& kv : cmd.defines) {
                if (kv.second.empty()) {
                    out.push_back("-D" + kv.first);
                } else {
                    out.push_back("-D" + kv.first + "=" + kv.second);
                }
            }
        }

        // ── Force-included headers ──────────────────────────────────────
        //
        // A forced include changes the meaning of the translation unit,
        // so it must reach the analyzer. The spelling is the analyzer's
        // own, so it does not depend on the original compiler's syntax.
        for (const auto& f : cmd.preIncludes) {
            out.push_back("--preinclude");
            out.push_back(f);
        }

        for (const auto& s : cmd.sourceFiles) {
            out.push_back(s);
        }

        return out;
    }
};

const CompilerAdapter* makeGenericAdapter() {
    static GenericAdapter inst;
    return &inst;
}

}  // namespace adapters
}  // namespace compdb
}  // namespace ccap
