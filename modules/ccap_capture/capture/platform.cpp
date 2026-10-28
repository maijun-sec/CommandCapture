// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

#include "capture/platform.h"

#include <algorithm>

#include "core/normalize.h"
#include "core/script_scanner.h"

#if defined(_WIN32)
#include "capture/windows_debugger.h"
#else
#include "capture/linux_preload.h"
#endif

namespace ccap {

namespace {

// Build a "capture completeness" summary sentence and put it into notes.
//
// Why this is needed:
//   The worst failure mode of a capture tool is **silently missing events** --
//   the user sees 3 events and assumes the script only ran 3 commands.
//
//   So every capture run has to answer one question explicitly:
//   "is what you are seeing everything?"
//
//   There are three possible conclusions:
//     complete  -- no known information loss
//     partial   -- there is known loss (with the reason stated)
//     unknown   -- there is a theoretical risk of loss, but it did not trigger
//     this time
void appendCompletenessNote(const std::vector<CommandEvent>& events,
                              RunResult* result) {
    // Count the events that are "known to be uncapturable"
    size_t staticLinked = 0;
    for (const auto& e : events) {
        for (const auto& n : e.notes) {
            if (n.find("statically linked") != std::string::npos) ++staticLinked;
        }
    }

    std::string summary = "capture completeness: ";
    if (result->timedOut) {
        summary +=
            "PARTIAL - the run was killed by --timeout, so the process tree "
            "was intentionally truncated";
        result->notes.push_back(summary);
        return;
    }

    if (staticLinked > 0) {
        summary += "PARTIAL - " + std::to_string(staticLinked) +
                   " event(s) target statically linked binaries; their child "
                   "processes cannot be captured (LD_PRELOAD does not apply)";
        result->notes.push_back(summary);
        return;
    }

#if defined(_WIN32)
    summary +=
        "COMPLETE for process creation (DEBUG_PROCESS sees every descendant); "
        "script builtins are covered by the static analysis layer";
#else
    summary +=
        "COMPLETE for dynamically linked processes (LD_PRELOAD covers every "
        "exec/spawn); script builtins are covered by the static analysis layer";
#endif
    result->notes.push_back(summary);
}

}  // namespace

Platform currentPlatform() {
#if defined(_WIN32)
    return Platform::Windows;
#elif defined(__linux__)
    return Platform::Linux;
#else
    return Platform::Unknown;
#endif
}

const char* currentPlatformName() {
    switch (currentPlatform()) {
        case Platform::Windows: return "windows";
        case Platform::Linux:   return "linux";
        default:                return "unknown";
    }
}

RunResult runCapture(const RunOptions& opts, EventCollector* collector) {
    RunResult result;

    if (opts.target.empty()) {
        result.ok = false;
        result.error = "no target specified";
        return result;
    }

    // 1) Script static analysis (must be done before the run, because the
    // script content may change)
    if (opts.scanScript) {
        std::string ext = extensionOf(opts.target);
        static const char* kScriptExts[] = {".bat", ".cmd", ".ps1", ".sh",
                                            ".bash", ".zsh"};
        bool isScript = false;
        for (const char* e : kScriptExts) {
            if (ext == e) { isScript = true; break; }
        }
        if (isScript) {
            std::string abs = opts.target;
            if (!isAbsolutePath(abs)) {
                std::string cand = normalizePath(currentDir() + "/" + abs);
                if (fileExists(cand)) abs = cand;
            }
            ScanOptions so;
            so.recursive = opts.scriptRecursive;
            so.maxDepth = opts.scriptDepth;
            scanScript(abs, so, collector, 0);
        }
    }

    // 2) Runtime capture
#if defined(_WIN32)
    CaptureOptions co;
    co.target = opts.target;
    co.args = opts.args;
    co.cwd = opts.cwd;
    co.quietMs = opts.quietMs;
    co.timeoutMs = opts.timeoutMs;
    co.clearBeingDebugged = opts.clearBeingDebugged;
    co.redirectOutputToStderr = opts.redirectOutputToStderr;

    CaptureResult cr = runWindowsDebugger(co, collector);
    result.exitCode = cr.exitCode;
    result.timedOut = cr.timedOut;
    result.notes = cr.notes;
    if (!cr.error.empty()) {
        // The target can run but the capture failed; that is not fatal
        result.notes.push_back(cr.error);
        if (cr.exitCode < 0 && cr.rootPid == 0) {
            result.ok = false;
            result.error = cr.error;
        }
    }
#else
    CaptureResult cr = runLinuxPreload(opts, collector);
    result.exitCode = cr.exitCode;
    result.timedOut = cr.timedOut;
    result.notes = cr.notes;
    if (!cr.error.empty()) {
        result.notes.push_back(cr.error);
        if (cr.exitCode < 0) {
            result.ok = false;
            result.error = cr.error;
        }
    }
#endif

    // The completeness conclusion -- placed last, because all events have to
    // be seen before it can be judged
    if (collector) {
        appendCompletenessNote(collector->events(), &result);
    }

    return result;
}

}  // namespace ccap
