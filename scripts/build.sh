#!/bin/sh
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BR_DIR="$ROOT/buildroot"
OUT_DIR="${QHB_OUTPUT_DIR:-$ROOT/output}"

if [ ! -f "$BR_DIR/Makefile" ]; then
    "$ROOT/scripts/fetch-buildroot.sh"
fi

mkdir -p "$OUT_DIR"

echo "[qhb] Configuring qhb_defconfig (BR2_EXTERNAL=$ROOT)"
make -C "$BR_DIR" O="$OUT_DIR" BR2_EXTERNAL="$ROOT" qhb_defconfig

echo "[qhb] Building rootfs..."
make -C "$BR_DIR" O="$OUT_DIR"

echo "[qhb] Build complete. Artifacts in $OUT_DIR/images/"
ls -lh "$OUT_DIR/images/"
