# NAND Flash Layout & Recovery

## Partition Map

| Partition | Physical Offset | Size | Role in `qhb-gateway` |
|---|---|---|---|
| `mtd5` (`blrom`) | `0x00000000` – `0x00020000` | 128 KiB | Patched U-Boot (Mode 4 TFTP → NAND fallback). **Do not overwrite at runtime.** |
| `mtd6` (`rootfstag`) | `0x00020000` – `0x000c0000` | 640 KiB | Primary `rootfstag` (factory). |
| `mtd1` block 6 | `0x000c0000` – `0x00160000` | 640 KiB | Backup `rootfstag` (used by `SLAVESS`). |
| `mtd0` (`rootfs`) | `0x00160000` – `0x02160000` | 32 MiB | Stock rootfs (preserved as primary fallback). |
| `mtd11` (`kernelbak`) | `0x02160000` – `0x04160000` | 32 MiB | **Active `qhb-gateway` `uImage`** (kernel + embedded Buildroot `initramfs`). |
| `mtd17` (`bundles`) | `0x0e160000` – `0x19060000` | 175 MiB | **Persistent `/data` (UBIFS)** for `/data/qhb.conf`, Dropbear keys, pairing DBs. |
| `mtd4` (`upgflag`) | `0x1fd60000` – `0x1fe00000` | 640 KiB | Boot bank selection (`SLAVESS`). |

## Hardware Write-Protect (GPIO 72)

NAND writes fail with `EIO` / silent no-ops unless GPIO 72 is driven HIGH first:
```sh
/bin/hi_cli /home/cli/api/ecs/gpio/writebit -v gpio 72 level 1
```
`scripts/flash-nand.sh` handles this automatically before `flash_erase` + `nandwrite` + SHA-256 readback verification.

## Recovery

1. **TFTP Mode 4:** Power on with a TFTP server at `192.168.0.10` serving `atp.img` (your `uImage`). U-Boot probes TFTP before falling back to `mtd11`.
2. **BootROM YMODEM:** If `mtd5` is ever damaged, BootROM (`VER5610`) accepts `blpatch-standalone.bin` over serial YMODEM (`115200 8N1`).
