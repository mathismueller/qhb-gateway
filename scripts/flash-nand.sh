#!/bin/sh
# Flash a verified uImage to NAND on the running Home Base 2 (GPIO 72 WP unlock + nandwrite + readback).
# Run directly on the board: ./flash-nand.sh /tmp/uImage [mtd11]
set -eu

UIMAGE="${1:-}"
MTD_DEV="${2:-mtd11}"

if [ -z "$UIMAGE" ] || [ ! -f "$UIMAGE" ]; then
    echo "Usage: $0 <path-to-uImage> [mtd-name (default: mtd11)]" >&2
    exit 1
fi

DEV="/dev/$MTD_DEV"
[ -c "$DEV" ] || { echo "Error: MTD char device not found: $DEV" >&2; exit 1; }

SIZE="$(wc -c < "$UIMAGE" | tr -d ' ')"
EXPECTED_SHA="$(sha256sum "$UIMAGE" | awk '{print $1}')"

echo "[qhb-flash] Unlocking NAND hardware write-protect (GPIO 72 -> 1)..."
if [ -x /bin/hi_cli ]; then
    /bin/hi_cli /home/cli/api/ecs/gpio/writebit -v gpio 72 level 1
else
    echo "Error: /bin/hi_cli not found; cannot disable NAND WP on GPIO 72" >&2
    exit 1
fi

echo "[qhb-flash] Erasing $DEV..."
flash_erase "$DEV" 0 0

echo "[qhb-flash] Writing $UIMAGE ($SIZE bytes) to $DEV..."
nandwrite -p "$DEV" "$UIMAGE"

echo "[qhb-flash] Verifying readback SHA-256..."
ACTUAL_SHA="$(dd if="/dev/mtdblock${MTD_DEV#mtd}" bs=4096 count=$(( (SIZE + 4095) / 4096 )) 2>/dev/null | head -c "$SIZE" | sha256sum | awk '{print $1}')"

if [ "$EXPECTED_SHA" != "$ACTUAL_SHA" ]; then
    echo "ERROR: Readback mismatch! expected=$EXPECTED_SHA actual=$ACTUAL_SHA" >&2
    exit 1
fi

echo "[qhb-flash] Verified bit-for-bit ($EXPECTED_SHA)."
