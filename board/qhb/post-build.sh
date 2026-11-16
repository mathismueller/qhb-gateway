#!/bin/sh
set -eu

TARGET_DIR="$1"
EXTERNAL_DIR="${BR2_EXTERNAL_QHB_GATEWAY_PATH:-$(cd "$(dirname "$0")/../.." && pwd)}"
VENDOR_ROOTFS="${QHB_VENDOR_DIR:-$EXTERNAL_DIR/vendor/rootfs}"

# Ensure mountpoints and tmpfs-backed runtime directories exist
mkdir -p "$TARGET_DIR/data" "$TARGET_DIR/var/log" "$TARGET_DIR/var/run"

# Seed default config template
if [ -f "$TARGET_DIR/etc/qhb/qhb.conf.default" ]; then
    chmod 0644 "$TARGET_DIR/etc/qhb/qhb.conf.default"
fi

# Inject extracted proprietary vendor island when available
if [ -d "$VENDOR_ROOTFS" ]; then
    echo "[qhb] Staging vendor overlay from $VENDOR_ROOTFS"
    cp -a "$VENDOR_ROOTFS/." "$TARGET_DIR/"
else
    echo "[qhb] NOTE: $VENDOR_ROOTFS not found; building open-source base rootfs only."
fi
