#!/bin/bash
# CommandCapture - capture every command a script or executable spawns.
#
# Copyright (c) 2026 maijun
#
# SPDX-License-Identifier: MIT
#
# Released under the MIT License. See the LICENSE file at the repository
# root for the full text.

# ccap compdb end-to-end regression tests
#
# Covers:
#   1. The full compiler list
#   2. Aliases (symlinks recognize each other)
#   3. build log parsing
#   4. Plain text command parsing
#   5. Script filtering
#   6. Header file / macro extraction
#   7. clang compilation database export
#   8. analyzer style translation
set -u
ROOT=/mnt/d/work/workspace/visualstudio/CommandCapture
CCAP="$ROOT/build-linux/ccap"

cd "$ROOT" || exit 1

PASS=0
FAIL=0
TMP=/tmp/ccap_compdb_test

check() {
    local name="$1"; local ok="$2"; local detail="${3:-}"
    if [ "$ok" = "1" ]; then
        echo "ok   $name"
        PASS=$((PASS+1))
    else
        echo "FAIL $name  $detail"
        FAIL=$((FAIL+1))
    fi
}

rm -rf "$TMP"
mkdir -p "$TMP"

# ── 1. Compiler list ─────────────────────────────────────────────────
echo "=== 1. The full compiler list ==="
"$CCAP" compdb list --format text > "$TMP/list.txt" 2>&1
for kind in gcc clang msvc generic; do
    check "the list contains $kind" \
        "$(grep -q "$kind" "$TMP/list.txt" && echo 1 || echo 0)"
done
check "the list mentions the version suffix" \
    "$(grep -q 'gcc-11' "$TMP/list.txt" && echo 1 || echo 0)"

# ── 2. build log ─────────────────────────────────────────
echo ""
echo "=== 2. build log parsing ==="
cat > "$TMP/build-log.txt" <<'EOF'
EXECUTING 'make all '
EXECUTING '/bin/sh -c cd qmake && make '
CWD = /export/home/acc/test-packages/qt-x11-free-3.3.2/qmake
COMPILING '/export/home/acc/prevent/bin/sa-translate g++ -c -o property.o -I. -Igenerators -DQT_NO_TEXTCODEC property.cpp '
EXECUTING '/usr/ccs/bin/as -Qy -s -o property.o /var/tmp/cc2Wo7sG.s '
CWD = /export/home/acc/test-packages/qt-x11-free-3.3.2/qt
COMPILING '/export/home/acc/prevent/bin/sa-translate g++ -c -o main.o -I. -DQT_NO_TEXTCODEC main.cpp '
EOF

"$CCAP" compdb parse "$TMP/build-log.txt" --format text > "$TMP/parse.txt" 2>&1
# The build log holds 5 commands: make / sh / wrapper(g++) / as / wrapper(g++)
check "parsed 5 commands" \
    "$(grep -q 'Parsed 5 command' "$TMP/parse.txt" && echo 1 || echo 0)" \
    "$(head -1 "$TMP/parse.txt")"
check "2 of them are compilation commands" \
    "$(grep -q '2 are compilation command' "$TMP/parse.txt" && echo 1 || echo 0)" \
    "$(cat "$TMP/parse.txt" | head -1)"

"$CCAP" compdb show "$TMP/build-log.txt" > "$TMP/show.txt" 2>&1
check "the wrapper prefix was stripped" \
    "$(grep -q '\[KEEP\] g++ ' "$TMP/show.txt" && echo 1 || echo 0)"
check "make was classified as a script" \
    "$(grep -q 'dropped: script' "$TMP/show.txt" && echo 1 || echo 0)"
check "as was excluded" \
    "$(grep -q 'dropped: excluded (as)' "$TMP/show.txt" && echo 1 || echo 0)"
check "the macros were extracted" \
    "$(grep -q 'QT_NO_TEXTCODEC' "$TMP/show.txt" && echo 1 || echo 0)"
check "the include paths were extracted" \
    "$(grep -q 'includes: . generators' "$TMP/show.txt" && echo 1 || echo 0)"
check "the CWD was associated correctly" \
    "$(grep -q 'qt-x11-free-3.3.2/qmake' "$TMP/show.txt" && echo 1 || echo 0)"

# ── 3. Export the clang compilation database ─────────────────────────────────────
echo ""
echo "=== 3. clang compilation database export ==="
"$CCAP" compdb export "$TMP/build-log.txt" --output "$TMP/compile_commands.json" \
    --format text > /dev/null 2>&1
check "the file was generated" \
    "$([ -f "$TMP/compile_commands.json" ] && echo 1 || echo 0)"
check "it is valid JSON" \
    "$(python3 -c "import json,sys; json.load(open('$TMP/compile_commands.json')); print(1)" 2>/dev/null || echo 0)"
check "it has 2 entries" \
    "$(python3 -c "import json; d=json.load(open('$TMP/compile_commands.json')); print(1 if len(d)==2 else 0)" 2>/dev/null || echo 0)"
check "the entry has arguments" \
    "$(python3 -c "import json; d=json.load(open('$TMP/compile_commands.json')); print(1 if 'arguments' in d[0] else 0)" 2>/dev/null || echo 0)"
check "file is an absolute path" \
    "$(python3 -c "import json; d=json.load(open('$TMP/compile_commands.json')); print(1 if d[0]['file'].startswith('/') else 0)" 2>/dev/null || echo 0)"

# ── 4. analyzer style translation ─────────────────────────────────────────
echo ""
echo "=== 4. analyzer style translation ==="
"$CCAP" compdb translate "$TMP/build-log.txt" > "$TMP/translate.txt" 2>&1
n=$(grep -c 'g++' "$TMP/translate.txt")
check "translated 2 commands" "$([ "$n" -eq 2 ] && echo 1 || echo 0)" "actual=$n"
check "it contains --sys_include style arguments" \
    "$(grep -q -- '-I ' "$TMP/translate.txt" && echo 1 || echo 0)"

# ── 5. Script filtering ───────────────────────────────────────────────────
echo ""
echo "=== 5. Script filtering ==="
cat > "$TMP/cmds.txt" <<'EOF'
g++ -I../include -DFOO=1 -std=c++17 -c src/main.cpp -o build/main.o
gcc -c src/util.c -o build/util.o
sh -c 'g++ -c other.cpp'
make -j8
bash build.sh
cmd.exe /C build.bat
EOF
"$CCAP" compdb parse "$TMP/cmds.txt" --format text > "$TMP/parse2.txt" 2>&1
check "2 of the 6 commands are compilation commands" \
    "$(grep -q '2 are compilation command' "$TMP/parse2.txt" && echo 1 || echo 0)" \
    "$(head -1 "$TMP/parse2.txt")"

# --keep-scripts is supposed to keep the scripts
"$CCAP" compdb parse "$TMP/cmds.txt" --format text --keep-scripts > "$TMP/parse3.txt" 2>&1
kept=$(grep -o '[0-9]* are compilation command' "$TMP/parse3.txt" | head -1 | cut -d' ' -f1)
check "--keep-scripts kept more commands" \
    "$([ "$kept" -gt 2 ] && echo 1 || echo 0)" "kept=$kept"

# ── 6. Reverse parsing of an existing compilation database ─────────────────────────────────────────
echo ""
echo "=== 6. Reverse parsing of an existing compile_commands.json ==="
"$CCAP" compdb parse "$TMP/compile_commands.json" --format text > "$TMP/parse4.txt" 2>&1
check "it can parse back its own output" \
    "$(grep -q '2 are compilation command' "$TMP/parse4.txt" && echo 1 || echo 0)" \
    "$(head -1 "$TMP/parse4.txt")"

# ── 7. Aliases ───────────────────────────────────────────────────────
echo ""
echo "=== 7. Aliases (symlinks recognize each other) ==="
# Create a symlink
mkdir -p "$TMP/bin"
cat > "$TMP/bin/real-gcc" <<'EOF'
#!/bin/sh
EOF
chmod +x "$TMP/bin/real-gcc"
ln -sf "$TMP/bin/real-gcc" "$TMP/bin/alias-cc" 2>/dev/null

"$CCAP" compdb aliases "$TMP/bin/real-gcc" > "$TMP/aliases.txt" 2>&1
check "the aliases command runs" \
    "$(grep -q 'real path' "$TMP/aliases.txt" && echo 1 || echo 0)"
check "the symlink is listed as an alias" \
    "$(grep -q 'alias-cc' "$TMP/aliases.txt" && echo 1 || echo 0)" \
    "$(cat "$TMP/aliases.txt")"

echo ""
echo "==================================="
echo "PASS: $PASS   FAIL: $FAIL"
echo "==================================="
[ "$FAIL" -eq 0 ] || exit 1
