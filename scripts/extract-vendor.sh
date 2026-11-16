#!/bin/sh
# Extract proprietary vendor blobs (.ko modules, voiper, dectimage.bin, hi_cli, uClibc)
# from a user-supplied stock Home Base 2 NAND dump into vendor/rootfs/.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MTD0_DIR=""
INITRAMFS_DIR=""
OUT_DIR="$ROOT/vendor/rootfs"

usage() {
    echo "Usage: $0 --mtd0 <extracted_mtd0_dir> --initramfs <extracted_initramfs_dir>"
    exit 1
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --mtd0)
            MTD0_DIR="${2:-}"
            shift 2
            ;;
        --initramfs)
            INITRAMFS_DIR="${2:-}"
            shift 2
            ;;
        *)
            usage
            ;;
    esac
done

[ -d "$MTD0_DIR" ] || { echo "Error: --mtd0 directory not found: $MTD0_DIR" >&2; usage; }
[ -d "$INITRAMFS_DIR" ] || { echo "Error: --initramfs directory not found: $INITRAMFS_DIR" >&2; usage; }

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/lib/extra" "$OUT_DIR/lib/vendor" "$OUT_DIR/bin" "$OUT_DIR/home" "$OUT_DIR/etc"

# Kernel modules (.ko)
cp "$INITRAMFS_DIR/lib/extra/"*.ko "$OUT_DIR/lib/extra/" 2>/dev/null || true
if [ -f "$INITRAMFS_DIR/lib/kernel/drivers/char/eq3_char_loop.ko" ]; then
    cp "$INITRAMFS_DIR/lib/kernel/drivers/char/eq3_char_loop.ko" "$OUT_DIR/lib/extra/"
fi

# Patch hisi_spi2tty.ko major 254 -> 220 (avoids collision with xz_dec_test)
if [ -f "$OUT_DIR/lib/extra/hisi_spi2tty.ko" ]; then
    python3 - "$OUT_DIR/lib/extra/hisi_spi2tty.ko" <<'PY'
import sys
path = sys.argv[1]
with open(path, "r+b") as f:
    data = bytearray(f.read())
    pos = data.find(bytes.fromhex("fe20a0e3"))
    if pos != -1:
        data[pos] = 0xdc
        f.seek(0)
        f.write(data)
        print(f"[qhb] Patched hisi_spi2tty.ko major 254 -> 220 at {hex(pos)}")
PY
fi

# Isolated vendor uClibc 0.9.32 runtime island in /lib/vendor
for lib in ld-uClibc-0.9.32.1-git.so libuClibc-0.9.32.1-git.so libdl-0.9.32.1-git.so \
           libgcc_s.so.1 libmsgapi.so libcfmapi.so liblgplutil.so libbhalapi.so libflashapi.so; do
    [ -f "$INITRAMFS_DIR/lib/$lib" ] && cp "$INITRAMFS_DIR/lib/$lib" "$OUT_DIR/lib/vendor/" || true
done

if [ -f "$OUT_DIR/lib/vendor/ld-uClibc-0.9.32.1-git.so" ]; then
    ln -sf ld-uClibc-0.9.32.1-git.so "$OUT_DIR/lib/vendor/ld-uClibc.so.0"
    ln -sf ld-uClibc-0.9.32.1-git.so "$OUT_DIR/lib/vendor/ld.so"
    [ -f "$OUT_DIR/lib/vendor/libuClibc-0.9.32.1-git.so" ] && ln -sf libuClibc-0.9.32.1-git.so "$OUT_DIR/lib/vendor/libc.so.0"
    [ -f "$OUT_DIR/lib/vendor/libdl-0.9.32.1-git.so" ] && ln -sf libdl-0.9.32.1-git.so "$OUT_DIR/lib/vendor/libdl.so.0"
fi

# Vendor CLI tools & DECT firmware
for bin in hi_cli hi_xpon ethcmd; do
    [ -f "$MTD0_DIR/bin/$bin" ] && cp "$MTD0_DIR/bin/$bin" "$OUT_DIR/bin/"
done
[ -d "$MTD0_DIR/home/cli" ] && cp -r "$MTD0_DIR/home/cli" "$OUT_DIR/home/"
[ -f "$MTD0_DIR/etc/dectimage.bin" ] && cp "$MTD0_DIR/etc/dectimage.bin" "$OUT_DIR/etc/"
[ -f "$MTD0_DIR/bin/voiper" ] && cp "$MTD0_DIR/bin/voiper" "$OUT_DIR/bin/voiper.real"

echo "[qhb] Vendor overlay staged at $OUT_DIR"
