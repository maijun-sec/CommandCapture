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
// The plan is the part of the unified entry that decides **what has to
// happen**, without doing any of it.
//
// It is separated from the run for two reasons:
//
//   * it is pure, so it can be **tested** without starting a build --
//     and the decisions it makes are the interesting ones;
//   * the run may have to change its mind (a build turns out not to be
//     an MSBuild one), and a plan that is recomputed is clearer than a
//     plan that is mutated.
//
// It lives in the compdb module rather than in the app, because every
// decision it makes is about the **compilers**: which ones are going to
// run, and how the build command has to be shaped for them to be
// visible.

#ifndef CCAP_COMPDB_BUILD_H
#define CCAP_COMPDB_BUILD_H

#include <string>
#include <vector>

namespace ccap {
namespace compdb {

// The options of the unified build entry.
struct BuildOptions {
    // The MSBuild diagnostic log to read.
    //
    // When it is empty and the build turns out to be an MSBuild one,
    // the backend produces a log of its own.
    std::string msbuildLog;

    // Do not run the .NET backend at all.
    bool noDotnet = false;

    // The MSBuild switches to hand to a **wrapper** build.
    //
    // MSBuild's verbosity and its file logger are command line
    // switches: there is no environment variable for either of them.
    // So for a wrapper -- a script that calls "dotnet build" inside it
    // -- the switches have to be added to the wrapper's own command
    // line, and only the user knows whether the script forwards them.
    //
    // The value is a single string, split on spaces.
    std::string msbuildArgs;
};

// What the unified entry worked out it has to do.
struct BuildPlan {
    // The build command, as ccap will run it.
    std::string target;
    std::vector<std::string> args;
    std::vector<std::string> commandLine;

    // Is this an MSBuild build?
    //
    // The decision is on the **command**, not on the file extensions:
    // a .csproj can be built by "dotnet build", by "msbuild" and by
    // "devenv", and a C++ project can be built by "msbuild" too.
    bool isMsBuild = false;

    // The diagnostic log to read.
    std::string msbuildLog;

    // Does the build have to produce the log, or is one already there?
    bool writesMsbuildLog = false;

    // Did the build write the log through the MSBuild file logger, or
    // does the log have to be collected from the build's stdout?
    //
    // The file logger is the better mechanism: it writes the log
    // directly, in UTF-8, and leaves the build's own output alone.
    // It is only available when the command is MSBuild-compatible --
    // it is a command line switch.
    //
    // For a wrapper build, the switch cannot be handed to the command,
    // so the log is collected from the stdout instead. That costs the
    // user their build output, which is why it is the fallback.
    bool logWrittenByLogger = false;

    // Did the user supply the MSBuild switches for a wrapper build?
    //
    // When the build command is a script, ccap cannot add the MSBuild
    // switches by itself -- the script may reject them ("cmake --build
    // -v:diag" fails). So the user says what the script accepts, and
    // this flag records that they did.
    //
    // When it is false and the build turns out to have a .NET part,
    // the .NET half is missing -- and the run says so.
    bool wrapperNeedsMsbuildArgs = false;

    // Is the build command a script rather than a build tool that
    // ccap can hand the MSBuild switches to?
    //
    // The run uses this to tell the two "no .NET compile" cases
    // apart: a build that simply has no .NET part, and a script build
    // whose inner MSBuild was not verbose enough. The second one is
    // actionable, so it gets an actionable message.
    bool isWrapper = false;
};

// Work out what the unified entry has to do.
BuildPlan planBuild(const std::string& target,
                     const std::vector<std::string>& args,
                     const BuildOptions& opts);

}  // namespace compdb
}  // namespace ccap

#endif  // CCAP_COMPDB_BUILD_H
