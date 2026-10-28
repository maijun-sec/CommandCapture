// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// Reproduce the case where one pid produces several runtime events with
// different programs: the process execs itself into a different program
// twice, so the pid stays the same while the executable changes.
//
// This is the shape that breaks the tree renderer: the pid->node index
// points at the FIRST node of that pid, and that node gets moved into its
// parent's children vector. If the same pid has several nodes, the
// surviving index entry can end up pointing at a moved-from node.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "child") == 0) {
        // This process will be exec'd into a different program.
        // execv replaces the image but keeps the pid.
        char* a[] = {(char*)"/bin/echo", (char*)"after-exec", 0};
        execv("/bin/echo", a);
        _exit(127);
    }

    // Parent: fork a child that execs into a different program.
    pid_t pid = fork();
    if (pid == 0) {
        char* a[] = {(char*)argv[0], (char*)"child", 0};
        execv(argv[0], a);
        _exit(127);
    }

    int st = 0;
    waitpid(pid, &st, 0);
    printf("multi-exec-test\n");
    return 0;
}
