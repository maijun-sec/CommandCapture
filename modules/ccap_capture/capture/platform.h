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

// The current platform
Platform currentPlatform();

// Platform name (used for output)
const char* currentPlatformName();

}  // namespace ccap
