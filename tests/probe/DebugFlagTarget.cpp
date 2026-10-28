// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// Probe v9: verify that the target's PEB.BeingDebugged can be cleared
// The criterion: after clearing it, the target reading PEB.BeingDebugged
// itself should get 0.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string>

// The target program: reads its own PEB.BeingDebugged and prints it
static int g_want_pid = 0;

int main(int argc, char** argv) {
    // Read the BeingDebugged of our own PEB directly
    // x64 PEB: BeingDebugged is at offset 0x02
    unsigned char* peb = (unsigned char*)__readgsdword(0x60);
    printf("target: BeingDebugged = %d\n", peb[2]);
    return 0;
}
