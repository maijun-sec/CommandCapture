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
#   So always confirm with GIT_TRACE_CURL=1 and check the "Trying" line
#   actually names the address you pinned. This script does that check
#   for you before it trusts a candidate.
#
#   The script tries each candidate in turn, skipping the ones this git
#   build cannot honour, and falls back to plain git when everything is
#   unreachable.
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
#   CCAP_GIT_TIMEOUT    Seconds allowed for one candidate attempt.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${CCAP_GIT_HOST:-github.com}"
TIMEOUT="${CCAP_GIT_TIMEOUT:-45}"
IPS="${CCAP_GIT_IPS:-140.82.112.3 140.82.113.3 140.82.114.3 140.82.116.3}"

log() { echo "  [git-push] $*" >&2; }

# Does this git build actually honour http.curloptResolve?
#
# We ask for a ref list and look at the trace line that names the
# address git dialled. If it is not the address we pinned, the setting
# is being ignored and pinning would be pointless.
curlopt_resolve_works() {
  local ip="$1" trace
  trace="$(
    GIT_TRACE_CURL=1 \
    timeout "$TIMEOUT" \
    git -C "$ROOT" -c "http.curloptResolve=${HOST}:443:${ip}" \
      ls-remote origin 2>&1 |
      grep -m1 -o "Trying [^:]*:" || true
  )"
  case "$trace" in
    *"Trying ${ip}:"*)
      return 0
      ;;
    *)
      if [ -n "$trace" ]; then
        log "this git build ignores http.curloptResolve (dialled ${trace#Trying }:)"
      fi
      return 1
      ;;
  esac
}

# Run a git subcommand, pinning the hostname to each candidate in turn.
run_git_pinned() {
  local ip rc
  for ip in $IPS; do
    log "trying ${ip}"
    GIT_TERMINAL_PROMPT=0 \
    timeout "$TIMEOUT" \
    git -C "$ROOT" \
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
    GIT_TERMINAL_PROMPT=0 git -C "$ROOT" "$@"
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
    git -C "$ROOT" \
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
  GIT_TERMINAL_PROMPT=0 timeout "$TIMEOUT" git -C "$ROOT" "$@"
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
      git -c "http.curloptResolve=${HOST}:443:${ip}" clone "$url" "$dir"
    else
      git clone "$url" "$dir"
    fi
    ;;
  *)
    echo "usage: $0 {push|fetch|ls-remote|clone <url> <dir>}" >&2
    exit 2
    ;;
esac
