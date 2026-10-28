// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// CommandCapture - Read PEB information from the debugged process
//
// Purpose: get the command line the process "actually sees", instead of the
// one we passed to CreateProcessW.
// Because cmd.exe / powershell / node rebuild their own command line,
// only reading PEB->ProcessParameters->CommandLine reflects reality.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace ccap {

struct PebInfo {
    bool ok = false;
    std::wstring commandLine;   // raw command line from the PEB (UNICODE)
    std::wstring imagePath;     // PEB->ImagePathName
    std::wstring currentDir;    // PEB->CurrentDirectory (DosPath)
    std::string  cmdLineUtf8;  // UTF-8 version of the command line
    // Parent process pid (PEB->InheritedFromUniqueProcessId)
    // This is the only reliable source for the real level of the process tree --
    // CREATE_PROCESS_DEBUG_EVENT carries no parent process information.
    uint32_t parentPid = 0;

    // Whether the BeingDebugged flag was successfully cleared
    bool beingDebuggedCleared = false;
};

// Read PEB information from the target process.
// hProcess must be the handle of the target process (the debuggee's handle,
// not ours).
//
// clear_being_debugged: zero the BeingDebugged flag in the target's PEB.
//   Background: DEBUG_PROCESS always makes the target see
//   PEB.BeingDebugged == 1,
//   and anti-debugging logic changes its behaviour based on that.
//   Measured (tests/probe/DbgFlag.cpp):
//     PEB.BeingDebugged        -> can be patched
//     IsDebuggerPresent()      -> reads the same flag, returns 0 after patching
//     CheckRemoteDebuggerPresent() -> queries the kernel's debug object list,
//     **cannot be patched**
//   So this is **best effort**, not complete anti-anti-debugging.
//   Off by default: silently changing the behaviour of the observed program
//   is not good.
PebInfo readPebInfo(HANDLE hProcess, DWORD pid,
                      bool clearBeingDebugged = false);

// Utility: wide characters -> UTF-8
std::string wideToUtf8(const std::wstring& w);

// Utility: UTF-8 -> wide characters
std::wstring utf8ToWide(const std::string& s);

// Utility: split a wide string into arguments using Windows CRT rules
std::vector<std::string> splitWideCommandLine(const std::wstring& w);

}  // namespace ccap
