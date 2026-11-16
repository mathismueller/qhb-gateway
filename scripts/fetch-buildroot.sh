#!/bin/sh
set -eu

BUILDROOT_VERSION="2025.02.18"
BUILDROOT_URL="https://buildroot.org/downloads/buildroot-${BUILDROOT_VERSION}.tar.gz"
BUILDROOT_SHA256="0620c699b3a1da41cc854bc8584692b913e57e284d3328121c674a43b8707ab1"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARGET_DIR="$ROOT/buildroot"
TARBALL="$ROOT/dl/buildroot-${BUILDROOT_VERSION}.tar.gz"

if [ -f "$TARGET_DIR/Makefile" ]; then
    echo "[qhb] Buildroot ${BUILDROOT_VERSION} already present at $TARGET_DIR"
    exit 0
fi

mkdir -p "$ROOT/dl"
if [ ! -f "$TARBALL" ]; then
    echo "[qhb] Downloading $BUILDROOT_URL"
    curl -fL "$BUILDROOT_URL" -o "$TARBALL"
fi

echo "$BUILDROOT_SHA256  $TARBALL" | sha256sum -c -

echo "[qhb] Extracting Buildroot ${BUILDROOT_VERSION}..."
mkdir -p "$TARGET_DIR"
tar -xzf "$TARBALL" -C "$TARGET_DIR" --strip-components=1
echo "[qhb] Buildroot ready."
