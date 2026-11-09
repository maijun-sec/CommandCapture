// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap - the unified build entry
#pragma once

#include <string>
#include <vector>

#include "capture/platform.h"
#include "ccap_compdb.h"
#include "ccap_compdb_build.h"

namespace ccap {
namespace app {

// The options of the unified build entry.
//
// These are the **run-time** options: how the build is started and where
// its output goes. The decisions about the compilers -- which backends
// will run, and how the build command has to be shaped -- live in
// compdb::BuildOptions and compdb::planBuild, because they are about
// the compilers and not about the process.
struct BuildOptions {
    // The working directory of the build.
    std::string cwd;

    int timeoutMs = 0;
    int quietMs = 300;

    // The JVM capture agent to attach, and the directory its captures
    // go into.
    std::string agentJar;
    std::string agentCaptureDir;

    // The file the build's stdout goes to.
    //
    // It is set by the plan for the .NET backend: the diagnostic log
    // has to reach a file **untouched**, and merging it with stderr
    // corrupts it.
    std::string stdoutFile;
};

// Run the build and collect the compile commands from every backend.
//
// The three backends are:
//
//   native   the process/debugger capture. It sees every process the
//            build spawns, so it covers C, C++, Rust, Go -- anything
//            that runs a compiler as a child process.
//
//   jvm      the in-process agent. The JVM compilers run **inside** the
//            JVM, so a process hook sees only the "java" launch, not
//            the compile.
//
//   dotnet   the MSBuild diagnostic log. The Roslyn compiler is loaded
//            **in-process** by the build, so under the default
//            configuration it never appears as a process at all.
//
// The result is ONE list: the consumer wants "how was every file in
// this repository compiled", not one list per language.
std::vector<compdb::CompileCommand> runBuild(
    const compdb::BuildPlan& plan, const BuildOptions& opts,
    const compdb::BuildOptions& planOpts, std::string* eventsNdjson,
    std::vector<std::string>* notes);

}  // namespace app
}  // namespace ccap
