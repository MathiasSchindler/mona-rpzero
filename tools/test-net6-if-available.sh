#!/usr/bin/env bash
set -euo pipefail

# Wrapper for running the TAP-based IPv6 integration test as part of `make test`.
#
# - Skips cleanly (exit 0) when prerequisites are missing.
# - Avoids interactive sudo prompts by default.
#
# Env vars:
#   SKIP_NET6=1        Always skip.
#   FORCE_NET6=1       Allow interactive sudo (may prompt).
#   TAP_IF=<name>      TAP interface (default mona0).

if [[ "${SKIP_NET6:-0}" == "1" ]]; then
  echo "[test] SKIP: net6 (SKIP_NET6=1)" >&2
  exit 0
fi

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "[test] SKIP: net6 (host OS not Linux)" >&2
  exit 0
fi

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || { echo "[test] SKIP: net6 (missing: $1)" >&2; exit 0; }
}

need_cmd ip
need_cmd dnsmasq
need_cmd sudo

if [[ "${FORCE_NET6:-0}" != "1" ]]; then
  # Non-interactive sudo check.
  if ! sudo -n true >/dev/null 2>&1; then
    echo "[test] SKIP: net6 (sudo needs a password; set FORCE_NET6=1 to run)" >&2
    exit 0
  fi
fi

# Run the actual test.
exec make test-net6 TAP_IF="${TAP_IF:-mona0}"
