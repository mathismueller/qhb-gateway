#!/bin/sh
set -eu

BINARIES_DIR="$1"

if [ -f "$BINARIES_DIR/rootfs.cpio.xz" ]; then
    (
        cd "$BINARIES_DIR"
        sha256sum rootfs.cpio.xz > SHA256SUMS
        if [ -f rootfs.tar.gz ]; then
            sha256sum rootfs.tar.gz >> SHA256SUMS
        fi
    )
    echo "[qhb] Generated $BINARIES_DIR/SHA256SUMS"
fi
