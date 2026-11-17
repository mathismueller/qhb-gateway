# Vendor Blobs Directory

This directory is intentionally kept empty in Git (except this file).

`qhb-gateway` does not redistribute proprietary Huawei, Telekom, or Synaptics/DSP Group binaries.
To populate `vendor/rootfs/` directly from a raw stock `mtd0_rootfs.bin` NAND dump (requires `jefferson`, e.g. `brew install jefferson`), run:

```bash
./scripts/extract-vendor.sh --bin /path/to/mtd0_rootfs.bin
```

Or if `mtd0` is already unpacked:

```bash
./scripts/extract-vendor.sh --mtd0 /path/to/unpacked_mtd0
```

When `vendor/rootfs/` is present, `board/qhb/post-build.sh` automatically stages the vendor
modules and runtime island into the generated Buildroot rootfs.
