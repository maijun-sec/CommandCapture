// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - the .NET backend
//
// WHY A SEPARATE BACKEND
//
// A .NET build does not spawn the compiler the way a native build does.
//
// The Roslyn compiler (csc) is a **library** that the build driver
// loads **in-process**. Under the default configuration
// (UseSharedCompilation=true, which is what the SDK sets) the compile
// happens inside the long-lived compiler server (VBCSCompiler), so
// there is no process creation to hook at all: the compiler never
// appears as a child process.
//
// Even with the shared compilation turned off, the compiler is invoked
// as
//
//     dotnet exec <sdk>/Roslyn/bincore/csc.dll /noconfig @<rsp>
//
// and the entire option list lives in a **response file**. A process
// hook would see the reference to the response file and nothing else.
//
// So this backend does not hook processes. It reads the build log.
//
// THE SOURCE OF TRUTH: THE DIAGNOSTIC LOG
//
// MSBuild's diagnostic log records the **fully expanded** command line
// of every task it runs. The Csc task's command line is there in full:
// every option, every source file, every reference -- with the response
// file already expanded by MSBuild.
//
// That is a better source than a process hook, because it needs none
// of the workarounds:
//   * it does not need UseSharedCompilation=false;
//   * it does not need the response file to be expanded;
//   * it records what the build **decided**, not what some process
//     happened to be launched with.
//
// The cost is that it only covers builds that go through MSBuild. A
// direct "csc" invocation is not seen here -- but that is the rarer
// case, and the native backend still sees it as a process.

#ifndef CCAP_COMPDB_DOTNET_BACKEND_H
#define CCAP_COMPDB_DOTNET_BACKEND_H

#include <string>
#include <vector>

#include "ccap_compdb.h"

namespace ccap {
namespace compdb {

// The .NET compile commands extracted from an MSBuild diagnostic log.
struct DotnetCompile {
    // The compiler's argv: argv[0] is the host ("dotnet.exe"), argv[1]
    // is "exec", argv[2] is the compiler dll.
    std::vector<std::string> argv;

    // The working directory the task ran in. The command line uses
    // **relative** paths, so this is not optional: without it the
    // sources cannot be resolved.
    std::string directory;

    // The compiler dll (csc.dll / vbc.dll / fsc.dll).
    std::string compilerPath;

    // The language: "C#", "Visual Basic", "F#".
    std::string language;
};

// Extract the Csc/Vbc/Fsc task command lines from an MSBuild
// diagnostic log.
//
// Returns false when the file is not a diagnostic log at all (no Csc
// task in it), which is different from "the log has no compile" -- the
// caller reports the two differently.
//
// WHY THE LOG ENCODING MATTERS
//
// MSBuild writes the diagnostic log as **UTF-16LE with a BOM** when its
// stdout is redirected to a file or a pipe, and as UTF-8 when it goes
// to a console. Reading it as UTF-8 when it is UTF-16 does not fail --
// it silently produces garbage, and the ASCII runs ("csc.dll") can be
// lost entirely. That looks exactly like "the log has no compile
// command in it", which is the wrong conclusion.
//
// So the encoding is detected from the BOM.
bool extractDotnetCompiles(const std::string& logPath,
                           std::vector<DotnetCompile>* out,
                           std::string* error);

// Convert the extracted compiles into the common CompileCommand form.
std::vector<CompileCommand> toCompileCommands(
    const std::vector<DotnetCompile>& compiles);

// Is this command line an MSBuild invocation?
//
// The .NET backend is only enabled for a command that goes through
// MSBuild, because only MSBuild writes the diagnostic log that the
// backend reads. Everything else is left to the native backend.
bool isMsBuildCommand(const std::vector<std::string>& argv);

// Split a Windows command line into argv.
//
// The rules are the ones the Windows command line parser uses:
//   * whitespace separates arguments OUTSIDE a quoted run;
//   * a double quote toggles the quoted state;
//   * a run of backslashes before a quote: the quote is literal unless
//     the backslashes are doubled;
//   * a doubled quote inside a quoted run is a literal quote.
//
// The backslash rule matters: a path like
// "C:\\Program Files\\dotnet\\dotnet.exe" contains a backslash run, and
// a naive splitter gets it wrong.
std::vector<std::string> splitWindowsCommandLine(const std::string& line);

}  // namespace compdb
}  // namespace ccap

#endif  // CCAP_COMPDB_DOTNET_BACKEND_H
