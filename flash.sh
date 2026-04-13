#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(dirname "$0")"
FW_DIR="$SCRIPT_DIR/esp01-firmware"
BIN="$FW_DIR/.pio/build/esp01/firmware.bin"
BAUD="${FLASH_BAUD:-115200}"

# Find the serial port
find_port() {
    local port
    port=$(ls /dev/cu.usbserial* 2>/dev/null | head -1)
    if [[ -z "$port" ]]; then
        echo "ERROR: No USB-serial adapter found" >&2
        exit 1
    fi
    echo "$port"
}

# Find esptool from PlatformIO installation
find_esptool() {
    local tool
    tool=$(find "$HOME/.platformio/packages" -name "esptool.py" -path "*/tool-esptoolpy*" 2>/dev/null | head -1)
    if [[ -z "$tool" ]]; then
        echo "ERROR: esptool not found. Run ./build.sh first to install PlatformIO packages." >&2
        exit 1
    fi
    echo "$tool"
}

# Build if firmware binary doesn't exist
if [[ ! -f "$BIN" ]]; then
    echo "Firmware not built yet, building..."
    "$SCRIPT_DIR/build.sh"
fi

PORT="${FLASH_PORT:-$(find_port)}"
ESPTOOL="$(find_esptool)"

echo "Port:    $PORT"
echo "Binary:  $BIN"
echo "Baud:    $BAUD"
echo ""
echo "Flashing..."
python3 "$ESPTOOL" \
    --port "$PORT" \
    --baud "$BAUD" \
    --before default_reset \
    --after soft_reset \
    write_flash 0x0 "$BIN"
