#!/bin/sh
# Extract proprietary vendor blobs (.ko modules, voiper, dectimage.bin, hi_cli, uClibc)
# from a raw stock Home Base 2 mtd0 JFFS2 dump (--bin) or an already-unpacked mtd0 directory (--mtd0).
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MTD0_BIN=""
MTD0_DIR=""
INITRAMFS_DIR=""
OUT_DIR="$ROOT/vendor/rootfs"
TMP_DIR=""

usage() {
    echo "Usage: $0 --bin <mtd0_rootfs.bin> | --mtd0 <unpacked_mtd0_dir> [--initramfs <dir>]"
    exit 1
}

cleanup() {
    if [ -n "$TMP_DIR" ] && [ -d "$TMP_DIR" ]; then
        rm -rf "$TMP_DIR"
    fi
}
trap cleanup EXIT

while [ "$#" -gt 0 ]; do
    case "$1" in
        --bin)
            MTD0_BIN="${2:-}"
            shift 2
            ;;
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

if [ -n "$MTD0_BIN" ]; then
    [ -f "$MTD0_BIN" ] || { echo "Error: --bin file not found: $MTD0_BIN" >&2; usage; }
    command -v jefferson >/dev/null 2>&1 || {
        echo "Error: 'jefferson' is required to unpack $MTD0_BIN (e.g. brew install jefferson)" >&2
        exit 1
    }
    TMP_DIR="$(mktemp -d)"
    MTD0_DIR="$TMP_DIR/mtd0"
    echo "[qhb] Unpacking $MTD0_BIN with jefferson..."
    jefferson "$MTD0_BIN" -d "$MTD0_DIR"
fi

[ -n "$MTD0_DIR" ] && [ -d "$MTD0_DIR" ] || {
    echo "Error: provide --bin <mtd0_rootfs.bin> or --mtd0 <unpacked_mtd0_dir>" >&2
    usage
}

# All vendor modules/libs live in mtd0 itself; allow optional --initramfs override
LIB_SRC="${INITRAMFS_DIR:-$MTD0_DIR}"

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/lib/extra" "$OUT_DIR/lib/vendor" "$OUT_DIR/bin" "$OUT_DIR/home" "$OUT_DIR/etc"

# Kernel modules (.ko)
cp "$LIB_SRC/lib/extra/"*.ko "$OUT_DIR/lib/extra/" 2>/dev/null || true
if [ -f "$LIB_SRC/lib/kernel/drivers/char/eq3_char_loop.ko" ]; then
    cp "$LIB_SRC/lib/kernel/drivers/char/eq3_char_loop.ko" "$OUT_DIR/lib/extra/"
fi
if [ -f "$LIB_SRC/lib/kernel/drivers/bluetooth/rtk_btusb.ko" ]; then
    cp "$LIB_SRC/lib/kernel/drivers/bluetooth/rtk_btusb.ko" "$OUT_DIR/lib/extra/"
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
    if [ -f "$LIB_SRC/lib/$lib" ]; then
        cp "$LIB_SRC/lib/$lib" "$OUT_DIR/lib/vendor/"
    fi
done

if [ -f "$OUT_DIR/lib/vendor/ld-uClibc-0.9.32.1-git.so" ]; then
    ln -sf ld-uClibc-0.9.32.1-git.so "$OUT_DIR/lib/vendor/ld-uClibc.so.0"
    ln -sf ld-uClibc-0.9.32.1-git.so "$OUT_DIR/lib/vendor/ld.so"
    if [ -f "$OUT_DIR/lib/vendor/libuClibc-0.9.32.1-git.so" ]; then
        ln -sf libuClibc-0.9.32.1-git.so "$OUT_DIR/lib/vendor/libc.so.0"
    fi
    if [ -f "$OUT_DIR/lib/vendor/libdl-0.9.32.1-git.so" ]; then
        ln -sf libdl-0.9.32.1-git.so "$OUT_DIR/lib/vendor/libdl.so.0"
    fi
fi

# Vendor CLI tools & DECT firmware
for bin in hi_cli hi_xpon ethcmd; do
    if [ -f "$MTD0_DIR/bin/$bin" ]; then
        cp "$MTD0_DIR/bin/$bin" "$OUT_DIR/bin/"
    fi
done

# Patch hi_xpon DT_NEEDED: replace unused libpthread.so.0 with libc.so.0
if [ -f "$OUT_DIR/bin/hi_xpon" ]; then
    python3 - "$OUT_DIR/bin/hi_xpon" <<'PY'
import sys
path = sys.argv[1]
with open(path, "r+b") as f:
    data = bytearray(f.read())
    target = b"libpthread.so.0"
    replacement = b"libc.so.0\x00\x00\x00\x00\x00\x00"
    pos = data.find(target)
    if pos != -1:
        data[pos:pos + len(target)] = replacement
        f.seek(0)
        f.write(data)
        print(f"[qhb] Patched hi_xpon DT_NEEDED libpthread.so.0 -> libc.so.0 at {hex(pos)}")
PY
fi
if [ -d "$MTD0_DIR/home/cli" ]; then
    cp -r "$MTD0_DIR/home/cli" "$OUT_DIR/home/"
fi
if [ -f "$MTD0_DIR/etc/dectimage.bin" ]; then
    cp "$MTD0_DIR/etc/dectimage.bin" "$OUT_DIR/etc/"
fi
if [ -f "$MTD0_DIR/bin/voiper" ]; then
    cp "$MTD0_DIR/bin/voiper" "$OUT_DIR/bin/voiper.real"
fi

echo "[qhb] Vendor overlay staged at $OUT_DIR"
