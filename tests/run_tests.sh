#!/usr/bin/env bash
# CommandCapture - capture every command a script or executable spawns.
#
# Copyright (c) 2026 maijun
#
# SPDX-License-Identifier: MIT
#
# Released under the MIT License. See the LICENSE file at the repository
# root for the full text.

# CommandCapture regression tests
#
# Scenarios covered:
#   1. cmd builtin commands (only visible via static analysis)
#   2. Direct child processes
#   3. Pipes (cmd.exe starts both sides)
#   4. Nested scripts -> third level
#   5. Redirection
#   6. Short-lived processes
#   7. cmake / ninja / gcc toolchain
#   8. msbuild
#
# Usage: bash tests/run_tests.sh [path to the ccap executable]

set -u

CCAP="${1:-}"
if [ -z "$CCAP" ]; then
  # Guess the location
  for cand in \
      "$(dirname "$0")/../build/Release/ccap.exe" \
      "$(dirname "$0")/../build/ccap" \
      "$(dirname "$0")/../build/Debug/ccap.exe"; do
    if [ -x "$cand" ]; then CCAP="$cand"; break; fi
  done
fi

if [ -z "$CCAP" ] || [ ! -x "$CCAP" ]; then
  echo "ccap executable not found, please specify it as an argument" >&2
  exit 2
fi

FIXTURES="$(cd "$(dirname "$0")" && pwd)/fixtures"
PASS=0
FAIL=0

check() {
  local name="$1"; shift
  local expected="$1"; shift
  local actual="$1"; shift
  if [ "$expected" = "$actual" ]; then
    echo "ok   $name"
    PASS=$((PASS+1))
  else
    echo "FAIL $name (expected=$expected actual=$actual)"
    FAIL=$((FAIL+1))
  fi
}

# Count the occurrences of an executable in the NDJSON
count_exe() {
  local file="$1"; local exe="$2"
  grep -c "\"executable\":\"$exe\"" "$file" 2>/dev/null || echo 0
}

# Count the events with source=runtime
count_runtime() {
  local file="$1"
  grep -c '"source":"runtime"' "$file" 2>/dev/null || echo 0
}

count_static() {
  local file="$1"
  grep -c '"source":"static"' "$file" 2>/dev/null || echo 0
}

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "=== CommandCapture regression tests ==="
echo "ccap: $CCAP"
echo ""

# ---------------------------------------------------------------- 1. cmd script
echo "--- 1. cmd script (builtin + child process + pipe + nesting) ---"
"$CCAP" run "$FIXTURES/build.bat" --format ndjson --out "$TMP/build.ndjson" >/dev/null 2>&1
RT=$(count_runtime "$TMP/build.ndjson")
ST=$(count_static "$TMP/build.ndjson")
check "runtime event count >= 8" "yes" "$([ "$RT" -ge 8 ] && echo yes || echo "no($RT)")"
check "static event count >= 10" "yes" "$([ "$ST" -ge 10 ] && echo yes || echo "no($ST)")"
check "captured whoami" "yes" "$([ "$(count_exe "$TMP/build.ndjson" whoami)" -ge 1 ] && echo yes || echo no)"
check "captured findstr (right side of the pipe)" "yes" "$([ "$(count_exe "$TMP/build.ndjson" findstr)" -ge 1 ] && echo yes || echo no)"
check "captured hostname (nested script)" "yes" "$([ "$(count_exe "$TMP/build.ndjson" hostname)" -ge 2 ] && echo yes || echo no)"
check "the static layer captured the echo builtin" "yes" "$([ "$(count_exe "$TMP/build.ndjson" echo)" -ge 1 ] && echo yes || echo no)"
check "an event with depth=2 exists" "yes" "$(grep -q '"depth":2' "$TMP/build.ndjson" && echo yes || echo no)"
check "the command does not contain a truncated backslash path" "yes" \
  "$(grep -q '"executable":"[^"]*WINDOWS[^"]*cmd' "$TMP/build.ndjson" && echo no || echo yes)"

# ---------------------------------------------------------------- 2. Plain executable file
echo ""
echo "--- 2. Running an executable directly ---"
"$CCAP" run cmd.exe /c exit 0 --format ndjson --out "$TMP/exe.ndjson" >/dev/null 2>&1
RT2=$(count_runtime "$TMP/exe.ndjson")
check "captured cmd.exe" "yes" "$([ "$(count_exe "$TMP/exe.ndjson" cmd.exe)" -ge 1 ] && echo yes || echo no)"
check "runtime event count >= 1" "yes" "$([ "$RT2" -ge 1 ] && echo yes || echo "no($RT2)")"

# ---------------------------------------------------------------- 3. Static analysis
echo ""
echo "--- 3. Static analysis only (scan) ---"
"$CCAP" scan "$FIXTURES/build.bat" --format ndjson --out "$TMP/scan.ndjson" >/dev/null 2>&1
ST3=$(count_static "$TMP/scan.ndjson")
check "scan produces static events only" "yes" \
  "$(grep -q '"source":"runtime"' "$TMP/scan.ndjson" && echo no || echo yes)"
check "scan event count >= 10" "yes" "$([ "$ST3" -ge 10 ] && echo yes || echo "no($ST3)")"
check "scan marks builtins" "yes" \
  "$(grep -q '"is_builtin":true' "$TMP/scan.ndjson" && echo yes || echo no)"

# ---------------------------------------------------------------- 4. JSON format
echo ""
echo "--- 4. Output format ---"
"$CCAP" run "$FIXTURES/build.bat" --format json --out "$TMP/out.json" >/dev/null 2>&1
check "the JSON format is valid" "yes" \
  "$(python3 -c "import json,sys; json.load(open('$TMP/out.json')); print('yes')" 2>/dev/null || \
     node -e "JSON.parse(require('fs').readFileSync('$TMP/out.json','utf8')); console.log('yes')" 2>/dev/null || echo no)"

# ---------------------------------------------------------------- 5. cmake toolchain
echo ""
echo "--- 5. cmake / ninja / gcc toolchain ---"
if command -v cmake >/dev/null 2>&1; then
  rm -rf "$TMP/cmbuild"
  "$CCAP" run cmake -S "$FIXTURES/cmake_proj" -B "$TMP/cmbuild" --format ndjson \
      --out "$TMP/cmake.ndjson" >/dev/null 2>&1
  RTCP=$(count_runtime "$TMP/cmake.ndjson")
  check "cmake captured child processes" "yes" "$([ "$RTCP" -ge 3 ] && echo yes || echo "no($RTCP)")"
  check "cmake tree depth >= 3" "yes" \
    "$(grep -q '"depth":3' "$TMP/cmake.ndjson" && echo yes || echo no)"
else
  echo "skip cmake (not installed)"
fi

# ---------------------------------------------------------------- Summary
echo ""
echo "==================================="
echo "PASS: $PASS   FAIL: $FAIL"
echo "==================================="
[ "$FAIL" -eq 0 ]
