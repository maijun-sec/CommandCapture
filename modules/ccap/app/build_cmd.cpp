// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap - the unified build entry
//
// WHAT THIS IS
//
// "ccap build <cmd> [args...]" runs the user's build and produces a
// compile database from **every** language the build uses.
//
// The point is that the user should not have to know which capture
// mechanism applies to which part of their build. A polyglot
// repository builds C++, Java and C# in one run, and the three need
// completely different mechanisms.
//
// THE BACKENDS
//
//   native   the process/debugger capture. It sees every process the
//            build spawns, so it covers C, C++, Rust, Go -- anything
//            that runs a compiler as a child process.
//
//   jvm      the in-process agent. The JVM compilers (javac, kotlinc,
//            scalac) run **inside** the JVM, so a process hook sees
//            only the "java" launch, not the compile. The agent hooks
//            the compiler's own entry points instead.
//
//   dotnet   the MSBuild diagnostic log. The Roslyn compiler (csc) is
//            a library the build loads **in-process**, and under the
//            default configuration it runs inside the long-lived
//            compiler server -- so it never appears as a process at
//            all. MSBuild's diagnostic log records the fully expanded
//            compiler command, which is a better source than a process
//            hook would be.
//
// WHY THE BACKENDS COMPOSE
//
// The native capture and the JVM agent already compose: the agent is
// injected into the environment of the whole process tree, and the
// native hooks run alongside it. The .NET backend is different -- it
// reads a file the build produces -- so it is a separate step that
// runs after the build.
//
// The three views are then merged into ONE compile database, because
// that is what a consumer wants: "how was every file in this
// repository compiled", not "how was the C# part compiled".

#include "build_cmd.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "ccap_compdb.h"
#include "ccap_compdb_build.h"
#include "ccap_compdb_dotnet.h"
#include "core/event.h"
#include "core/normalize.h"

namespace ccap {
namespace app {

namespace {

// Did an MSBuild run in this build?
//
// The question is asked only when the .NET backend found no compile
// command, and it decides what the user is told.
//
// A build with no MSBuild in it leaves the output of whatever else ran
// -- "ninja: no work to do.", a compiler's diagnostics, nothing. A
// build whose MSBuild ran leaves MSBuild's own output: its task
// lines, its target names, its "Build succeeded" summary.
//
// So the test is on the **content**, not the size: the size is not a
// reliable signal -- measured, a verbose MSBuild log is 5.8 MB and a
// normal one is 818 bytes, and a build with no MSBuild in it is 23
// bytes.
bool logLooksLikeMsbuildOutput(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    // The markers that prove an MSBuild ran.
    //
    // They have to survive the **localization**: the build runs in
    // whatever language the machine is set to, and the summary lines
    // are translated. So the markers are the ones that are not
    // translated:
    //
    //   "CSC :"          the compiler's own output. It is the
    //                   compiler's name, and the compiler does not
    //                   translate it. Measured: present in a log at
    //                   every verbosity.
    //   " -> "           the "output -> file" line the build prints
    //                   for every compilation it did.
    //   "MSBUILD"       the environment dump the diagnostic log starts
    //                   with.
    //
    // The English forms are tried too, for a build that runs in
    // English.
    static const char* kMarkers[] = {
        "CSC :", "Csc :", " -> ", "MSBUILD",
        "Build succeeded", "Build FAILED", "Task \"", "Target \"",
    };

    // Only the head of the file is read: the markers are in the first
    // few hundred lines, and a verbose log is megabytes.
    std::string head;
    head.resize(64 * 1024);
    in.read(&head[0], static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<size_t>(in.gcount()));

    // The log may be UTF-16, so the ASCII markers are searched in
    // both encodings: as they are, and with a zero byte after each
    // character.
    for (const char* m : kMarkers) {
        if (head.find(m) != std::string::npos) return true;

        std::string wide;
        for (const char* p = m; *p; ++p) {
            wide.push_back(*p);
            wide.push_back('\0');
        }
        if (head.find(wide) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// Run the build and collect the compile commands from every backend.
std::vector<compdb::CompileCommand> runBuild(
    const compdb::BuildPlan& plan, const BuildOptions& opts,
    const compdb::BuildOptions& planOpts, std::string* eventsNdjson,
    std::vector<std::string>* notes) {
    std::vector<compdb::CompileCommand> out;

    // ── the .NET backend ───────────────────────────────────────────────
    //
    // The stdout goes to the log file, untouched.
    //
    // This is what makes the log readable: MSBuild writes it in
    // UTF-16LE, and merging it with stderr would corrupt it.
    //
    // It is done **unconditionally** (unless the .NET backend is off),
    // because whether the build is an MSBuild one cannot be known
    // before it runs -- the build may be a script that calls
    // "dotnet build" inside it.
    //
    // EXCEPT WHEN THE FILE LOGGER DID THE JOB
    //
    //   When the build is MSBuild-compatible, the plan asked it to
    //   write the log through the file logger, and the log is already
    //   in the file. Redirecting the stdout on top of that would
    //   overwrite it -- the file logger opens the file, and so does
    //   the redirect.
    const std::string stdoutFile =
        (plan.writesMsbuildLog && !plan.logWrittenByLogger)
            ? plan.msbuildLog
            : opts.stdoutFile;

    // ── the native and JVM backends ────────────────────────────────────
    //
    // The native capture runs the build and watches the process tree.
    // The JVM agent is injected into the same tree by the capture
    // layer, so the two run together in one pass.
    //
    // The capture result is handed to the compdb parser through the
    // **same** path as every other capture, so there is only one
    // parsing implementation and two implementations cannot drift.
    std::string ndjson;
    {
        EventCollector collector;
        RunOptions ropts;
        ropts.target = plan.target;
        ropts.args = plan.args;
        ropts.cwd = opts.cwd;
        ropts.scanScript = false;     // only compile commands matter
        ropts.timeoutMs = opts.timeoutMs;
        ropts.quietMs = opts.quietMs;
        ropts.redirectOutputToStderr = true;
        ropts.agentJar = opts.agentJar;
        ropts.agentCaptureDir = opts.agentCaptureDir;
        ropts.stdoutFile = stdoutFile;

        RunResult rr = runCapture(ropts, &collector);
        if (rr.exitCode != 0 && notes) {
            notes->push_back("the build exited with " +
                             std::to_string(rr.exitCode));
        }

        for (const auto& e : collector.deduped()) {
            ndjson += ccap::eventToJson(e);
            ndjson += "\n";
        }
    }

    if (eventsNdjson) *eventsNdjson = ndjson;

    {
        compdb::CommandParser parser;
        compdb::CompilerTable table = compdb::CompilerTable::builtin();
        parser.setCompilerTable(table);

        auto cmds = parser.parseCaptureEvents(ndjson);

        // The agent's view wins where both have an entry.
        if (!opts.agentCaptureDir.empty()) {
            auto fromAgent =
                parser.parseJvmCaptureDirectory(opts.agentCaptureDir);
            cmds = compdb::mergeJvmCommands(cmds, fromAgent);
        }

        for (auto& c : cmds) out.push_back(c);
    }

    // ── the .NET backend ───────────────────────────────────────────────
    //
    // Now that the build has run, the diagnostic log is there.
    //
    // Whether the build was an MSBuild one is decided **by the log
    // itself**: a build that was not an MSBuild one has no compiler
    // task in it. That is the honest test, and it is the only one that
    // works for a wrapper script.
    //
    // So a log without a compiler task is NOT an error -- it means the
    // build simply had no .NET part. The note says so, and the run
    // continues with the native and JVM backends' results.
    //
    // THE WRAPPER CASE
    //
    //   A script build that got no MSBuild switches cannot have a
    //   diagnostic log with a compiler command in it: the inner MSBuild
    //   was not verbose. So an empty log here means "the .NET half is
    //   missing", not "there was no .NET part" -- and the two must not
    //   be reported the same way.
    //
    //   The two are told apart by the **size** of the log. A build with
    //   no MSBuild in it leaves almost nothing behind -- measured: 23
    //   bytes, "ninja: no work to do.". A build whose MSBuild was
    //   there but not verbose leaves the whole verbose output -- but
    //   only if something made it verbose, which for a wrapper means
    //   the user handed it the switches.
    //
    //   So the log's size is the test, and it is what tells "this build
    //   has no .NET part" apart from "the .NET part is missing".
    if (!plan.msbuildLog.empty() && !planOpts.noDotnet) {
        std::vector<compdb::DotnetCompile> dotnet;
        std::string err;
        if (compdb::extractDotnetCompiles(plan.msbuildLog, &dotnet, &err)) {
            auto cmds = compdb::toCompileCommands(dotnet);
            for (auto& c : cmds) {
                if (c.directory.empty()) c.directory = opts.cwd;
                out.push_back(c);
            }
            if (notes) {
                notes->push_back("the .NET backend read " +
                                 std::to_string(dotnet.size()) +
                                 " compile command(s) from the build log");
            }
        } else if (plan.isWrapper && plan.wrapperNeedsMsbuildArgs) {
            // A script build, and the user DID hand it the MSBuild
            // switches -- so an MSBuild really ran, and its log really
            // should have had a compiler command in it. Something else
            // went wrong, and the note says what was tried.
            if (notes) {
                notes->push_back(
                    "the .NET backend found no compile command in the log "
                    "the build was asked to write. The switches were "
                    "handed to the script, so check that it forwards "
                    "them to the MSBuild it calls, and that the MSBuild "
                    "accepts them");
            }
        } else if (plan.isWrapper && !plan.wrapperNeedsMsbuildArgs &&
                   logLooksLikeMsbuildOutput(plan.msbuildLog)) {
            // A script build that was NOT given the MSBuild switches.
            //
            //   The inner MSBuild was not verbose, so its log has no
            //   compiler command in it. That is a different thing from
            //   "this build has no .NET part", and it is actionable --
            //   so the message says what to do about it.
            //
            //   The log has to look like MSBuild output for this to be
            //   the right diagnosis. A build with no MSBuild in it at
            //   all leaves a log of a few bytes, and there is nothing
            //   to advise about.
            if (notes) {
                notes->push_back(
                    "the .NET backend found no compile command. This is a "
                    "script build, and the MSBuild it calls was not "
                    "verbose. Pass --msbuild-args to hand it \"-v:diag "
                    "-flp:logfile=<file>;verbosity=diag\", and make sure "
                    "the script forwards its arguments");
            }
        } else if (notes) {
            notes->push_back("the .NET backend: no .NET compile in this build (" +
                             err + ")");
        }
    }

    return out;
}

}  // namespace app
}  // namespace ccap
