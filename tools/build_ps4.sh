#!/usr/bin/env bash
# Linux/macOS build entry point. Windows users run tools/build_ps4.ps1.
set -euo pipefail
exec "$(cd "$(dirname "$0")" && pwd)/build_ci.sh" "$@"
