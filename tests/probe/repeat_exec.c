// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// The same pid execing the same command twice in a row —— the
// deduplication must not merge them
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc > 1 && argv[1][0] == 'r') {
        // fork a child process and let it exec the same command twice in
        // a row
        pid_t pid = fork();
        if (pid == 0) {
            char* a[] = {(char*)"/bin/echo", (char*)"same-arg", 0};
            execv("/bin/echo", a);
            _exit(127);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        return 0;
    }
    printf("repeat-test\n");
    return 0;
}
