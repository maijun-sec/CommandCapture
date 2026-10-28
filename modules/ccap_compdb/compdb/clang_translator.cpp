// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - clang command translation
//
// Converts a native compile command (gcc / MSVC / clang / ...) into a **real
// clang command line**.
//
// Why this is separate from the compiler adapters:
//
//   The adapters (gcc_adapter / msvc_adapter / ...) translate a command into
//   the argument set a **static-analysis front end** expects
//   (--sys_include, --preinclude, ...). That is not a compiler command line.
//
//   This module answers a different question: "if I want to compile this
//   translation unit with clang instead of the original compiler, what is the
//   command line?"
//
//   That is what tooling needs when it wants to replay a build under a
//   different compiler: a compile_commands.json that points at clang, a
//   clang-tidy run over a gcc-built project, a CI matrix entry that swaps
//   gcc for clang.
//
// The conversion is not a string substitution -- the two compilers use
// different option syntax, and some options have no direct equivalent:
//
//   MSVC          clang
//   -------      -----
//   /I<dir>       -I<dir>
//   /D<name>      -D<name>
//   /std:c++17    -std=c++17
//   /EHsc         (no direct equivalent -- clang enables exceptions by
//                  default on the platforms we target)
//   /Zi           -g
//   /O2           -O2
//   /Fo<file>     -o <file>
//   /Fe<file>     (linker output, not applicable to a compile)
//
// Options with no equivalent are reported in the command's notes, so the
// caller knows the translation is lossy rather than silently different.

#include "ccap_compdb.h"

#include "core/normalize.h"

namespace ccap {
namespace compdb {

namespace {

// The clang driver to invoke.
//
// "clang++" is used for C++ sources and "clang" for C sources, because
// linking a C++ translation unit needs the C++ driver (it pulls in
// libstdc++ / libc++).
const char* driverFor(const CompileCommand& cmd) {
    // ── The compiler may have been told the language explicitly ─────────
    //
    // MSVC's /TP (compile the following files as C++) and /Tp<file>
    // (compile that file as C++) override the file extension.
    //
    // This matters because the driver choice changes the **ABI** of the
    // produced object: clang++ links libstdc++, clang does not. Picking
    // the driver from the extension alone would silently produce a
    // different object than the original build did.
    //
    // Per-file overrides win over the global /TP.
    for (const auto& src : cmd.sourceFiles) {
        for (const auto& kv : cmd.perFileLanguages) {
            if (kv.first == src) {
                return (kv.second == "cpp") ? "clang++" : "clang";
            }
        }
    }
    if (cmd.forcedLanguage == "cpp") return "clang++";
    if (cmd.forcedLanguage == "c") return "clang";

    // ── Otherwise the extension decides ─────────────────────────────────
    for (const auto& src : cmd.sourceFiles) {
        std::string ext = ccap::extensionOf(src);
        if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c++" ||
            ext == ".C" || ext == ".mm") {
            return "clang++";
        }
    }
    return "clang";
}

// Map an MSVC /std: value to a clang -std= value.
//
// MSVC spells them /std:c++14, /std:c++17, /std:c++20, /std:c++latest.
// Clang wants -std=c++14, -std=c++17, -std=c++20.
// "latest" has no clang equivalent -- it maps to the newest standard the
// toolchain supports, which we cannot know here, so it is left to clang's
// default and reported.
std::string mapMsvcStd(const std::string& value) {
    if (value.empty()) return "";
    if (value == "latest") return "";
    // "/std:c++17" -> "c++17"
    std::string v = value;
    size_t colon = v.find(':');
    if (colon != std::string::npos) v = v.substr(colon + 1);
    if (v.empty()) return "";
    return v;
}

// MSVC optimisation flags -> clang equivalents.
const char* mapMsvcOptimisation(const std::string& value) {
    if (value == "/O2" || value == "/Ox") return "-O2";
    if (value == "/O1") return "-O1";
    if (value == "/Od") return "-O0";
    if (value == "/Os" || value == "/Ot" || value == "/Ob") return "-Os";
    return nullptr;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// The conversion
// ═══════════════════════════════════════════════════════════════════════════

std::vector<std::string> toClangCommand(const CompileCommand& cmd) {
    std::vector<std::string> out;
    out.push_back(driverFor(cmd));

    // ── System include paths ────────────────────────────────────────────
    //
    // -isystem is the clang spelling of both gcc's -isystem and MSVC's
    // /external:I. The order is preserved: the search order matters.
    for (const auto& d : cmd.systemDirs) {
        out.push_back("-isystem");
        out.push_back(d);
    }

    // ── Include paths ───────────────────────────────────────────────────
    for (const auto& d : cmd.includeDirs) {
        out.push_back("-I");
        out.push_back(d);
    }

    // ── Framework paths (macOS) ─────────────────────────────────────────
    for (const auto& d : cmd.frameworkDirs) {
        out.push_back("-F");
        out.push_back(d);
    }

    // ── Force-included headers ─────────────────────────────────────────
    //
    // MSVC's /FI<file> becomes clang's -include <file>.
    //
    // This one is not optional: a forced include can define macros that
    // change which code compiles. Dropping it would make the translated
    // command compile something **different** from the original.
    for (const auto& f : cmd.preIncludes) {
        out.push_back("-include");
        out.push_back(f);
    }

    // ── Macro definitions ───────────────────────────────────────────────
    for (const auto& kv : cmd.defines) {
        if (kv.second.empty()) {
            out.push_back("-D" + kv.first);
        } else {
            out.push_back("-D" + kv.first + "=" + kv.second);
        }
    }

    // ── Language standard ───────────────────────────────────────────────
    //
    // The extracted standard may be in either spelling:
    //   gcc / clang:  "c++17"      (from -std=c++17)
    //   MSVC:         "c++17"      (already normalised by the parser)
    //
    // But a raw -std: value can also reach here, so map it defensively.
    if (!cmd.languageStandard.empty()) {
        std::string standard = cmd.languageStandard;
        if (standard.rfind("/std:", 0) == 0 || standard.rfind("std:", 0) == 0) {
            standard = mapMsvcStd(standard);
        }
        if (!standard.empty()) {
            out.push_back("-std=" + standard);
        }
    }

    // ── Debug info ──────────────────────────────────────────────────────
    //
    // MSVC's /Zi becomes clang's -g. gcc's -g is already clang's -g.
    bool sawDebugFlag = false;
    for (const auto& opt : cmd.otherOptions) {
        if (opt == "/Zi" || opt == "/Z7") {
            out.push_back("-g");
            sawDebugFlag = true;
        }
    }
    if (!sawDebugFlag) {
        for (const auto& opt : cmd.otherOptions) {
            if (opt == "-g") {
                out.push_back("-g");
                break;
            }
        }
    }

    // ── Optimisation level ──────────────────────────────────────────────
    bool sawOptimisation = false;
    for (const auto& opt : cmd.otherOptions) {
        if (opt.size() > 2 && opt[0] == '/' && opt[1] == 'O') {
            const char* mapped = mapMsvcOptimisation(opt);
            if (mapped) {
                out.push_back(mapped);
                sawOptimisation = true;
            }
        }
    }
    if (!sawOptimisation) {
        for (const auto& opt : cmd.otherOptions) {
            if (opt.size() > 2 && opt[0] == '-' && opt[1] == 'O') {
                out.push_back(opt);
                break;
            }
        }
    }

    // ── Compile only ────────────────────────────────────────────────────
    //
    // A compile command must not link. clang spells it -c.
    //
    // The original command's intent was recorded during parsing
    // (cmd.compileOnly), because -c / /c is consumed there.
    //
    // The fallback: if the flag was not recorded but the command has source
    // files and is not link-only, it is a compile+link command. Emitting -c
    // for it would be wrong (it would drop the link step), so we only emit
    // -c when the intent is known.
    if (cmd.compileOnly) {
        out.push_back("-c");
    }

    // ── Output file ─────────────────────────────────────────────────────
    if (!cmd.outputFile.empty()) {
        out.push_back("-o");
        out.push_back(cmd.outputFile);
    }

    // ── Source files ────────────────────────────────────────────────────
    for (const auto& s : cmd.sourceFiles) {
        out.push_back(s);
    }

    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// Lossy translation report
// ═══════════════════════════════════════════════════════════════════════════

std::vector<std::string> clangTranslationLosses(const CompileCommand& cmd) {
    std::vector<std::string> losses;

    // MSVC-only options with no clang equivalent.
    static const char* kNoEquivalent[] = {
        "/EHsc", "/EHs", "/EHsc-",   // exception handling model
        "/MD", "/MT", "/MTd", "/MDd", // runtime library selection
        "/GR", "/permissive-",       // language strictness
        "/utf-8",                    // source charset (clang defaults to utf-8)
        "/W3", "/W4", "/WX",         // warning levels (clang uses -W*)
        "/nologo", "/FS", "/Fd", "/Fo",
    };

    for (const auto& opt : cmd.otherOptions) {
        for (const char* ne : kNoEquivalent) {
            if (opt == ne) {
                losses.push_back(std::string("no clang equivalent for ") + opt);
                break;
            }
        }
    }

    // MSVC's /std:latest has no fixed clang spelling.
    if (cmd.languageStandard == "latest" ||
        cmd.languageStandard == "/std:latest") {
        losses.push_back("no clang equivalent for /std:latest "
                         "(clang's default standard was used)");
    }

    // The runtime library selection changes the ABI, not just the flags.
    // Losing it can change behaviour, so it is worth calling out separately.
    for (const auto& opt : cmd.otherOptions) {
        if (opt == "/MD" || opt == "/MT" || opt == "/MDd" || opt == "/MTd") {
            losses.push_back(std::string("runtime library selection ") + opt +
                             " was dropped; the ABI may differ from the "
                             "original build");
            break;
        }
    }

    return losses;
}

}  // namespace compdb
}  // namespace ccap
