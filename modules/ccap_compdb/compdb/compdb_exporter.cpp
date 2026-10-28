// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - export
//
// Two kinds of output:
//   1. clang compilation database (compile_commands.json)
//   2. Analyzer-style translated commands (one per line)
//
// Specification: https://clang.llvm.org/docs/JSONCompilationDatabase.html

#include "ccap_compdb.h"

#include "core/json.h"
#include "core/normalize.h"

namespace ccap {
namespace compdb {

namespace {

// Relative path -> absolute (relative to directory)
std::string absolutize(const std::string& path, const std::string& directory) {
    if (path.empty()) return path;
    if (ccap::isAbsolutePath(path)) return ccap::normalizePath(path);
    if (directory.empty()) return path;
    return ccap::normalizePath(directory + "/" + path);
}

}  // namespace

std::string exportClangCompilationDb(const std::vector<CompileCommand>& commands) {
    // Use "arguments" rather than "command" --
    //
    // The specification supports both, but "arguments" is an **array**,
    // one token per element, so no command-line quote escaping is needed.
    // Consumers (clang tooling, ccls, ...) accept both, but arguments is
    // less error-prone.

    std::string out = "[\n";
    bool firstEntry = true;

    for (const auto& cmd : commands) {
        if (cmd.sourceFiles.empty()) continue;   // a link-only command has no compilation unit

        // directory: must be an absolute path -- consumers may run under a
        // different cwd
        std::string dir = cmd.directory;
        if (dir.empty()) dir = ccap::currentDir();
        dir = ccap::normalizePath(dir);

        // arguments: argv[0] is the compiler, the rest are arguments
        std::vector<std::string> args = cmd.argv;
        if (args.empty()) {
            args.push_back(cmd.compilerBase);
        }

        if (!firstEntry) out += ",\n";
        firstEntry = false;

        out += "  {\n";
        out += "    \"directory\": " + ccap::jsonQuote(dir) + ",\n";
        out += "    \"arguments\": [";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) out += ", ";
            out += ccap::jsonQuote(args[i]);
        }
        out += "],\n";

        // file: the compilation unit. The specification recommends an
        // absolute path.
        //
        // One command may have multiple source files (rare but it happens,
        // e.g. a unity build) -- that would produce multiple entries, one per
        // source file.
        for (const auto& src : cmd.sourceFiles) {
            out += "    \"file\": " + ccap::jsonQuote(absolutize(src, dir)) + "\n";
            // With multiple source files, should file be extra entries
            // separated by commas? No -- the specification allows only one
            // file per entry.
            // So only the first source file is taken here (in the vast
            // majority of cases there is only one).
            break;
        }
        out += "  }";
    }

    if (!firstEntry) out += "\n";
    out += "]\n";
    return out;
}

std::string exportTranslatedCommands(const std::vector<CompileCommand>& commands,
                                      const AdapterRegistry& registry,
                                      const AdapterContext& ctx) {
    std::string out;

    for (const auto& cmd : commands) {
        if (cmd.sourceFiles.empty()) continue;

        const CompilerAdapter* adapter = registry.find(cmd);
        if (!adapter) continue;

        std::vector<std::string> args = adapter->translate(cmd, ctx);
        if (args.empty()) continue;

        // The form:
        //   <compiler> <translated args>
        //
        // What we emit are the **translated arguments**, leaving the prefix
        // to the caller to decide (any front end).
        //
        // Note: the line is assembled in a local variable here and appended
        // to out at the end -- an early version assigned
        // `out = cmd.compilerPath` directly, which wiped out all previously
        // accumulated lines, so only the last command was emitted.
        std::string line;
        if (!cmd.compilerPath.empty()) {
            line = cmd.compilerPath;
        } else {
            line = cmd.compilerBase;
        }
        for (const auto& a : args) {
            line += ' ';
            line += a;
        }
        out += line;
        out += '\n';
    }

    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// clang command export
// ═══════════════════════════════════════════════════════════════════════════

std::string exportClangCommands(const std::vector<CompileCommand>& commands) {
    // One clang command per line, ready to be fed to a shell or a build
    // system.
    //
    // Unlike the compilation database (which keeps the *original* argv), this
    // rewrites the command for clang: the driver becomes clang / clang++,
    // and the options are translated to clang's spelling.
    std::string out;
    for (const auto& cmd : commands) {
        if (cmd.sourceFiles.empty()) continue;   // a pure link has no unit

        std::vector<std::string> args = toClangCommand(cmd);
        if (args.empty()) continue;

        for (size_t i = 0; i < args.size(); ++i) {
            if (i) out += ' ';
            // Quote only when the token actually needs it.
            bool needsQuote = args[i].empty() ||
                              args[i].find_first_of(" \t\"'") != std::string::npos;
            if (needsQuote) {
                out += '"';
                for (char c : args[i]) {
                    if (c == '"' || c == '\\') out += '\\';
                    out += c;
                }
                out += '"';
            } else {
                out += args[i];
            }
        }
        out += '\n';
    }
    return out;
}

}  // namespace compdb
}  // namespace ccap

