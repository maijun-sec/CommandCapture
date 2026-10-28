// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// Reproduce the tree-renderer bug where one pid produces several runtime
// events AND the first of them is itself a child.
//
// The renderer indexes pid -> the first node seen for that pid, then moves
// each child node into its parent's children vector. The moved-from node
// stays in the pool but is left in a moved-from state, so the pid index
// still points at a dead node.
//
// The shape that breaks it:
//   parent (pid A)
//     child (pid B)          <- primary node for pid B, and it IS a child
//       grandchild (pid C)   <- looks up pid B in the index
//   child2 (pid B)           <- the SAME pid B again (exec), also a child
//
// The grandchild resolves its parent through pid B's index entry -- but by
// then that entry may point at a moved-from node, so the grandchild is
// attached to a node that is never rendered, and disappears from the tree.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "middle") == 0) {
        // This process has the same pid as its parent's "middle" stage.
        // It spawns a grandchild, then execs into something else.
        pid_t pid = fork();
        if (pid == 0) {
            char* a[] = {(char*)"/bin/echo", (char*)"grandchild", 0};
            execv("/bin/echo", a);
            _exit(127);
        }
        int st = 0;
        waitpid(pid, &st, 0);

        // Now exec into a different program. The pid does not change.
        char* a[] = {(char*)"/bin/echo", (char*)"middle-execed", 0};
        execv("/bin/echo", a);
        _exit(127);
    }

    // Parent: a child that will become "middle" and then exec.
    pid_t pid = fork();
    if (pid == 0) {
        char* a[] = {(char*)argv[0], (char*)"middle", 0};
        execv(argv[0], a);
        _exit(127);
    }

    int st = 0;
    waitpid(pid, &st, 0);
    printf("tree-bug-test\n");
    return 0;
}
