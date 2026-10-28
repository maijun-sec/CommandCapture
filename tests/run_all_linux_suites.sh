#!/usr/bin/env bash
# CommandCapture - capture every command a script or executable spawns.
#
# Copyright (c) 2026 maijun
#
# SPDX-License-Identifier: MIT
#
# Released under the MIT License. See the LICENSE file at the repository
# root for the full text.

# Run all the Linux suites at once; if any of them fails the whole thing
# fails.
#
# Why this is a separate file:
#   Passing a multi-line script through `wsl -e bash -lc "..."` from
#   PowerShell is easy to get wrong: the nested quotes get eaten. A file is
#   the least effort and the most reproducible.
set -u

cd "$(dirname "$0")/.." || exit 1

# ── Guard: shell scripts must use LF line endings ─────────────────────
#
# A CRLF shell script fails in a way that has nothing to do with the test:
#
#   line 13: set: -: invalid option
#   line 16: $'\r': command not found
#   line 17: cd: $'...CommandCapture\r': No such file or directory
#
# The script never runs a single assertion -- it just dies on line 13.
# And it dies *before* the test output, so it looks like "the suite failed"
# rather than "the file has the wrong line endings".
#
# This bit us once: a Python edit script rewrote a .sh file with the
# platform default line separator, which on Windows is CRLF.
#
# So: check it here, before running anything.
echo "########## pre-flight: line endings ##########"
line_ending_failures=0
for s in tests/*.sh tests/probe/*.sh; do
    [ -f "$s" ] || continue
    if grep -qU $'\r' "$s"; then
        echo "FAIL CRLF line endings in $s"
        line_ending_failures=$((line_ending_failures + 1))
    fi
done
if [ "$line_ending_failures" -ne 0 ]; then
    echo
    echo "$line_ending_failures shell script(s) have CRLF line endings."
    echo "Convert them to LF before running the suites."
    exit 1
fi
echo "OK: all shell scripts use LF line endings"
echo

SUITES=(
  run_tests_linux.sh
  run_tests_linux_extra.sh
  run_tests_linux_dedup.sh
  run_tests_linux_stdout.sh
  run_tests_linux_depth.sh
  run_tests_compdb.sh
)

overall=0
declare -a results

for s in "${SUITES[@]}"; do
  echo "########## $s ##########"
  log="/tmp/ccap_${s%.sh}.log"
  bash "tests/$s" > "$log" 2>&1
  rc=$?
  if [ $rc -eq 0 ]; then
    echo "EXIT=0"
    tail -6 "$log"
  else
    echo "EXIT=$rc  <<<< FAILED"
    overall=1
    # On failure dump the whole log, otherwise only the tail is visible and
    # the problem cannot be located
    cat "$log"
  fi
  results+=("$s exit=$rc")
  echo
done

echo "########## Summary ##########"
for r in "${results[@]}"; do
  echo "  $r"
done

exit $overall
