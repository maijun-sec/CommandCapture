#!/usr/bin/env bash
# CommandCapture - capture every command a script or executable spawns.
#
# Copyright (c) 2026 maijun
#
# SPDX-License-Identifier: MIT
#
# Released under the MIT License. See the LICENSE file at the repository
# root for the full text.

# Push helper for machines whose route to github.com is unreliable.
#
# WHY THIS EXISTS
#
#   On some networks the A record for github.com points at a frontend
#   that accepts TCP on 443 and then refuses or resets the HTTP exchange:
#
#     fatal: unable to access 'https://github.com/...':
#       Failed to connect to github.com port 443: Connection refused
#
#   The fix is to pin a different GitHub frontend IP.
#
#   IMPORTANT CAVEAT -- "http.curloptResolve" IS NOT PORTABLE
#
#   git maps that setting onto libcurl's CURLOPT_RESOLVE. Whether it
#   has any effect depends on the git/libcurl build:
#
#     * git 2.40 on Windows honours it. Verified:
#
#         GIT_TRACE_CURL=1 git -c http.curloptResolve=github.com:443:140.82.112.3 ls-remote
#         == Info:   Trying 140.82.112.3:443...
#
#     * git 2.34 on Linux/WSL IGNORES it silently. The same command
#       still prints the DNS address:
#
#         == Info:   Trying 20.205.243.166:443...
#
#       A silently ignored setting is the worst kind: it looks like it
#       is working because the command sometimes succeeds.
#
#   So always confirm the pinned route actually works before trusting a
#   candidate.  This script probes the candidate IP directly and only
#   uses it when the connection succeeds.
#
#   The script tries each candidate in turn, skipping the ones that are
#   not reachable, and falls back to plain git when everything is
#   unreachable.
#
#   WHICH GIT RUNS
#
#   On Windows the "bash" on PATH is usually WSL, and WSL's git is
#   often 2.34 -- which IGNORES http.curloptResolve silently.  The
#   repository is configured for the Windows git (2.40), which honours
#   it.  So this script looks for the Windows git and calls it
#   explicitly, and converts the repository path to a Windows path for
#   it.  Set CCAP_GIT to override.
#
# USAGE
#   bash tools/git-push.sh push [-u] [--force]
#   bash tools/git-push.sh fetch [--prune]
#   bash tools/git-push.sh ls-remote
#   bash tools/git-push.sh clone <url> <dir>
#
# ENVIRONMENT
#   CCAP_GIT_IPS        Space-separated candidate frontend IPs.
#   CCAP_GIT_HOST       Hostname to pin (default: github.com).
#   CCAP_GIT_DIRECT     Set to 1 to bypass pinning and call git directly.
#   CCAP_GIT            The git binary to run (default: the Windows git
#                       when found, else the one on PATH).
#   CCAP_GIT_TIMEOUT    Seconds allowed for one candidate attempt.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${CCAP_GIT_HOST:-github.com}"
TIMEOUT="${CCAP_GIT_TIMEOUT:-45}"
IPS="${CCAP_GIT_IPS:-140.82.112.3 140.82.113.3 140.82.114.3 140.82.116.3}"

# The repository path, in the form the git we run understands.
#
# Under WSL, $ROOT is a Linux path ("/mnt/d/..."), which the Windows git
# cannot chdir into.  So when we run the Windows git we have to hand it
# the Windows path instead.
GIT_ROOT="$ROOT"

# Which git to run.
#
# On Windows the "bash" on PATH is usually WSL, and WSL's git is often
# 2.34 -- which IGNORES http.curloptResolve silently.  The repository
# itself is configured for the Windows git, and that one (2.40) does
# honour the setting.  So when this script runs under WSL it must call
# the Windows git explicitly, otherwise the pinning below silently
# does nothing and the push dies against the bad DNS entry.
#
# CCAP_GIT overrides it entirely.
GIT="${CCAP_GIT:-git}"
if [ "${CCAP_GIT:-}" = "" ]; then
  # Look for a Windows git that honours the setting.  Where it lives
  # differs per machine (C: or D:, "Git" or "Program Files"), so try the
  # usual places instead of hardcoding one.
  for candidate in \
    /mnt/c/development/Git/cmd/git.exe \
    /mnt/d/development/Git/cmd/git.exe \
    /mnt/c/Program\ Files/Git/cmd/git.exe \
    /mnt/d/Program\ Files/Git/cmd/git.exe
  do
    if [ -x "$candidate" ]; then
      GIT="$candidate"
      break
    fi
  done
fi

if [ "$GIT" != "git" ]; then
  # The Windows git wants a Windows path.
  case "$ROOT" in
    /mnt/*)
      drive="$(printf '%s' "$ROOT" | cut -c6 | tr '[:lower:]' '[:upper:]')"
      rest="$(printf '%s' "$ROOT" | cut -c7-)"
      GIT_ROOT="${drive}:${rest}"
      ;;
  esac
fi

log() { echo "  [git-push] $*" >&2; }

log "using git: ${GIT} ($(${GIT} --version 2>/dev/null || echo unknown))"

# Does this git build actually honour http.curloptResolve?
#
# The trace line git prints is:
#
#   == Info:   Trying 140.82.112.3:443...
#
# We do not parse that any more; see the comment on the function below
# for why.
# Does this git build actually honour http.curloptResolve?
#
# WHAT MATTERS IS WHETHER THE PINNED COMMAND WORKS, so that is what we
# test: run the real command with the pin and see whether it succeeds.
#
# The earlier version of this check parsed GIT_TRACE_CURL output and
# looked for the address git dialled.  That is fragile in two ways:
#
#   * The trace line is "Trying 140.82.112.3:443...", so a "[^:]*"
#     pattern stops at the colon inside the IP and captures only
#     "Trying 140.82.112" -- which never compares equal to the pinned
#     IP, so a git that honours the setting was reported as ignoring
#     it.
#
#   * Under WSL the trace does not reach the shell at all: the Windows
#     git writes it where the WSL pipe cannot see it, so the detection
#     had nothing to read and fell back to plain git.
#
# So: ask the operating system instead.  Does a TLS connection to the
# pinned IP succeed?  If it does, the address is reachable and pinning
# it is worth trying; if it does not, skip that candidate.
curlopt_resolve_works() {
  local ip="$1"
  if command -v curl >/dev/null 2>&1; then
    if curl -sS -I --max-time 10 -o /dev/null \
         "https://${ip}" -H "Host: ${HOST}" -k >/dev/null 2>&1; then
      return 0
    fi
    return 1
  fi
  # No curl: fall back to trying the command itself.
  GIT_TERMINAL_PROMPT=0 \
  timeout "$TIMEOUT" \
  "$GIT" -C "$GIT_ROOT" -c "http.curloptResolve=${HOST}:443:${ip}" \
    ls-remote origin >/dev/null 2>&1
}

# Run a git subcommand, pinning the hostname to each candidate in turn.
run_git_pinned() {
  local ip rc
  for ip in $IPS; do
    log "trying ${ip}"
    GIT_TERMINAL_PROMPT=0 \
    timeout "$TIMEOUT" \
    "$GIT" -C "$GIT_ROOT" \
      -c "http.curloptResolve=${HOST}:443:${ip}" \
      -c "http.lowSpeedLimit=1" \
      -c "http.lowSpeedTime=30" \
      "$@"
    rc=$?
    if [ $rc -eq 0 ]; then
      log "succeeded via ${ip}"
      return 0
    fi
    log "candidate ${ip} failed (exit ${rc})"
  done
  return $rc
}

run_git() {
  if [ "${CCAP_GIT_DIRECT:-0}" = "1" ]; then
    GIT_TERMINAL_PROMPT=0 "$GIT" -C "$GIT_ROOT" "$@"
    return $?
  fi

  local ip rc
  for ip in $IPS; do
    if ! curlopt_resolve_works "$ip"; then
      continue
    fi
    log "pinning ${HOST} to ${ip}"
    GIT_TERMINAL_PROMPT=0 \
    timeout "$TIMEOUT" \
    "$GIT" -C "$GIT_ROOT" \
      -c "http.curloptResolve=${HOST}:443:${ip}" \
      -c "http.lowSpeedLimit=1" \
      -c "http.lowSpeedTime=30" \
      "$@"
    rc=$?
    if [ $rc -eq 0 ]; then
      log "succeeded via ${ip}"
      return 0
    fi
    log "candidate ${ip} failed (exit ${rc})"
  done

  log "no pinned candidate worked; falling back to plain git"
  GIT_TERMINAL_PROMPT=0 timeout "$TIMEOUT" "$GIT" -C "$GIT_ROOT" "$@"
}

case "${1:-}" in
  push)
    shift
    run_git push "$@"
    ;;
  fetch)
    shift
    run_git fetch "$@"
    ;;
  ls-remote)
    run_git ls-remote origin
    ;;
  clone)
    shift
    url="${1:?usage: git-push.sh clone <url> <dir>}"
    dir="${2:?usage: git-push.sh clone <url> <dir>}"
    ip=""
    for candidate in $IPS; do
      if curlopt_resolve_works "$candidate"; then
        ip="$candidate"
        break
      fi
    done
    if [ -n "$ip" ]; then
      log "pinning ${HOST} to ${ip}"
      GIT_TERMINAL_PROMPT=0 \
      timeout "$TIMEOUT" \
      "$GIT" -C "http.curloptResolve=${HOST}:443:${ip}" clone "$url" "$dir"
    else
      "$GIT" clone "$url" "$dir"
    fi
    ;;
  *)
    echo "usage: $0 {push|fetch|ls-remote|clone <url> <dir>}" >&2
    exit 2
    ;;
esac
