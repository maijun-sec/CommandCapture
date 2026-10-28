// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// The observed target: reads its own debugger detection result
#include <windows.h>
#include <stdio.h>

// The PEB has no complete definition in the public SDK, so only the
// fields we need are taken here.
// BeingDebugged sits at offset 0x02 in the x64 PEB.
struct MIN_PEB {
    BYTE Reserved1[2];
    BYTE BeingDebugged;
};

int main() {
    // x64: the PEB pointer is at gs:[0x60] (read it as a qword!
    // __readgsdword would crash)
    MIN_PEB* peb = (MIN_PEB*)__readgsqword(0x60);
    printf("target: PEB.BeingDebugged = %d\n", peb->BeingDebugged);
    printf("target: IsDebuggerPresent = %d\n", IsDebuggerPresent());
    DWORD remote = 0;
    BOOL ok = CheckRemoteDebuggerPresent(GetCurrentProcess(), (PBOOL)&remote);
    printf("target: CheckRemoteDebuggerPresent = %d (ok=%d)\n",
           (ok && remote) ? 1 : 0, ok ? 1 : 0);
    fflush(stdout);
    return 0;
}
