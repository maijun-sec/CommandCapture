// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap - CLI argument parsing
//
// Turns argv into a structured option set.
//
// Why this is split across two files:
//   main.cpp decides *what* to run, cli.cpp decides *how* to parse the
//   arguments. The compdb subcommands have their own option set; keeping
//   them together would turn main.cpp into a junk drawer.

#pragma once

#include <string>
#include <vector>

namespace ccap {
namespace cli {

// Top level subcommand
enum class Command {
    Run,        // ccap run <target>
    Scan,       // ccap scan <script>
    Compdb,     // ccap compdb <subcommand>
    Help,
};

// compdb subcommands
enum class CompdbCommand {
    Capture,    // run a build once and capture the compile commands
    Parse,      // parse compile commands out of files
    List,       // list the known compiler types
    Aliases,    // show every alias of a compiler
    Show,       // show the details of the parsed compile commands
    Export,     // generate compile_commands.json
    Translate,  // generate analyzer-style translated commands
    Help,
};

// The complete option set
struct Options {
    Command command = Command::Run;
    CompdbCommand compdbCommand = CompdbCommand::Help;

    // run / scan
    std::string target;
    std::vector<std::string> args;

    // compdb
    std::vector<std::string> compdbInputs;   // files to parse / build command to run
    std::string compdbConfig;                // config file path
    std::string compdbOutput;                // output path
    std::string compdbFormat = "clang";      // clang | translated
    bool compdbIncludeScripts = false;      // keep script commands
    bool compdbShowAll = false;             // do not filter when showing
    std::vector<std::string> compdbExclude;  // extra exclusions
    std::vector<std::string> compdbInclude;  // allowlist
    bool compdbVerbose = false;

    // common
    std::string format = "json";
    std::string outFile;
    std::string cwd;
    bool scan = true;
    bool recursive = true;
    int depth = 4;
    int timeoutMs = 0;
    int quietMs = 300;
    bool verbose = false;
    bool clearBeingDebugged = false;
    bool redirectOutputToStderr = false;
    bool tree = false;
    bool summary = false;
    std::vector<std::string> excludes;
    std::vector<std::string> excludeStrs;
    std::vector<std::string> includes;
};

// Parse the arguments.
//
// Returns true on success; on failure err holds the reason.
bool parse(int argc, char** argv, Options* out, std::string* err);

// Print the help text
void printUsage();

}  // namespace cli
}  // namespace ccap
