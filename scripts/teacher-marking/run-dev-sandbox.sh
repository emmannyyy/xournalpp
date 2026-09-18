#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNTIME_ROOT="${REPO_ROOT}/.marking-local/runtime"
EXECUTABLE="${REPO_ROOT}/.install/bin/studyszn-marker"

if [[ ! -x "${EXECUTABLE}" ]]; then
    echo "StudySzn Marker is not installed at ${EXECUTABLE}." >&2
    echo "Build and install the fork before launching it." >&2
    exit 1
fi

mkdir -p \
    "${RUNTIME_ROOT}/config" \
    "${RUNTIME_ROOT}/state" \
    "${RUNTIME_ROOT}/cache" \
    "${RUNTIME_ROOT}/data" \
    "${RUNTIME_ROOT}/tmp"

export XDG_CONFIG_HOME="${RUNTIME_ROOT}/config"
export XDG_STATE_HOME="${RUNTIME_ROOT}/state"
export XDG_CACHE_HOME="${RUNTIME_ROOT}/cache"
export XDG_DATA_HOME="${RUNTIME_ROOT}/data"
export TMPDIR="${RUNTIME_ROOT}/tmp"
export GSETTINGS_BACKEND="memory"
export STUDYSZN_MARKER_DEV_SANDBOX="1"

exec "${EXECUTABLE}" --disable-audio "$@"
