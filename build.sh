#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/esp01-firmware"
pio run "$@"
