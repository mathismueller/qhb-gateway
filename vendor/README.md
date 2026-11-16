# Vendor Blobs Directory

This directory is intentionally kept empty in Git (except this file).

`qhb-gateway` does not redistribute proprietary Huawei, Telekom, or Synaptics/DSP Group binaries.
To populate `vendor/rootfs/` from your own stock NAND backup (`mtd0` / `initramfs`), run:

```bash
./scripts/extract-vendor.sh --mtd0 /path/to/mtd0_rootfs --initramfs /path/to/initramfs_root
```

When `vendor/rootfs/` is present, `board/qhb/post-build.sh` automatically stages the vendor
modules and runtime island into the generated Buildroot rootfs.
