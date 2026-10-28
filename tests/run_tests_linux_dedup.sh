#!/bin/bash
# CommandCapture - capture every command a script or executable spawns.
#
# Copyright (c) 2026 maijun
#
# SPDX-License-Identifier: MIT
#
# Released under the MIT License. See the LICENSE file at the repository
# root for the full text.

# Verify the deduplication: the same pid execing the same command twice in a
# row must not be merged into one
set -u
ROOT=/mnt/d/work/workspace/visualstudio/CommandCapture
BUILD="$ROOT/build-linux"
CCAP="$BUILD/ccap"

cd "$ROOT" || exit 1
gcc -O0 -o /tmp/ccap_rep tests/probe/repeat_exec.c 2>/dev/null || exit 1

echo "=== A. The same pid execs the exact same command twice in a row ==="
# An external command inside sh -c (not a builtin) gets exec'ed
"$CCAP" run /bin/sh -c '/bin/echo same; /bin/echo same' --format text 2>&1 |
    grep -E '^\[' | head -10
echo "expected: 2 echo events (different pid or different exec_index)"

echo ""
echo "=== B. The same pid execs the same command twice in a row (fork+execv scenario) ==="
"$CCAP" run /tmp/ccap_rep r --format text 2>&1 | grep -E '^\[' | head -10
echo "expected: to see repeat-test and echo"

echo ""
echo "=== C. Static layer: identical commands on several lines of the same script must not be merged ==="
cat > /tmp/ccap_dup.sh <<'EOF'
#!/bin/sh
echo same
echo same
echo same
EOF
"$CCAP" scan /tmp/ccap_dup.sh --format text 2>&1 | grep -E '^\[' | head -10
echo "expected: 3 echo events"
