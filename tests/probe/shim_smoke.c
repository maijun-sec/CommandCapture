// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// Linux shim smoke test: verify that the program under test still runs
// normally after the LD_PRELOAD injection
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc > 1 && argv[1][0] == 'e') {
        char* a[] = {(char*)"/bin/echo", (char*)"hello-from-execve", 0};
        char* e[] = {(char*)"PATH=/usr/bin:/bin", 0};
        execve("/bin/echo", a, e);
        perror("execve");
        return 1;
    }
    if (argc > 1 && argv[1][0] == 'p') {
        char* a[] = {(char*)"/usr/bin/env", 0};
        char* e[] = {(char*)"PATH=/usr/bin:/bin", 0};
        execv("/usr/bin/env", a);
        perror("execv");
        return 1;
    }
    printf("shim-smoke: main ran fine\n");
    return 0;
}
