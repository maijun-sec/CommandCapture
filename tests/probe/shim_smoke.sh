#!/bin/bash
# CommandCapture - capture every command a script or executable spawns.
#
# Copyright (c) 2026 maijun
#
# SPDX-License-Identifier: MIT
#
# Released under the MIT License. See the LICENSE file at the repository
# root for the full text.

# Linux shim smoke test
set -u
ROOT=/mnt/d/work/workspace/visualstudio/CommandCapture
SHIM="$ROOT/build-linux/ccap_shim.so"

cd "$ROOT" || exit 1
gcc tests/probe/shim_smoke.c -o /tmp/shim_smoke || exit 1

echo "=== 1. no preload ==="
/tmp/shim_smoke
echo "exit=$?"

echo ""
echo "=== 2. preload + execve ==="
LD_PRELOAD="$SHIM" /tmp/shim_smoke e
echo "exit=$?"

echo ""
echo "=== 3. preload + execv ==="
LD_PRELOAD="$SHIM" /tmp/shim_smoke p
echo "exit=$?"

echo ""
echo "=== 4. preload a real program (ls) ==="
LD_PRELOAD="$SHIM" /bin/ls /dev/null
echo "exit=$?"

echo ""
echo "=== 5. preload bash ==="
LD_PRELOAD="$SHIM" /bin/bash -c 'echo bash-ran; /bin/echo inner-ran'
echo "exit=$?"
