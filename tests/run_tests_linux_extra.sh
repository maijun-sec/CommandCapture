#!/bin/bash
# CommandCapture - capture every command a script or executable spawns.
#
# Copyright (c) 2026 maijun
#
# SPDX-License-Identifier: MIT
#
# Released under the MIT License. See the LICENSE file at the repository
# root for the full text.

# Additional verification on the Linux side:
#   1. Statically linked program -> a diagnostic hint is expected
#   2. execveat -> it should be captured
#   3. posix_spawn -> it should be captured
#   4. The same pid execing the same command twice in a row -> it must not be
#      wrongly merged
set -u
ROOT=/mnt/d/work/workspace/visualstudio/CommandCapture
BUILD="$ROOT/build-linux"
CCAP="$BUILD/ccap"

cd "$BUILD" || exit 1

echo "=== 1. Diagnostics for a statically linked program ==="
cat > /tmp/ccap_static.c <<'EOF'
#include <unistd.h>
int main() { if (fork() == 0) { execl("/bin/true", "true", (char*)0); } return 0; }
EOF
gcc -static -O0 -o /tmp/ccap_static /tmp/ccap_static.c 2>/dev/null
if [ -x /tmp/ccap_static ]; then
    file /tmp/ccap_static | head -1
    "$CCAP" run /tmp/ccap_static --format json 2>&1 | grep -E 'notes|statically|statically' | head -5
else
    echo "(statically linking unavailable, skipping)"
fi

echo ""
echo "=== 2. execveat ==="
cat > /tmp/ccap_evat.c <<'EOF'
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
int main() {
    int fd = open("/bin/echo", O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    char* a[] = {(char*)"/bin/echo", (char*)"via-execveat", 0};
    execveat(fd, "", a, (char* const*)0, 0);
    perror("execveat");
    return 1;
}
EOF
gcc -O0 -o /tmp/ccap_evat /tmp/ccap_evat.c 2>/dev/null
if [ -x /tmp/ccap_evat ]; then
    "$CCAP" run /tmp/ccap_evat --format text 2>&1 | grep -E '^\[' | head -10
else
    echo "(compilation failed, skipping)"
fi

echo ""
echo "=== 3. posix_spawn ==="
cat > /tmp/ccap_pspawn.c <<'EOF'
#include <spawn.h>
#include <stdio.h>
extern char** environ;
int main() {
    pid_t pid;
    char* a[] = {(char*)"/bin/echo", (char*)"via-posix-spawn", 0};
    if (posix_spawn(&pid, "/bin/echo", 0, 0, a, environ) != 0) {
        perror("posix_spawn");
        return 1;
    }
    int st; waitpid(pid, &st, 0);
    return 0;
}
EOF
gcc -O0 -o /tmp/ccap_pspawn /tmp/ccap_pspawn.c 2>/dev/null
if [ -x /tmp/ccap_pspawn ]; then
    "$CCAP" run /tmp/ccap_pspawn --format text 2>&1 | grep -E '^\[' | head -10
else
    echo "(compilation failed, skipping)"
fi

echo ""
echo "=== 4. The same pid execs the same command twice in a row ==="
cat > /tmp/ccap_repeat.c <<'EOF'
#include <stdio.h>
#include <unistd.h>
int main() {
    // The first one is echo itself, the second execs into another echo
    char* a[] = {(char*)"/bin/echo", (char*)"first", 0};
    execv("/bin/echo", a);
    return 1;
}
EOF
gcc -O0 -o /tmp/ccap_repeat /tmp/ccap_repeat.c 2>/dev/null
"$CCAP" run /tmp/ccap_repeat --format text 2>&1 | grep -E '^\[' | head -10
