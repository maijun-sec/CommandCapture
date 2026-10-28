#!/usr/bin/env bash
# Push helper for this machine.
#
# Why it exists:
#   github.com resolves to an IP that this network sometimes resets
#   mid-response. TCP connect succeeds, then the TLS session dies with
#   "Recv failure: Connection was reset" (errno 10054).
#
#   Several other GitHub frontend IPs are reachable, but a plain TCP
#   probe is NOT a reliable test -- a port can accept the connection
#   and then reset the HTTP exchange. So this script validates each
#   candidate by actually running "git ls-remote" against it.
#
# Usage:
#   bash tools/git-push.sh push [-u] [--force]
#   bash tools/git-push.sh fetch
#   bash tools/git-push.sh ls-remote
#   bash tools/git-push.sh clone <url> <dir>

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# GitHub frontend IPs. The one DNS hands out (20.205.243.166) is NOT in
# this list on purpose -- it is the one that gets reset.
IPS=(
  "140.82.112.3"
  "140.82.113.3"
  "140.82.114.3"
  "140.82.116.3"
  "20.205.243.168"
)

HOST="github.com"
PORT="443"

# Pick the first IP that actually answers the git protocol.
# The probe is a real ls-remote, not a TCP connect: a port can accept
# the connection and then reset the exchange.
pick_ip() {
  local ip
  for ip in "${IPS[@]}"; do
    if git -C "$ROOT" -c "http.curloptResolve=${HOST}:${PORT}:${ip}" \
        ls-remote origin >/dev/null 2>&1; then
      echo "$ip"
      return 0
    fi
  done
  echo ""
  return 1
}

run_git() {
  local ip
  ip="$(pick_ip)"
  if [ -n "$ip" ]; then
    echo "  (using ${ip})" >&2
    git -C "$ROOT" -c "http.curloptResolve=${HOST}:${PORT}:${ip}" "$@"
    return $?
  fi
  echo "  (no pinned IP worked; trying unpinned)" >&2
  git -C "$ROOT" "$@"
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
    for ip in "${IPS[@]}"; do
      if git -c "http.curloptResolve=${HOST}:${PORT}:${ip}" clone "$url" "$dir" 2>/dev/null; then
        echo "  (used ${ip})" >&2
        exit 0
      fi
    done
    git clone "$url" "$dir"
    ;;
  *)
    echo "usage: $0 {push|fetch|ls-remote|clone ...}" >&2
    exit 2
    ;;
esac
