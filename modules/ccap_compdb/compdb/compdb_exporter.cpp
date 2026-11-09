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

// Join the arguments into one command line.
//
// A token is quoted only when it actually needs it. The rule is the
// shell's: a token with a space, a quote or a tab in it needs the
// quotes; everything else is emitted bare.
//
// The backslash and the quote are escaped inside the quotes, because
// the command line has to survive a round trip through a shell.
std::string joinCommandLine(const std::vector<std::string>& args) {
    std::string out;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) out += ' ';
        const std::string& a = args[i];
        bool needsQuote = a.empty() ||
                          a.find_first_of(" \t\"'") != std::string::npos;
        if (needsQuote) {
            out += '"';
            for (char c : a) {
                if (c == '"' || c == '\\') out += '\\';
                out += c;
            }
            out += '"';
        } else {
            out += a;
        }
    }
    return out;
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
        // A command without sources is a link, and a link has no
        // compilation unit.
        //
        // The Go link step is the exception, and it is kept **because**
        // it has no sources: its "-importcfg" is the authoritative
        // statement of which packages are in the binary, and that
        // answer exists nowhere else. The compile steps only say what
        // each package imports.
        //
        // Dropping it would throw the authoritative answer away, so
        // the line is emitted -- the export data of every package in
        // the binary, and the path of the manifest that pairs them
        // with their import paths.
        if (cmd.sourceFiles.empty() &&
            !(cmd.kind == CompilerKind::Go &&
              !cmd.importConfigPath.empty())) {
            continue;
        }

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

        out += joinCommandLine(args);
        out += '\n';
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// The level-aware clang command export
// ═══════════════════════════════════════════════════════════════════════════

std::vector<std::string> exportClangCommandLines(
    const std::vector<CompileCommand>& commands,
    const OptionMapper& mapper,
    TranslateLevel level,
    std::vector<MappingReport>* reports) {
    std::vector<std::string> out;
    for (const auto& cmd : commands) {
        if (cmd.sourceFiles.empty()) continue;   // a pure link has no unit

        MappingReport report;
        std::vector<std::string> args = mapper.map(cmd, level, &report);
        if (args.empty()) continue;

        out.push_back(joinCommandLine(args));
        if (reports) reports->push_back(report);
    }
    return out;
}

std::string exportClangCommandLinesText(
    const std::vector<CompileCommand>& commands,
    const OptionMapper& mapper,
    TranslateLevel level) {
    std::vector<std::string> lines =
        exportClangCommandLines(commands, mapper, level, nullptr);

    std::string out;
    for (const auto& l : lines) {
        out += l;
        out += '\n';
    }
    return out;
}

std::string exportMappingReports(
    const std::vector<CompileCommand>& commands,
    const OptionMapper& mapper,
    TranslateLevel level) {
    std::string out;
    out += "retention level: ";
    out += translateLevelName(level);
    out += "\n";

    for (const auto& cmd : commands) {
        if (cmd.sourceFiles.empty()) continue;

        MappingReport report;
        mapper.map(cmd, level, &report);

        out += "\n--- ";
        out += cmd.compilerBase;
        if (!cmd.sourceFiles.empty()) {
            out += " ";
            out += cmd.sourceFiles.front();
        }
        out += "\n";

        out += report.toText();

        if (!report.unknown.empty()) {
            out += "  unknown options (not in the switch table): ";
            for (size_t i = 0; i < report.unknown.size(); ++i) {
                if (i) out += ", ";
                out += report.unknown[i];
            }
            out += "\n";
        }
        if (!report.noEquivalent.empty()) {
            out += "  no clang equivalent: ";
            for (size_t i = 0; i < report.noEquivalent.size(); ++i) {
                if (i) out += ", ";
                out += report.noEquivalent[i];
            }
            out += "\n";
        }
    }

    return out;
}

}  // namespace compdb
}  // namespace ccap

