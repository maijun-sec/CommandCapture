// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// 32-bit target program: used to cross-verify that the PEB read is
// correct under WOW64
#include <windows.h>
#include <stdio.h>

// The public SDK has no PEB definition, so only the fields we need are
// taken here.
// The PEB layout (the same for x86 and x64):
//   BYTE Reserved1[2];
//   BYTE BeingDebugged;   <- offset 0x02
//   BYTE Reserved2[1];
struct MIN_PEB {
    BYTE Reserved1[2];
    BYTE BeingDebugged;
    BYTE Reserved2[1];
};

int main() {
    // x86: the PEB pointer is at fs:[0x30]
    MIN_PEB* peb = (MIN_PEB*)__readfsdword(0x30);
    printf("target32: PEB ptr = %p\n", (void*)peb);
    printf("target32: BeingDebugged = %d\n", peb->BeingDebugged);
    printf("target32: IsDebuggerPresent = %d\n", IsDebuggerPresent());

    // Then print our own command line to verify that CommandLine in
    // the PEB is readable
    wchar_t* cmdline = GetCommandLineW();
    printf("target32: cmdline = %ls\n", cmdline ? cmdline : L"(null)");
    fflush(stdout);
    return 0;
}
