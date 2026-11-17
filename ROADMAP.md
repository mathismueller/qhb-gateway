# `qhb-gateway` — Architecture & Execution Roadmap

> **Target Hardware:** Telekom Qivicon Home Base 2 (Typ 3101 / Huawei HiSilicon SD5650T-V110, Dual-Core Cortex-A9 **soft-float**, 512 MiB DDR3, NAND Flash)  
> **Goal:** A clean, low-maintenance, flash-resident smart-home radio gateway running on Buildroot, exposing long-lived network protocols to Home Assistant / RaspberryMatic with minimal NAND wear.  
> **Design Philosophy:** Separate **proven stable production** (Buildroot + 3.4.11 vendor kernel + clean `BR2_EXTERNAL` CI) from **long-term R&D** (6.12 LTS kernel, mainline U-Boot, blob elimination).

---

## 1. Locked Architectural Decisions

| Area | Decision | Rationale |
|---|---|---|
| **Repository** | Single monorepo (`qhb-gateway`) using Buildroot `BR2_EXTERNAL` | Clean separation from upstream Buildroot; daemons live in-tree (`src/`), kernel pinned via submodule/patchset. |
| **Bootloader** | Keep patched U-Boot in `mtd5` for production; `blpatch-standalone` for recovery | Zero maintenance today; TFTP Mode 4 + A/B flash fallback already verified. Custom/mainline U-Boot deferred to Phase 5. |
| **Storage & Wear** | Read-only `uImage` (kernel + embedded `initramfs` in RAM) + small UBIFS `/data` (`mtd17`); logs in `tmpfs` | OS runs 100% in 512 MiB RAM (immune to power loss, zero rootfs wear). Only keys, pairing DBs, and `/data/qhb.conf` touch UBIFS. |
| **Config UX** | Single declarative `/data/qhb.conf` + zero-config DHCP + mDNS defaults | One file to back up or edit; works out of the box on a new LAN. |
| **Vendor Blobs** | Clean Git repo (`scripts/extract-vendor.sh` from stock `mtd0`); CI builds open base + optional private injection | Avoids committing proprietary Huawei/Telekom/DSP Group binaries; long-term goal to replace blobs in Phase 4/5. |
| **v1.0 Radios** | Ethernet + Zigbee (`2002`), HomeMatic (`3008`), Bluetooth (`6053`), DECT (MQTT); Wi-Fi off by default | Proven wired radio bridge for Home Assistant; Wi-Fi remains an optional toggle. |
| **CI & Testing** | Two-tier CI: fast PR unit/protocol tests + cached Buildroot `uImage` builds on `main`/tags + local HIL smoke test | Fast PR feedback without burning 30-min Buildroot builds on every commit. |

---

## 2. Repository Structure (`qhb-gateway`)

Following standard Buildroot `BR2_EXTERNAL` and embedded Linux project conventions:

```text
qhb-gateway/
├── .github/workflows/
│   ├── pr-checks.yml              # Fast: daemon host unit tests, C/shell lint, dtc/defconfig checks
│   └── build-release.yml          # Cached Buildroot + uImage build on main/tags (+ optional vendor overlay)
├── board/qhb/
│   ├── linux.config               # Minimal, hardened 3.4.11 kernel defconfig (initramfs enabled)
│   ├── post-build.sh              # Stages /etc, init scripts, mknod/dev rules
│   ├── post-image.sh              # Produces uImage + flashable artifacts + checksums
│   └── rootfs_overlay/
│       ├── etc/init.d/            # S10data-ubifs, S40qhb-net, S45bluetooth, S50ser2net, S51zigbee, S60hmip, S65dect
│       └── etc/hb2/               # Default /data/qhb.conf template & mDNS service defs
├── configs/
│   └── qhb_defconfig              # Buildroot external defconfig (Cortex-A9 soft-float, musl/uClibc-ng)
├── external.desc                  # BR2_EXTERNAL metadata (name: QHB_GATEWAY)
├── external.mk                    # Includes package/*/*.mk
├── Config.in                      # Top-level BR2_EXTERNAL menuconfig entries
├── package/                       # Buildroot packages for in-tree daemons
│   ├── qhb-zigbeed/
│   ├── qhb-btproxy/
│   ├── hb2-dectd/
│   └── hb2-rfe-daemon/
├── src/                           # Clean C/Python/Go daemon sources + host unit tests
│   ├── qhb-zigbeed/               # ASHv2 TCP (2002) <-> EZSP-SPI (/dev/ttyspi0)
│   ├── qhb-btproxy/               # ESPHome BT Proxy TCP (6053) <-> BlueZ/HCI
│   ├── hb2-dectd/                 # MQTT + HA Discovery <-> voiper UDP (3490)
│   ├── hb2-rfe-daemon/            # HB-RF-ETH TCP (3008) <-> eq3_char_loop / UART1
│   └── mlog_stub/                 # Out-of-tree kernel stub for hisi_spi2tty.ko
├── linux/                         # Pinned kernel submodule (neuschaefer/linux @ vendor/telekom-homebase2)
├── vendor/                        # .gitignore'd except README.md (populated locally by extract-vendor.sh)
├── scripts/
│   ├── extract-vendor.sh          # Extracts .ko, voiper, dectimage.bin, hi_cli, uClibc from stock mtd0
│   ├── flash-nand.sh              # Safe A/B flasher (GPIO 72 WP unlock + nandwrite + sha256 verify)
│   └── hil-smoke-test.py          # Local TFTP boot + serial + TCP port probe smoke test
├── docs/
│   ├── FLASH_LAYOUT.md            # NAND partition map, A/B recovery, unbricking
│   └── PROTOCOLS.md               # HA / RaspberryMatic / MQTT integration guide
└── ROADMAP.md
```

---

## 3. Flash Layout & Low-Wear Storage Design

We keep the bootloader (`mtd5`) and A/B kernel banks (`mtd10`/`mtd11`) intact, and reclaim the unused Qivicon Java/OSGi `bundles` partition (`mtd17`) for persistent state:

| Partition | Size | Role | Wear / Filesystem |
|---|---|---|---|
| `mtd5` (`blrom`) | 128 KiB | Patched U-Boot (TFTP Mode 4 → NAND fallback) | **Read-only** (never touched at runtime) |
| `mtd10` / `mtd11` | 32 MiB × 2 | **Bank A / Bank B `uImage`** (Kernel + embedded Buildroot `initramfs`) | **Read-only** (written only on firmware upgrade) |
| `mtd17` (`bundles`) | 175 MiB | **Persistent `/data`** (`qhb.conf`, Dropbear host keys, Zigbee/DECT pairing DBs) | **UBIFS** (wear-leveled, power-cut safe, low write volume) |
| RAM (`512 MiB`) | ~450 MiB free | Rootfs (`initramfs`), `/var/log`, `/tmp`, `voiper` runtime | **`tmpfs`** (zero NAND writes during normal operation) |

**Wear-minimization rules:**
- All logs (`syslog`, daemon stdout/stderr) go to ring-buffered `tmpfs` (`/var/log`), never NAND.
- `/data` is only written on initial provisioning, key generation, or device pairing changes.
- Firmware upgrades flash the inactive kernel bank (`mtd10`/`mtd11`) and verify SHA-256 before switching.

---

## 4. Long-Lived Network Protocols (v1.0)

Zero smart-home application logic runs on the box; all protocols are thin, standard wire bridges:

| Subsystem | Port / Transport | Protocol | Consumer |
|---|---|---|---|
| **Zigbee (`EM3587`)** | TCP `2002` | ASHv2 over TCP (`qhb-zigbeed`) | Home Assistant ZHA (`socket://<ip>:2002`) |
| **HomeMatic (`Si1002`)** | TCP `3008` | `HB-RF-ETH` (`qhb-rfe-daemon`) | RaspberryMatic / OpenCCU / debmatic |
| **Bluetooth (`RTL8761AUV`)** | TCP `6053` | ESPHome Bluetooth Proxy (`qhb-btproxy`) | Home Assistant ESPHome integration |
| **DECT ULE (`DCX81`)** | MQTT | MQTT + HA Discovery (`qhb-dectd`) | Home Assistant MQTT integration |
| **Future USB Dongles** | TCP (optional) | Raw `ser2net` / RFC 2217 | Z2M / OTBR (Thread) via USB 2.0 ports |

---

## 5. Phased Execution Roadmap

### Phase 0: Repository Bootstrap & Migration (Clean Slate)
- [x] Initialize `qhb-gateway` git repo with `BR2_EXTERNAL` skeleton (`external.desc`, `external.mk`, `Config.in`, `configs/qhb_defconfig`).
- [x] Fork `neuschaefer/linux` to `mathismueller/linux` (`qhb/3.4.11` with local build/GCC fixes) and pin `linux/` submodule.
- [x] Migrate clean daemon sources (`qhb-zigbeed`, `qhb-btproxy`, `qhb-dectd`, `qhb-rfe-daemon`, `mlog_stub`) from `quivcon_homebase_2_reverse_engineering` into `src/` + `package/*/`.
- [x] Write `scripts/extract-vendor.sh` (`--bin` raw `mtd0_rootfs.bin` via `jefferson` or `--mtd0`) to stage vendor island into `vendor/rootfs/`.
- [x] Document NAND layout & recovery (`blpatch-standalone.bin`) in `docs/FLASH_LAYOUT.md`.

### Phase 1: Self-Contained Flashable Buildroot Image (v1.0-rc1)
- [x] Wire `scripts/build-kernel.sh` + `board/qhb/linux.config` to embed Buildroot's `rootfs.cpio` into `uImage` (`CONFIG_INITRAMFS_SOURCE`) and compile `mlog_stub.ko`.
- [x] Implement `/etc/init.d/S10data-ubifs` to mount pre-provisioned `ubi0:qhb_data` on `mtd17` and fall back to `tmpfs` (zero-write safe during TFTP testing).
- [x] Implement `/data/qhb.conf` loader + migrated init scripts (`S40qhb-net` … `S65qhb-dect`) with zero-config DHCP defaults and Wi-Fi off by default.
- [ ] Verify all 4 radios + Dropbear SSH + `tmpfs` logging on real hardware via TFTP `atp.img` (zero USB stick attached).
- [ ] Provision `mtd17` UBIFS (`ubiformat`/`ubimkvol`), flash `uImage` to `mtd11`, and verify cold unattended power-on boot.

### Phase 2: CI Pipeline & Automated Verification (v1.0)
- [x] **PR Workflow (`.github/workflows/pr-checks.yml`):**
  - `shellcheck` on all scripts & init scripts, `uv` Python syntax check, and host-compiled `gcc` checks for all 4 daemons.
  - [ ] Add host unit & framing tests for `qhb-zigbeed`, `qhb-dectd`, `qhb-rfe-daemon`, and `qhb-btproxy`.
- [x] **Release Workflow (`.github/workflows/build-release.yml`):**
  - Cached Buildroot `BR2_EXTERNAL` + `uImage` + `mlog_stub.ko` build on `main` and version tags (`v*`).
- [ ] **Local Hardware-in-the-Loop (`scripts/hil-smoke-test.py`):**
  - Script implemented; run end-to-end against physical board over TFTP + serial + SSH.

### Phase 3: Kernel Hardening & Stabilization (v1.1)
- [ ] Backport `3.4.11 → 3.4.113` stable security fixes onto `mathismueller/linux` (`qhb/3.4.11`) while verifying 100% ABI compatibility with vendor `.ko` modules.
- [ ] Strip unused kernel protocols/filesystems and lock down default firewall rules (expose only SSH + configured radio bridge ports).
- [x] Add A/B safe NAND flash script (`scripts/flash-nand.sh` with GPIO 72 WP unlock + SHA-256 readback verify).

### Phase 4: Blob Elimination (Long-Term Independence)
- [ ] Replace `hi_kspi.ko` + `hisi_spi2tty.ko` with an open SD5650T SPI driver (or `spidev` + userspace EZSP-SPI directly inside `qhb-zigbeed`).
- [ ] Investigate replacing `voiper` / `hi_cli` dependencies where practical (e.g. direct `hi_cli` register equivalents for GPIO 72 & switch `fid 268` rule).
- [ ] Evaluate replacing `rtl8192cd.ko` with mainline `mac80211` if Wi-Fi is desired on newer kernels.

### Phase 5: Mainline 6.x / 6.12 LTS Kernel & Custom U-Boot (R&D Track)
> *Runs on a separate branch without blocking v1.x stability; reuses the exact same Buildroot `rootfs.cpio.xz`.*
- [ ] **Minimal Mainline Boot:** Boot Linux 6.12 LTS + appended DTS (`CONFIG_ARM_APPENDED_DTB` + `CONFIG_ARM_ATAG_DTB_COMPAT`) via TFTP with GIC, SP804, PL310, SMP (`hi_platsmp`), and `amba-pl011` UART0/1.
- [ ] **Standard Peripherals:** Port USB 2.0 (`EHCI`/`OHCI`/`xHCI` + `btusb` for RTL8761AUV), NAND (`hinfc` / `hi_nand`), and PCIe (`hi_pcie`).
- [ ] **Hard Wall — Internal Ethernet:** Disassemble U-Boot's minimal standalone GMAC driver + `hi_keth.ko` CFE bypass (`fid 268`) to write a clean open-source Linux 6.x Ethernet driver.
- [ ] **Custom / Mainline U-Boot (Optional):** Replace `mtd5` binary-patched U-Boot with a clean, source-built U-Boot once DDR3 + NANDC + GMAC init are fully understood.
