// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

#include "cli.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ccap {
namespace cli {

namespace {

bool needValue(int argc, char** argv, int* i, const char* opt,
                std::string* err) {
    if (*i + 1 >= argc) {
        *err = std::string(opt) + " needs a value";
        return false;
    }
    ++(*i);
    (void)argv;
    return true;
}

}  // namespace

void printUsage() {
    std::printf(
        "CommandCapture - capture every command a script or executable\n"
        "                spawns, directly or indirectly\n"
        "\n"
        "Usage:\n"
        "  ccap run <script|exe> [args...]   run the target and capture the\n"
        "                                    whole process tree\n"
        "  ccap scan <script>                static analysis only (builtin\n"
        "                                    commands)\n"
        "  ccap compdb <subcommand>          compile command tool (recognise /\n"
        "                                    parse / export)\n"
        "\n"
        "Common options:\n"
        "  --format <json|ndjson|text>   output format (default json)\n"
        "  --out <file>                   write to a file instead of stdout\n"
        "  --cwd <dir>                    working directory for the target\n"
        "  --no-scan                      disable script static analysis\n"
        "  --no-recursive                 do not recurse into called scripts\n"
        "  --depth <n>                    static analysis recursion depth\n"
        "                                 (default 4)\n"
        "  --timeout <ms>                 run timeout\n"
        "  --quiet-ms <ms>                quiet period to wait for leftover\n"
        "                                 children after the root exits\n"
        "                                 (default 300; a safety net, not the\n"
        "                                 end condition)\n"
        "  --exclude <name>              exclude an executable name (repeatable)\n"
        "  --exclude-str <text>           exclude events whose command line\n"
        "                                 contains this substring (repeatable)\n"
        "  --include <name>              keep only this executable name\n"
        "                                 (repeatable; allowlist)\n"
        "  --tree                        print the process tree view (indented)\n"
        "  --summary                     append a summary line\n"
        "  --clear-being-debugged         clear the target's PEB\n"
        "                                 BeingDebugged flag (Windows only)\n"
        "  --stdout-to-stderr             redirect the target's stdout to stderr\n"
        "                                 (ccap's own stdout then carries only\n"
        "                                   the capture data, so --format json\n"
        "                                   can be piped straight into a\n"
        "                                   consumer)\n"
        "  --verbose                      extra diagnostics on stderr\n"
        "\n"
        "compdb options:\n"
        "  --config <file>              config file (JSON)\n"
        "  --output <file>              output file (default compile_commands.json)\n"
        "  --output-format <clang|clang-cmd|translated>\n"
        "                                 output format (default clang)\n"
        "                                   clang       compile_commands.json, the\n"
        "                                               original commands verbatim\n"
        "                                   clang-cmd   one real clang command per\n"
        "                                               line, translated from the\n"
        "                                               original compiler\n"
        "                                   translated  analyzer-style arguments\n"
        "  --keep-scripts               keep script commands (filtered by default)\n"
        "  --all                        show every command (no filtering)\n"
        "\n"
        "compdb subcommands:\n"
        "  ccap compdb capture <build cmd...>  run a build, capture compile commands\n"
        "  ccap compdb parse <file...>        parse compile commands from files\n"
        "                                      (the build-log.txt format is\n"
        "                                       detected automatically)\n"
        "  ccap compdb list                  list the known compiler types\n"
        "  ccap compdb aliases <compiler...>  show every alias of a compiler\n"
        "                                      (symlinks are recognised)\n"
        "  ccap compdb show <file...>         show the parsed compile command details\n"
        "  ccap compdb export <file...>       generate compile_commands.json\n"
        "  ccap compdb translate <file...>    generate analyzer-style translated\n"
        "                                      commands\n"
        "\n"
        "Examples:\n"
        "  ccap run build.bat\n"
        "  ccap run ./configure --prefix=/usr/local\n"
        "  ccap scan deploy.sh --format ndjson\n"
        "  ccap run build.bat --tree\n"
        "  ccap compdb list\n"
        "  ccap compdb capture -- make -j8\n"
        "  ccap compdb export build-int/build-log.txt --out compile_commands.json\n");
}

bool parse(int argc, char** argv, Options* out, std::string* err) {
    int i = 1;

    // ── top level subcommand ───────────────────────────────────────────
    if (i < argc && std::strcmp(argv[i], "run") == 0) {
        out->command = Command::Run;
        ++i;
    } else if (i < argc && std::strcmp(argv[i], "scan") == 0) {
        out->command = Command::Scan;
        ++i;
    } else if (i < argc && std::strcmp(argv[i], "compdb") == 0) {
        out->command = Command::Compdb;
        ++i;

        // ── second level subcommand ─────────────────────────────────────
        if (i < argc) {
            const char* sub = argv[i];
            if (std::strcmp(sub, "capture") == 0) {
                out->compdbCommand = CompdbCommand::Capture;
            } else if (std::strcmp(sub, "parse") == 0) {
                out->compdbCommand = CompdbCommand::Parse;
            } else if (std::strcmp(sub, "list") == 0) {
                out->compdbCommand = CompdbCommand::List;
            } else if (std::strcmp(sub, "aliases") == 0) {
                out->compdbCommand = CompdbCommand::Aliases;
            } else if (std::strcmp(sub, "show") == 0) {
                out->compdbCommand = CompdbCommand::Show;
            } else if (std::strcmp(sub, "export") == 0) {
                out->compdbCommand = CompdbCommand::Export;
            } else if (std::strcmp(sub, "translate") == 0) {
                out->compdbCommand = CompdbCommand::Translate;
            } else if (std::strcmp(sub, "help") == 0) {
                out->compdbCommand = CompdbCommand::Help;
            } else {
                *err = std::string("unknown compdb subcommand: ") + sub;
                return false;
            }
            ++i;
        } else {
            out->compdbCommand = CompdbCommand::Help;
        }
    } else {
        out->command = Command::Run;
    }

    // ─────────────────────────────────────────────────────────────────
    // Key design: **the target's own arguments must never be stolen**
    //
    //   ccap run cmake -S dir -B build
    //
    // Here -S / -B are **cmake's** options, not ccap's. If ccap parsed
    // them as its own, the build would not even start -- and the error
    // message ("unknown option: -S") points nowhere near the real problem.
    //
    // The rules (in priority order):
    //   1. Everything after `--` -> passed to the target
    //   2. **Options ccap knows** (--format / --out / --tree ...) are
    //      always consumed by ccap, wherever they appear
    //   3. A `-xxx` that ccap does not know and that appears **after**
    //      the target -> passed to the target (it is the target's option)
    //   4. A `-xxx` that ccap does not know and that appears **before**
    //      the target -> error (most likely a typo of a ccap option)
    //
    // The combination of rules 2 and 3 is the key:
    //   ccap run build.bat --format text   -> --format is consumed by ccap
    //   ccap run cmake -S dir -B build     -> -S / -B go to cmake
    //
    // The classic build wrappers use the same idea
    // (`<wrapper> --dir idir -- make -j8`), but more relaxed --
    // `--` is not mandatory here.
    // ─────────────────────────────────────────────────────────────────
    bool targetSeen = false;   // run/scan: the target has been seen
    bool afterDashdash = false;

    for (; i < argc; ++i) {
        std::string a = argv[i];

        // Everything after `--` is positional
        if (a == "--") {
            afterDashdash = true;
            continue;
        }

        if (afterDashdash) {
            if (out->command == Command::Compdb) {
                out->compdbInputs.push_back(a);
            } else if (out->target.empty()) {
                out->target = a;
                targetSeen = true;
            } else {
                out->args.push_back(a);
            }
            continue;
        }

        if (a == "--help" || a == "-h") {
            out->command = Command::Help;
            return true;
        } else if (a == "--format") {
            if (!needValue(argc, argv, &i, "--format", err)) return false;
            out->format = argv[i];
        } else if (a == "--out") {
            if (!needValue(argc, argv, &i, "--out", err)) return false;
            out->outFile = argv[i];
        } else if (a == "--cwd") {
            if (!needValue(argc, argv, &i, "--cwd", err)) return false;
            out->cwd = argv[i];
        } else if (a == "--no-scan") {
            out->scan = false;
        } else if (a == "--no-recursive") {
            out->recursive = false;
        } else if (a == "--depth") {
            if (!needValue(argc, argv, &i, "--depth", err)) return false;
            out->depth = std::atoi(argv[i]);
        } else if (a == "--timeout") {
            if (!needValue(argc, argv, &i, "--timeout", err)) return false;
            out->timeoutMs = std::atoi(argv[i]);
        } else if (a == "--quiet-ms") {
            if (!needValue(argc, argv, &i, "--quiet-ms", err)) return false;
            out->quietMs = std::atoi(argv[i]);
        } else if (a == "--exclude") {
            if (!needValue(argc, argv, &i, "--exclude", err)) return false;
            out->excludes.push_back(argv[i]);
        } else if (a == "--exclude-str") {
            if (!needValue(argc, argv, &i, "--exclude-str", err)) return false;
            out->excludeStrs.push_back(argv[i]);
        } else if (a == "--include") {
            if (!needValue(argc, argv, &i, "--include", err)) return false;
            out->includes.push_back(argv[i]);
        } else if (a == "--tree") {
            out->tree = true;
        } else if (a == "--summary") {
            out->summary = true;
        } else if (a == "--verbose") {
            out->verbose = true;
        } else if (a == "--clear-being-debugged") {
            out->clearBeingDebugged = true;
        } else if (a == "--stdout-to-stderr") {
            out->redirectOutputToStderr = true;
        } else if (a == "--config") {
            if (!needValue(argc, argv, &i, "--config", err)) return false;
            out->compdbConfig = argv[i];
        } else if (a == "--output") {
            if (!needValue(argc, argv, &i, "--output", err)) return false;
            out->compdbOutput = argv[i];
        } else if (a == "--output-format") {
            if (!needValue(argc, argv, &i, "--output-format", err)) return false;
            out->compdbFormat = argv[i];
        } else if (a == "--keep-scripts") {
            out->compdbIncludeScripts = true;
        } else if (a == "--all") {
            out->compdbShowAll = true;
        } else if (!a.empty() && a[0] == '-' && a != "-") {
            // An option ccap does not know
            if (targetSeen && out->command != Command::Compdb) {
                // After the target -> that is the target's own option
                out->args.push_back(a);
                continue;
            }
            *err = "unknown option: " + a +
                   "\n  (if this option belongs to the target, put it after "
                   "the target, or use -- to end ccap's options)";
            return false;
        } else {
            // Positional argument
            if (out->command == Command::Compdb) {
                out->compdbInputs.push_back(a);
            } else if (out->target.empty()) {
                out->target = a;
                targetSeen = true;
            } else {
                out->args.push_back(a);
            }
        }
    }

    (void)afterDashdash;
    return true;
}

}  // namespace cli
}  // namespace ccap
