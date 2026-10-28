#!/bin/bash
# CommandCapture - capture every command a script or executable spawns.
#
# Copyright (c) 2026 maijun
#
# SPDX-License-Identifier: MIT
#
# Released under the MIT License. See the LICENSE file at the repository
# root for the full text.

# Verify that --stdout-to-stderr lets the JSON be piped and consumed directly
set -u
BUILD=/mnt/d/work/workspace/visualstudio/CommandCapture/build-linux
CCAP="$BUILD/ccap"

cd "$BUILD" || exit 1

echo "=== stdout contains only the JSON ==="
"$CCAP" run /bin/sh -c 'echo TARGET_OUTPUT' --format json --stdout-to-stderr 2>/dev/null \
    > /tmp/ccap_linux.json

head -3 /tmp/ccap_linux.json
echo "..."

python3 - <<'PY'
import json
d = json.load(open("/tmp/ccap_linux.json"))
print("JSON parsed successfully, event_count =", d["event_count"])
PY

echo ""
echo "=== stderr received the target output ==="
"$CCAP" run /bin/sh -c 'echo TARGET_OUTPUT' --format json --stdout-to-stderr 2>&1 >/dev/null | head -3
