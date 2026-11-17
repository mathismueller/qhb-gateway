#!/bin/sh
# Build the Linux uImage (optionally embedding Buildroot's rootfs.cpio) and mlog_stub.ko.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LINUX_SRC="$ROOT/linux"
KBUILD_OUT="${QHB_KBUILD_OUT:-$ROOT/output/linux}"
IMAGES_DIR="${QHB_IMAGES_DIR:-$ROOT/output/images}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabi-}"
RESCUE=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --rescue)
            RESCUE=1
            shift
            ;;
        *)
            echo "Usage: $0 [--rescue]" >&2
            exit 1
            ;;
    esac
done

[ -f "$LINUX_SRC/Makefile" ] || {
    echo "Error: linux submodule not initialized. Run: git submodule update --init --recursive" >&2
    exit 1
}

mkdir -p "$KBUILD_OUT" "$IMAGES_DIR"
cp "$ROOT/board/qhb/linux.config" "$KBUILD_OUT/.config"

# Embed Buildroot rootfs.cpio if present
if [ -f "$IMAGES_DIR/rootfs.cpio.xz" ]; then
    xz -dc "$IMAGES_DIR/rootfs.cpio.xz" > "$IMAGES_DIR/rootfs.cpio"
fi
if [ -f "$IMAGES_DIR/rootfs.cpio" ]; then
    echo "[qhb] Embedding $IMAGES_DIR/rootfs.cpio into uImage"
    "$LINUX_SRC/scripts/config" --file "$KBUILD_OUT/.config" \
        --set-str CONFIG_INITRAMFS_SOURCE "$IMAGES_DIR/rootfs.cpio"
else
    echo "[qhb] NOTE: $IMAGES_DIR/rootfs.cpio not found; building kernel without embedded rootfs."
    "$LINUX_SRC/scripts/config" --file "$KBUILD_OUT/.config" \
        --set-str CONFIG_INITRAMFS_SOURCE ""
fi

if [ "$RESCUE" = "1" ]; then
    echo "[qhb] Rescue mode: disabling CONFIG_MTD_HSAN_NAND"
    "$LINUX_SRC/scripts/config" --file "$KBUILD_OUT/.config" --disable CONFIG_MTD_HSAN_NAND
fi

JOBS="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

yes "" | make -C "$LINUX_SRC" O="$KBUILD_OUT" ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" oldconfig
make -C "$LINUX_SRC" O="$KBUILD_OUT" ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" -j"$JOBS" zImage
make -C "$LINUX_SRC" O="$KBUILD_OUT" ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" -j1 uImage

# Build out-of-tree mlog_stub.ko
make -C "$LINUX_SRC" O="$KBUILD_OUT" ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" \
    M="$ROOT/src/mlog_stub" modules

cp "$KBUILD_OUT/arch/arm/boot/uImage" "$IMAGES_DIR/uImage"
if [ -f "$ROOT/src/mlog_stub/mlog_stub.ko" ]; then
    cp "$ROOT/src/mlog_stub/mlog_stub.ko" "$IMAGES_DIR/mlog_stub.ko"
fi

(
    cd "$IMAGES_DIR"
    sha256sum uImage > SHA256SUMS.kernel
    if [ -f mlog_stub.ko ]; then
        sha256sum mlog_stub.ko >> SHA256SUMS.kernel
    fi
)
echo "[qhb] Kernel build complete: $IMAGES_DIR/uImage"
