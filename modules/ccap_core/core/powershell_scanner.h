// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// CommandCapture - PowerShell script static analysis
//
// Why it needs a separate implementation:
//   PowerShell's syntax differs a lot from cmd / sh:
//     - commands are "Verb-Noun" cmdlets (Get-ChildItem), plus a lot of
//       aliases (ls, dir, cat)
//     - the pipe | connects commands
//     - & is the call operator (& "C:\path\to.exe")
//     - { } is a script block, and it contains commands too
//     - $var is a variable, and paths may be assembled inside variables
//     - # is a comment, <# #> is a block comment
//
// Design trade-offs:
//   We **do not** try to fully parse the PowerShell syntax tree (that would
//   need a real parser). The goal is to list "which external programs /
//   cmdlets will this script execute", so lexical splitting along the
//   "statement -> pipeline segment -> command" hierarchy is enough.
#pragma once

#include <string>
#include <vector>

#include "core/event.h"

namespace ccap {

struct PsScanOptions {
    bool resolvePaths = true;
    // Whether known aliases are expanded to their canonical name
    // (ls -> Get-ChildItem)
    bool expandAliases = true;
};

// PowerShell builtin cmdlet prefix (the verb list)
bool isPowershellBuiltin(const std::string& name);

// Expand a PowerShell alias (ls -> Get-ChildItem). Unknown aliases are
// returned unchanged.
std::string expandPsAlias(const std::string& name);

// Scan a PowerShell script, writing the commands into collector.
void scanPowershell(const std::string& path,
                     const PsScanOptions& opts,
                     EventCollector* collector,
                     int depth = 0);

}  // namespace ccap
