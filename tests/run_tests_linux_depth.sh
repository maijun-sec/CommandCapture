#!/bin/bash
# CommandCapture - capture every command a script or executable spawns.
#
# Copyright (c) 2026 maijun
#
# SPDX-License-Identifier: MIT
#
# Released under the MIT License. See the LICENSE file at the repository
# root for the full text.

# Linux depth and diagnostics regression tests
#
# Covers:
#   1. Depth computation (exec process replacement vs real parent/child nesting)
#   2. The root event (the first exec happens before LD_PRELOAD takes effect)
#   3. Diagnostics for a statically linked intermediate
#   4. The capture completeness conclusion
set -u
ROOT=/mnt/d/work/workspace/visualstudio/CommandCapture
BUILD="$ROOT/build-linux"
CCAP="$BUILD/ccap"

cd "$BUILD" || exit 1

PASS=0
FAIL=0

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

# Get the depth of an executable from the ndjson
depth_of() {
    local file="$1"; local exe="$2"
    grep -F "\"executable\":\"$exe\"" "$file" | head -1 \
        | sed -n 's/.*"depth":\([0-9]*\).*/\1/p'
}

echo "=== 1. Depth: real parent/child nesting ==="
cat > /tmp/ccap_depth.sh <<'EOF'
#!/bin/bash
sh -c '/bin/echo inner'
EOF
chmod +x /tmp/ccap_depth.sh

"$CCAP" run /tmp/ccap_depth.sh --format ndjson --no-scan --out /tmp/ccap_depth.ndjson >/dev/null 2>&1

d_bash=$(depth_of /tmp/ccap_depth.ndjson "bash")
d_sh=$(depth_of /tmp/ccap_depth.ndjson "sh")
d_echo=$(depth_of /tmp/ccap_depth.ndjson "echo")
echo "  bash=$d_bash sh=$d_sh echo=$d_echo"
check "the root process has depth=0" "$([ "$d_bash" = "0" ] && echo 1 || echo 0)" "actual=$d_bash"
check "the child process has depth=1" "$([ "$d_sh" = "1" ] && echo 1 || echo 0)" "actual=$d_sh"
check "the grandchild process has depth=2" "$([ "$d_echo" = "2" ] && echo 1 || echo 0)" "actual=$d_echo"

echo ""
echo "=== 1b. The shim exports the correct symbols (ABI names must not be renamed) ==="
# The shim's job is to **intercept** glibc's exec/spawn symbols —— so the
# names it exports must be **exactly** the same as glibc's.
#
# This went wrong once: a bulk rename (snake_case -> camelBack) turned
# `posix_spawn` into `posixSpawn`. The shim then exported `posixSpawn`, so
# glibc's `posix_spawn` was never intercepted —— and GNU make prefers
# posix_spawn, so **not a single** child process exec'ed by make was caught.
#
# Symptom: make only reports the root event, no child events at all.
# Root cause: the function name *is* the ABI, renaming it = failing silently.
#
# So the symbol table is checked directly here —— that does not depend on
# runtime behaviour, runs fast and pinpoints the problem.
SHIM="$ROOT/build-linux/ccap_shim.so"
if [ -f "$SHIM" ]; then
    syms=$(nm -D --defined-only "$SHIM" 2>/dev/null)
    for s in execve execv execvp execvpe execle fexecve execveat posix_spawn posix_spawnp; do
        check "the shim exports $s" \
            "$(echo "$syms" | grep -q " T $s\$" && echo 1 || echo 0)" \
            "$s is not in the symbol table"
    done
    # Reverse check: the camelBack spelling of posix_spawn must not appear
    check "the shim does not wrongly export posixSpawn" \
        "$(echo "$syms" | grep -q " T posixSpawn\$" && echo 0 || echo 1)"
else
    check "the shim exists" 0 "$SHIM not found"
fi

echo ""
echo "=== 2. Depth: an exec process replacement does not change the level ==="
# make execs itself into echo —— the pid stays the same, so does the level
mkdir -p /tmp/ccap_mk2
cat > /tmp/ccap_mk2/Makefile <<'EOF'
all:
	@/bin/echo from-make
EOF
"$CCAP" run make -C /tmp/ccap_mk2 --format ndjson --no-scan --out /tmp/ccap_mk2.ndjson >/dev/null 2>&1

mk_depth=$(depth_of /tmp/ccap_mk2.ndjson "make")
echo_depth=$(depth_of /tmp/ccap_mk2.ndjson "echo")
echo "  make=$mk_depth echo=$echo_depth"
check "make has depth=0" "$([ "$mk_depth" = "0" ] && echo 1 || echo 0)" "actual=$mk_depth"
# Key regression: after the process replacement the level must still be 0,
# it must not become 1
check "echo still has depth=0 after the exec" "$([ "$echo_depth" = "0" ] && echo 1 || echo 0)" "actual=$echo_depth"

echo ""
echo "=== 3. The root event exists (the first exec happens before LD_PRELOAD) ==="
"$CCAP" run /bin/true --format ndjson --no-scan --out /tmp/ccap_root.ndjson >/dev/null 2>&1
root_count=$(grep -c '"source":"runtime"' /tmp/ccap_root.ndjson)
check "the root event exists" "$([ "$root_count" -ge 1 ] && echo 1 || echo 0)" "actual=$root_count"
check "the root event has depth=0" \
    "$(grep -q '"depth":0' /tmp/ccap_root.ndjson && echo 1 || echo 0)"
check "the root event carries the source explanation" \
    "$(grep -q 'reported by ccap' /tmp/ccap_root.ndjson && echo 1 || echo 0)"

echo ""
echo "=== 4. The kind / resolved fields ==="
check "the kind field is populated" \
    "$(grep -q '"kind":"process"' /tmp/ccap_root.ndjson && echo 1 || echo 0)"
check "the resolved field is populated" \
    "$(grep -q '"resolved":true' /tmp/ccap_root.ndjson && echo 1 || echo 0)"

echo ""
echo "=== 5. Diagnostics for a statically linked intermediate ==="
# Build a dynamically linked program that execs a statically linked one
cat > /tmp/ccap_mid.c <<'EOF'
#include <unistd.h>
int main(void) {
    char *argv[] = {"/tmp/ccap_static_mid", NULL};
    execv("/tmp/ccap_static_mid", argv);
    return 1;
}
EOF
if [ ! -x /tmp/ccap_static_mid ]; then
    echo 'int main(void){return 0;}' > /tmp/ccap_static_mid.c
    gcc -static -o /tmp/ccap_static_mid /tmp/ccap_static_mid.c 2>/dev/null || \
        echo 'int main(void){return 0;}' > /tmp/ccap_static_mid.c
fi
gcc -o /tmp/ccap_mid /tmp/ccap_mid.c 2>/dev/null
if [ -x /tmp/ccap_static_mid ] && [ -x /tmp/ccap_mid ]; then
    "$CCAP" run /tmp/ccap_mid --format ndjson --no-scan --out /tmp/ccap_mid.ndjson >/dev/null 2>&1
    echo "  events:"
    grep -o '"executable":"[^"]*"' /tmp/ccap_mid.ndjson | sed 's/^/    /'
    check "the statically linked intermediate is flagged" \
        "$(grep -q 'statically linked' /tmp/ccap_mid.ndjson && echo 1 || echo 0)"
else
    echo "  skip (cannot build the statically linked probe)"
fi

echo ""
echo "=== 6. The capture completeness conclusion ==="
"$CCAP" run /bin/true --format json --no-scan --out /tmp/ccap_comp.json >/dev/null 2>&1
check "the notes contain the completeness conclusion" \
    "$(grep -q 'capture completeness' /tmp/ccap_comp.json && echo 1 || echo 0)"

echo ""
echo "=== 7. Timeout ==="
start=$(date +%s%N)
"$CCAP" run /bin/sleep 10 --format json --timeout 1000 --no-scan --out /tmp/ccap_to.json >/dev/null 2>&1
end=$(date +%s%N)
elapsed_ms=$(( (end - start) / 1000000 ))
echo "  elapsed=${elapsed_ms}ms"
check "the timeout argument is respected (<4000ms)" \
    "$([ "$elapsed_ms" -lt 4000 ] && echo 1 || echo 0)" "actual=${elapsed_ms}ms"
check "the timeout is flagged" \
    "$(grep -q '"timed_out": *true' /tmp/ccap_to.json && echo 1 || echo 0)"
check "the notes say PARTIAL after the timeout" \
    "$(grep -q 'PARTIAL' /tmp/ccap_to.json && echo 1 || echo 0)"

echo ""
echo "=== 8. --tree / --exclude also work on Linux ==="
# Build the fixture here —— it does not depend on files left behind by
# other test scripts
cat > /tmp/ccap_lt2.sh <<'EOF'
#!/bin/bash
echo builtin-echo
whoami
gcc --version
EOF
chmod +x /tmp/ccap_lt2.sh

"$CCAP" run /tmp/ccap_depth.sh --tree --no-scan --out /tmp/ccap_tree.txt >/dev/null 2>&1
check "the tree has a root node" "$(grep -q '^+- ' /tmp/ccap_tree.txt && echo 1 || echo 0)"
check "the tree has indented child nodes" "$(grep -qE '^  \+- ' /tmp/ccap_tree.txt && echo 1 || echo 0)"

"$CCAP" scan /tmp/ccap_lt2.sh --format ndjson --out /tmp/ccap_f.ndjson >/dev/null 2>&1
"$CCAP" scan /tmp/ccap_lt2.sh --format ndjson --exclude gcc --out /tmp/ccap_f2.ndjson >/dev/null 2>&1
n1=$(grep -c '"source"' /tmp/ccap_f.ndjson 2>/dev/null || echo 0)
n2=$(grep -c '"source"' /tmp/ccap_f2.ndjson 2>/dev/null || echo 0)
check "--exclude reduced the event count" "$([ "$n2" -lt "$n1" ] && echo 1 || echo 0)" "before=$n1 after=$n2"
check "the target is gone after --exclude" \
    "$(grep -q '"executable":"gcc"' /tmp/ccap_f2.ndjson && echo 0 || echo 1)"

echo ""
echo "=== 9. The tree renderer must not lose events ==="
#
# Regression: the tree renderer used to move each child node out of the
# node pool into its parent's children vector. The pid -> node index still
# pointed at the moved-from node, so a later event whose parent was that
# pid was attached to a dead node and **silently vanished from the tree**.
#
# The shape that triggers it:
#   parent (pid A)
#     child (pid B)          <- the primary node for pid B, and it IS a
#                                child, so it gets moved
#       grandchild (pid C)   <- resolves its parent through pid B's index
#     child2 (pid B)         <- the same pid B again (exec)
#
# Symptom: the raw event list had 4 events, the tree showed only 3.
#
# The check is deliberately about the COUNT: the point is that no event may
# be lost, not that the layout looks a particular way.
cat > /tmp/ccap_tree_bug.c <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "middle") == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            char* a[] = {(char*)"/bin/echo", (char*)"grandchild", 0};
            execv("/bin/echo", a);
            _exit(127);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        char* a[] = {(char*)"/bin/echo", (char*)"middle-execed", 0};
        execv("/bin/echo", a);
        _exit(127);
    }
    pid_t pid = fork();
    if (pid == 0) {
        char* a[] = {(char*)argv[0], (char*)"middle", 0};
        execv(argv[0], a);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return 0;
}
EOF
gcc -o /tmp/ccap_tree_bug /tmp/ccap_tree_bug.c 2>/dev/null

"$CCAP" run /tmp/ccap_tree_bug --format ndjson --no-scan --out /tmp/ccap_tb.ndjson >/dev/null 2>&1
"$CCAP" run /tmp/ccap_tree_bug --format text --tree --no-scan --out /tmp/ccap_tb.txt >/dev/null 2>&1

raw_events=$(grep -c '"source":"runtime"' /tmp/ccap_tb.ndjson 2>/dev/null || echo 0)
# Count every tree node, at any indentation level.
# (grepping '^+- ' would only match the root: the children are indented.)
tree_nodes=$(grep -cE '^\s*\+- ' /tmp/ccap_tb.txt 2>/dev/null || echo 0)
echo "  raw events=$raw_events  tree nodes=$tree_nodes"

check "the tree has as many nodes as there are runtime events" \
    "$([ "$tree_nodes" -eq "$raw_events" ] && echo 1 || echo 0)" \
    "events=$raw_events nodes=$tree_nodes"
check "the grandchild is not lost from the tree" \
    "$(grep -q 'grandchild' /tmp/ccap_tb.txt && echo 1 || echo 0)"
check "the grandchild is nested under its parent" \
    "$(grep -qE '^    \+- echo grandchild' /tmp/ccap_tb.txt && echo 1 || echo 0)"
check "the exec'd sibling is a sibling, not a child" \
    "$(grep -qE '^  \+- echo middle-execed' /tmp/ccap_tb.txt && echo 1 || echo 0)"
echo ""
echo "==================================="
echo "PASS: $PASS   FAIL: $FAIL"
echo "==================================="
[ "$FAIL" -eq 0 ] || exit 1
