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
    Build,      // ccap build <cmd> -- the unified capture entry
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
    Map,        // map the compile commands to clang, at a given level
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
    std::string compdbFormat = "clang";      // clang | translated | clang-cmd
    bool compdbIncludeScripts = false;      // keep script commands
    bool compdbShowAll = false;             // do not filter when showing
    std::vector<std::string> compdbExclude;  // extra exclusions
    std::vector<std::string> compdbInclude;  // allowlist
    bool compdbVerbose = false;

    // The retention level of the generated clang commands.
    //
    // "ast" -- the arguments needed for a correct clang AST
    // "bc"  -- the arguments needed for a correct LLVM bitcode file
    std::string compdbLevel = "ast";

    // Show the mapping report (what was kept, what was dropped and why)
    // alongside the generated commands.
    bool compdbExplain = false;

    // Map an unknown compiler to a profile.
    //
    // The value is "<compiler-name>=<profile-id>", where the compiler
    // name may end with '*' for a prefix match.
    //
    // Example: --map-compiler "cc=gnu"
    std::vector<std::string> compdbMapCompiler;

    // The profile to use for a compiler that is not recognised.
    //
    // Example: --default-profile gnu
    std::string compdbDefaultProfile;

    // The compiler version, for the version-scoped rules.
    //
    // The tool never runs the compiler by itself -- a build log from
    // another machine has no toolchain here -- so the version comes
    // from the caller.
    //
    // Example: --compiler-version 11.4.0
    std::string compdbCompilerVersion;

    // What to do with an option the switch table does not know.
    //
    // "report" (the default) drops it from the generated command but
    // records it in the audit report. "keep" passes it through
    // unchanged. "drop" drops it silently. "fail" makes the mapping
    // fail.
    //
    // Example: --unknown-policy keep
    std::string compdbUnknownPolicy;

    // The executable-name match patterns to add.
    //
    // The value is "<pattern>=<target>", where the pattern is a glob
    // ("*-gcc") or a redirect ("replace/my-cc/g++").
    //
    // Example: --compiler-match "*-gcc=gnu"
    std::vector<std::string> compdbCompilerMatch;

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

    // ── The unified build entry ─────────────────────────────────────────
    //
    // "ccap build <cmd> [args...]" runs the user's build and produces a
    // compile database from **every** language the build uses.
    //
    // The backends are:
    //
    //   native   the process/debugger capture. Covers C, C++, Rust,
    //            Go, and anything else that spawns a compiler.
    //   jvm      the in-process agent. Covers Java, Kotlin, Scala.
    //   dotnet   the MSBuild diagnostic log. Covers C#, VB, F#.
    //
    // All three are enabled at once: a polyglot repository builds all
    // of them in one run, and the user should not have to know which
    // backend applies to which part of the build.
    //
    // Which ones are actually used is decided per run, from what the
    // build turns out to be.
    //
    // The output path of the generated compile database.
    std::string buildOutput;

    // The output format: "clang" (the compilation database),
    // "translated" (the analyzer-style commands), "clang-cmd" (the
    // clang command lines).
    std::string buildFormat = "clang";

    // Keep the capture events as well, not just the compile database.
    //
    // The events are the raw process tree; the compile database is the
    // distilled view. A consumer that wants "what did this build run"
    // needs the events, and one that wants "how was this file
    // compiled" needs the database.
    bool buildEvents = false;

    // The path of the MSBuild diagnostic log to read.
    //
    // When it is empty, the .NET backend writes one itself (by adding
    // the verbosity flag to the build command) and reads it back. When
    // it is given, that log is read instead -- which is how a build
    // that was captured elsewhere is brought in.
    std::string buildMsbuildLog;

    // The MSBuild switches to hand to a **wrapper** build.
    //
    // WHY THIS IS NEEDED
    //
    //   MSBuild's verbosity and its file logger are **command line**
    //   switches: there is no environment variable for either of them.
    //   Measured: MSBUILDVERBOSITY, MSBUILDLOGTEXT and
    //   MSBuildResponseFiles are all ignored.
    //
    //   So the switches have to reach the MSBuild command, and for a
    //   wrapper ccap cannot know where they belong: "cmake --build
    //   -v:diag" fails with "Unknown argument -v:diag", and so does
    //   "cmake --build --no-incremental".
    //
    //   ccap therefore does NOT guess. The user says what the script
    //   accepts, and the switches are added to the wrapper's own
    //   command line -- where a script that forwards its arguments
    //   ("%*" in a batch file, "$@" in a shell script) passes them on
    //   to the MSBuild it calls.
    //
    //   The value is a single string, split on spaces.
    std::string buildMsbuildArgs;

    // Do not run the .NET backend even when the build looks like an
    // MSBuild one.
    bool buildNoDotnet = false;

    // The JVM capture agent to attach to the target, and the directory its
    // captures go into.
    //
    // When the agent jar is given, ccap attaches it to the whole process
    // tree by putting JAVA_TOOL_OPTIONS into the environment of the root
    // process -- see core/agent_inject.h for why the environment and not
    // the command line.
    std::string agentJar;
    std::string agentCaptureDir;
};

// Parse the arguments.
//
// Returns true on success; on failure err holds the reason.
bool parse(int argc, char** argv, Options* out, std::string* err);

// Print the help text
void printUsage();

}  // namespace cli
}  // namespace ccap
