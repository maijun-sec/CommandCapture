// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// CommandCapture - Platform abstraction layer
#pragma once

#include <string>
#include <vector>

#include "core/event.h"

namespace ccap {

// Capture options (unified across platforms)
struct RunOptions {
    std::string target;                 // script path or executable file
    std::vector<std::string> args;      // arguments passed to the target
    std::string cwd;                    // working directory
    bool scanScript = true;            // whether to do script static analysis
    bool scriptRecursive = true;
    int scriptDepth = 4;
    bool quiet = false;                 // suppress the target's standard output
    int timeoutMs = 0;
    int quietMs = 300;
    bool verbose = false;

    // Windows only: zero the BeingDebugged flag in the target's PEB.
    // DEBUG_PROCESS always makes the target see BeingDebugged == 1,
    // and anti-debugging logic changes its behaviour based on that. When
    // enabled, IsDebuggerPresent() returns 0.
    // CheckRemoteDebuggerPresent() queries the kernel, it cannot be patched.
    bool clearBeingDebugged = false;

    // Redirect the target's stdout to stderr.
    // ccap's own stdout keeps only the capture data, so the JSON can be
    // consumed directly from a pipe.
    bool redirectOutputToStderr = false;

    // Redirect the target's stdout to this **file**.
    //
    // WHY A FILE AND NOT A PIPE
    //
    // The MSBuild diagnostic log is written to stdout, and it is
    // **UTF-16LE** -- so the bytes have to reach a file untouched. A
    // pipe would work too, but the reader on the other end has to
    // drain it while the build runs, and a build that writes 10 MB of
    // diagnostic output would block until the reader catches up.
    //
    // WHY NOT MERGE WITH STDERR
    //
    // Because the merge corrupts the log. The two streams have
    // different encodings -- MSBuild writes UTF-16, the console
    // messages are in the console code page -- and interleaving them
    // produces a file that decodes to garbage. Measured: the Csc task
    // line was unreadable after a 2>&1 redirect, while the same build
    // with a pure stdout redirect produced a log that parsed cleanly.
    //
    // When it is empty, the stdout is not redirected.
    std::string stdoutFile;

    // The JVM capture agent to attach to the target, and the directory its
    // captures go into.
    //
    // WHY AN ENVIRONMENT VARIABLE AND NOT A COMMAND LINE ARGUMENT
    //
    //   The obvious way to attach an agent is to put "-javaagent:..." on
    //   the command line. That does not work for most of the commands
    //   that matter, and the reason is worth writing down:
    //
    //     * `javac` is a program, not a JVM launcher. Its own option
    //       parser rejects "-javaagent" with "invalid flag".
    //     * `mvn` and `gradle` do not pass a "-javaagent" they were
    //       given to the JVM that does the compiling. Gradle's
    //       `org.gradle.jvmargs` places the argument *after* "-cp",
    //       where the JVM treats it as an application argument rather
    //       than a JVM option -- it fails with "Unrecognized option".
    //
    //   JAVA_TOOL_OPTIONS has neither problem. The JVM reads it before
    //   it starts, so its position in the command line cannot matter,
    //   and every java command reads it -- java, javac, javap, jar,
    //   javadoc, jlink, jshell, jcmd and jpackage, all measured.
    //
    // WHY THE ENVIRONMENT IS THE RIGHT PLACE ON WINDOWS TOO
    //
    //   On Windows the command line of a child process cannot be
    //   changed after the fact: by the time the debugger sees
    //   CREATE_PROCESS_DEBUG_EVENT the PEB is already mapped and the
    //   command line has been handed to the C runtime. So the argument
    //   cannot be rewritten the way it can be on Linux, where the
    //   execve hook owns the argv.
    //
    //   The environment does not have that problem, because it is
    //   inherited: a process that has JAVA_TOOL_OPTIONS passes it to
    //   every process it starts. Measured: mvn -> java -> javac and
    //   gradle -> daemon -> javac both carry it through.
    //
    //   So injecting it once, into the environment of the root
    //   process, covers the whole process tree.
    //
    // The jar path and the capture directory. When agentJar is empty,
    // no agent is attached.
    std::string agentJar;

    // The directory the agent writes its captures into.
    std::string agentCaptureDir;

    // The MSBuild diagnostic log to produce, and where to put it.
    //
    // MSBuild reads the log file path from the MSBUILDLOGTEXT
    // environment variable, so setting that variable in the environment
    // of the root process makes the build write the log by itself --
    // without the build command being touched.
    //
    // WHY THE ENVIRONMENT AND NOT A COMMAND LINE FLAG
    //
    //   The build command belongs to the user. Adding "-v:diag" to it
    //   means guessing where the flag goes, and it breaks entirely when
    //   the build is not MSBuild at all -- a wrapper script, an IDE
    //   build, a solution that is built by a different tool.
    //
    //   The environment variable is invisible to the build, so it
    //   cannot be misplaced, and it is inherited by every process in
    //   the tree -- so MSBuild finds it wherever it runs.
    //
    // When it is empty, no log is produced.
    std::string msbuildLogFile;
};

// Capture result
struct RunResult {
    int exitCode = -1;
    bool timedOut = false;
    bool ok = true;
    std::string error;
    std::vector<std::string> notes;
};

// The raw result returned by a platform capture implementation.
// Each platform implementation (windows_debugger / linux_preload) defines its
// own CaptureResult,
// but the field semantics are identical, which makes it easy for
// platform.cpp to do uniform error handling.
struct CaptureResult {
    int exitCode = -1;
    int rootPid = 0;
    bool timedOut = false;
    std::string error;
    std::vector<std::string> notes;
};

// Execute the target and collect command events. The events are written to
// collector.
RunResult runCapture(const RunOptions& opts, EventCollector* collector);

// Create a directory, and every missing directory above it.
//
// The JVM agent needs a directory to write its captures into, and it does
// not create it. When the user did not name one, ccap makes a directory
// of its own -- so this lives here rather than in core, because core
// must not call platform APIs.
//
// Returns true when the directory exists afterwards.
bool createDirectoryTree(const std::string& path);

// The current platform
Platform currentPlatform();

// Platform name (used for output)
const char* currentPlatformName();

}  // namespace ccap
