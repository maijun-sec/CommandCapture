// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// CommandCapture - script static analysis
//
// Why this layer is needed: the builtin commands of cmd.exe / sh
// (echo, cd, set, for, if...) never create a process. No process-level
// mechanism (debugger / LD_PRELOAD / ETW) can see them. The only option is
// to statically scan the script content.
//
// Supported:
//   .bat/.cmd  cmd.exe syntax
//   .sh        POSIX shell syntax
//   .ps1       PowerShell syntax (limited support)
#pragma once

#include <string>
#include <vector>

#include "core/event.h"

namespace ccap {

// Scan a script file, writing the recognizable commands into collector.
// Supports recursively scanning the other scripts it invokes (same
// directory / PATH lookup).
struct ScanOptions {
    bool recursive = true;        // recursively scan the invoked scripts
    int maxDepth = 4;            // maximum recursion depth
    bool resolvePaths = true;    // try to resolve the absolute path of the
                                 // executable
    int maxFileSize = 2 * 1024 * 1024;  // maximum size of a single file
};

void scanScriptFile(const std::string& path,
                      const ScanOptions& opts,
                      EventCollector* collector,
                      int depth = 0);

// Dispatch based on the extension
void scanScript(const std::string& path,
                const ScanOptions& opts,
                EventCollector* collector,
                int depth = 0);

}  // namespace ccap
