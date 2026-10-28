// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// CommandCapture - Windows debugger based capture
//
// Mechanism: the target is started with CreateProcessW(..., DEBUG_PROCESS),
// and Windows automatically notifies us about every child process as a
// debugged process (CREATE_PROCESS_DEBUG_EVENT), so the complete process tree
// can be captured.
//
// Key points (all of them summarized from pitfalls that were hit):
//   1. STARTUPINFO must be passed as a valid pointer with cb filled in,
//      otherwise CreateProcessW fails silently.
//   2. ContinueDebugEvent must use DBG_CONTINUE (0x00010002); using 0 gets
//      stuck on the first exception.
//   3. Reading remote memory must use the debuggee's hProcess.
//   4. The event loop must not stop at the first EXIT; it has to wind down
//      with a quiet-tick.
//   5. hFile in CREATE_PROCESS_DEBUG_EVENT is a handle and must be closed with
//      CloseHandle.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "core/event.h"
#include "capture/platform.h"

namespace ccap {

struct CaptureOptions {
    // target script / executable file
    std::string target;
    // arguments passed to the target
    std::vector<std::string> args;
    // working directory (empty = inherit the current one)
    std::string cwd;
    // quiet period: how long to wait after all processes have exited before
    // deciding the run is over (milliseconds)
    int quietMs = 300;
    // per-run timeout (milliseconds), 0 = no limit
    int timeoutMs = 0;
    // Whether to zero the BeingDebugged flag in the target's PEB.
    // DEBUG_PROCESS always makes the target see BeingDebugged == 1,
    // and anti-debugging logic changes its behaviour based on that. When
    // enabled, IsDebuggerPresent() returns 0.
    // Note: CheckRemoteDebuggerPresent() queries the kernel, it cannot be
    // patched.
    bool clearBeingDebugged = false;

    // How the target's standard output is handled:
    //   true  -> forward it to our stderr (stdout is left for the capture data,
    //   so the JSON stays parseable)
    //   false -> inherit our stdout (the default, same as a normal run)
    //
    // The reason for defaulting to false: the tool should behave like
    // "running it directly",
    // so the user's build script output shows up in the terminal.
    // But it should be turned on when --format json is used and the output is
    // consumed from a pipe.
    bool redirectOutputToStderr = false;
};

// Run the target on Windows as a debugger and collect the command creation
// events of the whole process tree.
// The events are written to collector.
CaptureResult runWindowsDebugger(const CaptureOptions& opts,
                                    EventCollector* collector);

}  // namespace ccap
