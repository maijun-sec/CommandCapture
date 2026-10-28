#!/bin/bash
# CommandCapture - capture every command a script or executable spawns.
#
# Copyright (c) 2026 maijun
#
# SPDX-License-Identifier: MIT
#
# Released under the MIT License. See the LICENSE file at the repository
# root for the full text.

# Linux end-to-end verification
set -u
ROOT=/mnt/d/work/workspace/visualstudio/CommandCapture
BUILD="$ROOT/build-linux"
CCAP="$BUILD/ccap"

cd "$BUILD" || exit 1

cat > /tmp/ccap_lt.sh <<'EOF'
#!/bin/bash
echo builtin-echo
cd /tmp
export FOO=bar
whoami > /dev/null
cat /etc/hostname > /dev/null
bash -c 'echo nested-inner'
gcc --version > /dev/null
EOF
chmod +x /tmp/ccap_lt.sh

echo "=== 1. run a shell script ==="
"$CCAP" run /tmp/ccap_lt.sh --format text 2>&1 | grep -E '^\[' | head -30

echo ""
echo "=== 2. scan only ==="
"$CCAP" scan /tmp/ccap_lt.sh --format text 2>&1 | grep -E '^\[' | head -20

echo ""
echo "=== 3. cmake toolchain ==="
rm -rf /tmp/ccap_cmb
mkdir -p /tmp/ccap_cmb/src
cat > /tmp/ccap_cmb/CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.10)
project(Demo CXX)
add_executable(demo src/main.cpp)
EOF
cat > /tmp/ccap_cmb/src/main.cpp <<'EOF'
#include <cstdio>
int main(){ std::printf("hello\n"); return 0; }
EOF
"$CCAP" run cmake -S /tmp/ccap_cmb -B /tmp/ccap_cmb/build --format text 2>&1 | grep -E '^\[' | head -30

echo ""
echo "=== 4. make ==="
cat > /tmp/ccap_cmb/Makefile <<'EOF'
all:
	@echo making
	@/bin/echo from-make
EOF
"$CCAP" run make -C /tmp/ccap_cmb --format text 2>&1 | grep -E '^\[' | head -20
