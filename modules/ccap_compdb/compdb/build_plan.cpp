// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - the unified build plan
//
// See ccap_compdb_build.h for the design rationale.

#include "ccap_compdb_build.h"

#include <cctype>
#include <ctime>

#include "ccap_compdb_dotnet.h"

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace ccap {
namespace compdb {

namespace {

// The process id of this process.
//
// It is part of the unique path: two processes cannot share a pid at
// the same time, so two runs in the same second get different paths.
long long currentProcessId() {
#if defined(_WIN32)
    return static_cast<long long>(::GetCurrentProcessId());
#else
    return static_cast<long long>(::getpid());
#endif
}

// The temporary directory.
std::string tempDir() {
#if defined(_WIN32)
    char base[MAX_PATH + 1] = {0};
    const DWORD got = ::GetTempPathA(MAX_PATH, base);
    if (got == 0) return std::string(".");
    std::string dir(base);
    if (!dir.empty() && dir.back() == '\\') dir.pop_back();
    return dir;
#else
    return std::string("/tmp");
#endif
}

// A path that no other run uses.
//
// The clock and the pid are enough: two processes cannot share a pid
// at the same time, and two runs in the same second get different
// pids.
std::string uniquePath(const char* stem, const char* extension) {
    const long long stamp = static_cast<long long>(::time(nullptr));
    std::string name = std::string(stem) + "-" + std::to_string(stamp) + "-" +
                       std::to_string(currentProcessId()) +
                       extension;
#if defined(_WIN32)
    return tempDir() + "\\" + name;
#else
    return tempDir() + "/" + name;
#endif
}

// Lower-case an ASCII string.
//
// The build command line is compared case-insensitively for the
// verbosity switch: MSBuild accepts "-v:", "/v:" and "--verbosity",
// and a build that writes "-V:diag" is the same switch.
std::string lowerAscii(const std::string& s) {
    std::string out = s;
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// Does the command line already carry this switch?
bool hasSwitch(const std::vector<std::string>& commandLine,
               const char* const* prefixes, size_t count) {
    for (const auto& a : commandLine) {
        const std::string lower = lowerAscii(a);
        for (size_t i = 0; i < count; ++i) {
            const std::string p = lowerAscii(prefixes[i]);
            if (lower == p ||
                (lower.size() > p.size() &&
                 lower.compare(0, p.size(), p) == 0)) {
                return true;
            }
        }
    }
    return false;
}

// The base name of a path, without the directories.
//
// The command may be an absolute path ("C:\\Program Files\\LLVM\\bin\\
// clang-cl.exe"), so the comparison has to be on the last component.
std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Strip the executable suffix, so "clang-cl.exe" becomes "clang-cl".
std::string stripExeSuffix(const std::string& name) {
    const std::string lower = lowerAscii(name);
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".exe") == 0) {
        return name.substr(0, name.size() - 4);
    }
    return name;
}

// Split a string on spaces, honoring the double quotes.
//
// The MSBuild switches the user supplies are a single string, and one
// of them carries a path with spaces in it: "-flp:logfile=C:\\my
// dir\\build.log;verbosity=diag". So the split has to keep the quoted
// parts together.
std::vector<std::string> splitArgs(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    bool inQuotes = false;
    bool has = false;

    for (char c : text) {
        if (c == '"') {
            inQuotes = !inQuotes;
            has = true;
            continue;
        }
        if (!inQuotes && (c == ' ' || c == '\t')) {
            if (has) {
                out.push_back(current);
                current.clear();
                has = false;
            }
            continue;
        }
        current.push_back(c);
        has = true;
    }
    if (has) out.push_back(current);
    return out;
}

// The log path out of a file logger switch, if there is one.
//
// The switch is "-flp:logfile=<path>;verbosity=diag" -- the path comes
// first, and the parameters after the semicolons are the logger's own.
//
// The path may be quoted, and it may have a semicolon in it -- a
// Windows path does not, but the quoting has to be handled anyway.
std::string fileLoggerPath(const std::vector<std::string>& args) {
    static const char* kPrefixes[] = {
        "-flp:logfile=", "/flp:logfile=", "-flp:LogFile=",
        "/flp:LogFile=",
    };
    for (const auto& a : args) {
        for (const char* p : kPrefixes) {
            const size_t n = std::char_traits<char>::length(p);
            if (a.size() < n) continue;
            if (lowerAscii(a.substr(0, n)) != lowerAscii(p)) continue;

            std::string value = a.substr(n);
            // The parameters after the semicolon are the logger's own.
            const size_t semi = value.find(';');
            if (semi != std::string::npos) value = value.substr(0, semi);
            // The quotes are the shell's, not part of the path.
            if (value.size() >= 2 && value.front() == '"' &&
                value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
            if (!value.empty()) return value;
        }
    }
    return std::string();
}

// What kind of build command is this?
enum class CommandKind {
    // The command IS an MSBuild-compatible build: "dotnet build",
    // "msbuild", "devenv". It understands the MSBuild switches, so they
    // can be handed to it directly.
    MsBuild,

    // The command runs a compiler driver: "gcc", "clang", "cl". It
    // cannot have a .NET part, and it rejects unknown switches.
    NativeCompiler,

    // The command is a build tool or a script: "make", "ninja",
    // "cmake", "cmd /c build.bat", "sh build.sh".
    //
    // It may run an MSBuild build inside it, and it may not. The
    // switches it understands are its own, not MSBuild's.
    Wrapper,
};

// Classify the build command.
//
// WHY THIS IS NOT "IS IT AN MSBUILD BUILD"
//
//   A binary decision is not enough, because the two questions have
//   different answers:
//
//   * "may this build contain a .NET part" -- the wrapper case says
//     yes, because the script may call "dotnet build" inside it.
//
//   * "may I hand this command an MSBuild switch" -- the wrapper case
//     says NO. "cmake --build -v:diag" fails with "Unknown argument
//     -v:diag". Measured: injecting the switch into a "cmake --build"
//     broke the build outright.
//
//   So the command is classified, and each class gets the treatment it
//   can survive.
//
// THE CLASSIFICATION
//
//   MsBuild         the MSBuild-compatible entry points. They take the
//                   switches.
//
//   NativeCompiler the compiler drivers. They take nothing, and they
//                   have no .NET part.
//
//   Wrapper         everything else. This is the default on purpose:
//                   a command this function has never heard of gets
//                   the cautious treatment, because the cost of a
//                   wrongly redirected stdout is lower than the cost
//                   of a silently empty .NET capture.
CommandKind classifyCommand(const std::vector<std::string>& commandLine) {
    if (commandLine.empty()) return CommandKind::Wrapper;

    const std::string name =
        stripExeSuffix(baseName(commandLine[0]));

    // The MSBuild-compatible entry points.
    //
    // "dotnet" is one of them only when it is asked to BUILD: "dotnet
    // test" and "dotnet run" are not builds, and "dotnet exec" is not
    // one either. The verbs are checked for that reason.
    if (name == "dotnet" || name == "msbuild" || name == "devenv") {
        if (name == "dotnet") {
            // "dotnet build", "dotnet msbuild", "dotnet publish".
            static const char* kBuildVerbs[] = {
                "build", "msbuild", "publish", "pack",
            };
            for (const auto& a : commandLine) {
                const std::string lower = lowerAscii(a);
                for (const char* v : kBuildVerbs) {
                    if (lower == v) return CommandKind::MsBuild;
                }
            }
            return CommandKind::Wrapper;
        }
        return CommandKind::MsBuild;
    }

    // The native compiler drivers.
    //
    // The list is the set the compdb parser recognizes as compilers.
    // "g++" and "gcc" are separate names on purpose: they are
    // different drivers, and neither one spawns a build system.
    static const char* kNativeCompilers[] = {
        "clang", "clang++", "clang-cl", "gcc", "g++", "cc", "c++",
        "cl", "icc", "icpc", "clangg", "clang-cpp",
    };
    for (const char* c : kNativeCompilers) {
        if (name == c) return CommandKind::NativeCompiler;
    }

    return CommandKind::Wrapper;
}

}  // namespace

BuildPlan planBuild(const std::string& target,
                     const std::vector<std::string>& args,
                     const BuildOptions& opts) {
    BuildPlan plan;

    plan.target = target;
    plan.args = args;
    plan.commandLine.push_back(target);
    for (const auto& a : args) plan.commandLine.push_back(a);

    // Is this an MSBuild build?
    //
    // The check is on the **command**, not on the file extensions: a
    // .csproj can be built by "dotnet build", by "msbuild", and by
    // "devenv", and a C++ project can be built by "msbuild" too. So
    // the decision has to be made on what the command is.
    plan.isMsBuild = !opts.noDotnet &&
                     isMsBuildCommand(plan.commandLine);

    // The command's class.
    //
    // It decides what the build can be given: an MSBuild build takes
    // the MSBuild switches, a compiler driver takes nothing, and a
    // wrapper takes only what its own script forwards.
    const CommandKind kind = classifyCommand(plan.commandLine);
    plan.isWrapper = (kind == CommandKind::Wrapper);

    // The .NET backend needs a log to read.
    //
    // THE LOG IS PRODUCED BY THE BUILD, AND THE BUILD HAS TO BE TOLD
    // TO PRODUCE IT
    //
    //   There is no environment variable for the log: the one that
    //   exists is not honored, and the file logger is a **command
    //   line** switch. So the build command has to carry it.
    //
    //   That is only possible when the command understands MSBuild
    //   switches. "cmake --build -v:diag" fails with "Unknown
    //   argument -v:diag" -- measured. So the switches go ONLY to an
    //   MSBuild-compatible command.
    //
    //   For a wrapper, the switches cannot be injected: the wrapper
    //   has its own argument syntax, and "cmd /c build.bat -v:diag"
    //   passes "-v:diag" to the script, which may or may not forward
    //   it. The wrapper is handled by the **stdout** instead -- see
    //   below.
    //
    // WHY THE FILE LOGGER AND NOT THE STDOUT
    //
    //   MSBuild writes its diagnostic log to the **stdout**, in
    //   UTF-16LE, and the console messages go to the stderr in the
    //   console code page. So capturing the stdout means redirecting
    //   it to a file, and the build's own output stops reaching the
    //   terminal.
    //
    //   The file logger writes the log to a file **directly**, in
    //   UTF-8, and leaves the stdout alone. Measured: "dotnet build
    //   -flp:logfile=..." produced a 5.8 MB log with the fully
    //   expanded Csc command in it, and the build's output still
    //   reached the terminal.
    //
    //   The stdout capture is kept as the fallback for the wrapper
    //   case, where the log cannot be requested by a switch.
    if (!opts.noDotnet) {
        // The diagnostic log path.
        //
        // A given log is used as it is; otherwise one is planned.
        //
        // A compiler driver gets no log at all: it certainly has no
        // .NET part, and the stdout redirect is not free -- the
        // build's output would go into a file nobody reads.
        if (!opts.msbuildLog.empty()) {
            plan.msbuildLog = opts.msbuildLog;
            plan.writesMsbuildLog = false;
        } else if (kind != CommandKind::NativeCompiler) {
            plan.msbuildLog = uniquePath("ccap-msbuild", ".log");
            plan.writesMsbuildLog = true;
        }

        // The verbosity, for an MSBuild-compatible command.
        //
        // A verbosity the user chose is **kept**: if they asked for
        // "minimal", the build is quieter, and the log may not have the
        // Csc task in it. That is their choice, and the backend reports
        // it when the log turns out to have no compile in it.
        if (kind == CommandKind::MsBuild) {
            // The file logger, which is how the log reaches a file
            // without hijacking the stdout.
            //
            // The **logger** carries the diagnostic verbosity, not the
            // build. The build's own verbosity stays where the user
            // put it: raising it to diagnostic would print the whole
            // log to the console -- measured: 100k lines of task
            // output, which drowns everything else.
            //
            // The file logger's verbosity is set on the logger itself,
            // so the two are independent.
            if (plan.writesMsbuildLog) {
                const std::string flp =
                    "-flp:logfile=" + plan.msbuildLog + ";verbosity=diag";
                plan.commandLine.push_back(flp);
                plan.args.push_back(flp);
                plan.logWrittenByLogger = true;
            } else {
                // No logger: the log has to come from the build's own
                // output, so the build has to be verbose.
                static const char* kVerbositySwitches[] = {
                    "-v:", "/v:", "-v", "/v", "-verbosity", "/verbosity",
                    "--verbosity",
                };
                if (!hasSwitch(plan.commandLine, kVerbositySwitches,
                               sizeof(kVerbositySwitches) / sizeof(char*))) {
                    plan.commandLine.push_back("-v:diag");
                    plan.args.push_back("-v:diag");
                }
            }
        } else if (kind == CommandKind::Wrapper) {
            // A WRAPPER BUILD
            //
            //   The script may call "dotnet build" inside it, and the
            //   log has to come from that inner build. MSBuild's
            //   verbosity and its file logger are command line
            //   switches -- there is no environment variable for
            //   either of them, and ccap cannot add a switch to a
            //   command it is not running.
            //
            //   So the user says what the script accepts, and ccap adds
            //   those switches to the wrapper's own command line. A
            //   script that forwards its arguments ("%*" in a batch
            //   file, "$@" in a shell script) passes them on to the
            //   MSBuild it calls.
            //
            //   Without this, the wrapper's .NET half is silently
            //   missing -- and a silent miss is the worst outcome for a
            //   capture tool. So the note says so.
            const std::vector<std::string> extra = splitArgs(opts.msbuildArgs);
            for (const auto& a : extra) {
                plan.commandLine.push_back(a);
                plan.args.push_back(a);
            }
            plan.wrapperNeedsMsbuildArgs = !opts.msbuildArgs.empty();

            // The user's own file logger, if they gave one.
            //
            // When the switches carry a "-flp:logfile=..." the log
            // lands in the user's file, and that is the file to read.
            // Reading the stdout copy as well would work, but it would
            // capture the same log twice -- once through the logger and
            // once through the stdout redirect -- and the second copy
            // is the expensive one: it takes the build's output away
            // from the user.
            const std::string userLog = fileLoggerPath(extra);
            if (!userLog.empty()) {
                plan.msbuildLog = userLog;
                plan.writesMsbuildLog = false;
                plan.logWrittenByLogger = true;
            }
        }

        // The incremental switch, unless the user already decided.
        //
        // WHY IT IS FORCED
        //
        //   An **up-to-date** build compiles nothing: the Csc task is
        //   never run, so the log has no compiler command in it and the
        //   capture comes out **empty**.
        //
        //   That is the worst possible outcome for a capture tool: it
        //   looks like it worked, and it produced nothing. Measured: a
        //   second "dotnet build" of an already-built project wrote a
        //   5 MB diagnostic log with no Csc task in it at all.
        //
        //   The same is true for the native compilers: an up-to-date
        //   ninja build compiles nothing either.
        //
        //   So the build is forced to run the compiler. The cost is
        //   that the build is slower -- which is the right trade for a
        //   tool whose whole job is to observe the compiles.
        //
        //   It goes only to an MSBuild-compatible command: "--no-
        //   incremental" is an MSBuild switch, and "cmake --build
        //   --no-incremental" fails the same way "-v:diag" does.
        //
        //   The native side has its own answer: there is no universal
        //   switch for it, so the caller asks for a clean build.
        if (kind == CommandKind::MsBuild || plan.wrapperNeedsMsbuildArgs) {
            static const char* kIncrementalSwitches[] = {
                "--no-incremental", "-p:incremental", "/p:incremental",
                "-p:buildprojectreferences",
            };
            if (!hasSwitch(plan.commandLine, kIncrementalSwitches,
                           sizeof(kIncrementalSwitches) / sizeof(char*))) {
                plan.commandLine.push_back("--no-incremental");
                plan.args.push_back("--no-incremental");
            }
        }
    }

    return plan;
}

}  // namespace compdb
}  // namespace ccap
