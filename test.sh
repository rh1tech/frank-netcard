#!/usr/bin/env bash
# Usage:
#   ./test.sh                                              # basic tests only
#   ./test.sh --wifi SSID,password                         # include WiFi + network tests
#   ./test.sh --wifi SSID,password --full                  # full test (TLS + UDP)
#   ./test.sh --wifi SSID,password --full --tls-host rh1.tech:443  # test against custom TLS host
set -euo pipefail
cd "$(dirname "$0")"

PORT=""
BAUD=115200

# Find serial port
for p in /dev/cu.usbserial*; do
    if [[ -e "$p" ]]; then
        PORT="$p"
        break
    fi
done

PORT="${FLASH_PORT:-$PORT}"
if [[ -z "$PORT" ]]; then
    echo "ERROR: No USB-serial adapter found" >&2
    exit 1
fi

exec python3 "$(dirname "$0")/test_firmware.py" --port "$PORT" --baud "$BAUD" "$@"
