# iOS 16 on the OnePlus 6T (`fajita`)

A port of [ios-sweet](https://github.com/dipa-studio/ios-sweet) to the OnePlus 6T (Qualcomm SDM845). The approach is the same as on sweet: Apple's open-source XNU, booted from RAM by our shim with `fastboot boot`, runs the unmodified iPhone X iOS 16.7.16 userland from an HFS+ volume in `userdata`.

> **Status: written and reviewed, not yet booted.** Everything here is built from mainline Linux sources (device trees and drivers) and from the sweet code. Nothing has run on a 6T yet. The shim builds and the kernel patches apply cleanly to the sweet series, but the kernel itself still needs a CI build, and every hardware assumption needs a first boot to confirm it. The boot order below is designed so each step tells you which assumption failed.

## Quick start on a new computer

On Linux (Ubuntu 24.04 tested; Fedora and Arch work with their own package names):

```
git clone -b fajita https://github.com/anoshmisiosdev/ios-sweet && cd ios-sweet
./fajita.sh deps          # lists anything missing and the install command; builds apfs-fuse if needed
gh auth login             # or: export GITHUB_TOKEN=... (needed to download CI artifacts)
./fajita.sh all
```

`all` runs these steps, each of which also works on its own and resumes where it stopped:

| Step | What it does | Needs |
| --- | --- | --- |
| `kernel` | Downloads the newest successful `xnu` build of this branch from GitHub Actions into `xnu/out/` | `gh` login or `GITHUB_TOKEN` |
| `ipsw` | Looks up the iPhone X iOS 16.7.16 (20H392) IPSW on ipsw.me and pulls only the needed parts from Apple's CDN (OS image, both cryptexes, BuildManifest, D22AP device tree), ~7 GB, resumable and CRC-checked. `./fajita.sh ipsw BUILD` picks another iOS 16 build; `python3 tools/ipsw-fetch.py x --list` lists them | ~8 GB disk |
| `root` | Builds `build/ios16-root.img` and the sparse `build/ios16-root.simg` for the phone, with sweet's root tweaks applied | sudo, ~25 GB disk |
| `qemu-smoke` | Boots the kernel alone in QEMU for a minute (`build/qemu-smoke/out/fb.png`) | — |
| `qemu-ios` | Makes a trimmed copy of the root, loads it as a RAM disk, boots iOS in QEMU and saves a screenshot every minute to `build/qemu-ios/out/snaps/` | lots of RAM (below) |
| `phone` | Builds `shim/out/shim-boot.img` for the 6T and prints the fastboot commands. It never runs them | `dtb/fajita-appended.dtb` (step 0 below) |

About `qemu-ios`. It shows what iOS draws, including how the 1125×2436 iPhone X screen is cropped to the 6T's 2340 rows, without touching the phone. It is **experimental**: sweet only ever used QEMU for the kernel, and booting the whole iOS userland from a RAM disk has not been tried yet.
- **Memory:** the trimmed root is about as large as its contents (~10 GB or more), and the guest gets that plus 3 GB. Expect to need **~32 GB of host RAM**. The script warns when there isn't enough.
- **Speed:** without hardware acceleration it emulates every instruction and can take hours. On an arm64 Linux host with `/dev/kvm`, or an Apple silicon Mac (`hvf`), it runs on the real CPU (`QEMU_ACCEL=kvm|hvf`; picked automatically). The kernel uses 16K pages, so the host CPU must support them too.
- **Run length:** `QEMU_SECS` sets how long it runs (default 3 hours).

## Why the 6T is close to sweet

SDM845 and sweet's SM7150 share most of the I/O map, so most of sweet's drivers keep working unchanged:

| Block | sweet (SM7150) | fajita (SDM845) | Change |
| --- | --- | --- | --- |
| GIC v3 | `0x17a00000` / `0x17a60000` | same | none |
| UFS host | `0x1d84000` | same | none |
| CPUs | 8, MPIDR `0x000`–`0x700` | same (4× A55 + 4× A75) | name, clock only |
| Timer | vtimer INTID 19, 19.2 MHz | same | none |
| SPMI PMIC arbiter | v5 at `0xc440000` | same | none |
| USB (DWC3, QUSB2, QSCRATCH) | `0xa600000`, `0x88e2000` | same | 4 GCC clock-status offsets |
| Splash framebuffer | `0x9c000000`, 1080×2400 | `0x9d400000`, **1080×2340** | constants |
| Panel | video mode (DSC) | **command mode** (s6e3fc2x01) | autorefresh / kick, see below |
| RPMh apps DRV2 | `0x18220000` | `0x179e0000` | base |
| Touch | Goodix GT9896, GENI SE7 | **Synaptics S3706B (RMI4)**, GENI SE4 `0xa90000` | new driver |
| Volume keys | PM6150L GPIO2 / RESIN | PM8998 **GPIO6 / GPIO5** | key map |
| Touch power | L11C | L28A + GPIO88 (1.8 V) | rail |
| Boot image | v2, separate DTB | **v0, Image.gz + appended DTB** | build.sh |

## The display: command mode

The 6T's panel keeps its own frame memory. The display engine (DPU) only sends it a frame when triggered. ABL sends the splash once and stops, so pixels written into the splash buffer would never reach the screen.

- **Shim (`shim/fajita_disp.c`):** before drawing anything, it checks that the display engine is powered (dispcc GDSC and clocks). It then finds the command-mode CTL and the ping-pong block with tear check enabled, and sets **`PP_AUTOREFRESH_CONFIG`** so the DPU re-sends the frame on every tear-effect pulse. As in OnePlus's own display driver, `CTL_START` is never written while autorefresh runs. It also checks which source pipe fetches the splash buffer. After that, the panel behaves like sweet's video-mode panel for the shim, XNU's early console and every iOS frame.
- **Kernel (patch 0083):** `AppleCLCD` reads the DPU state again. If autorefresh is off, it writes `CTL_START` once per vsync tick, skipping ticks where the previous transfer is still running. Boot-arg `fajita_dpu=0|1|2`: never touch the DPU, auto (default), or switch autorefresh off and always kick.

The first shim screen prints a `DPU` block (CTL, PP, autorefresh before/after, dispcc status). **If you see that screen at all, the display path works.**

## What's in the branch

- **`shim/`:** 6T framebuffer, and the display autorefresh that runs first. There is no IMEM restart-reason write (the address is unverified on SDM845); reset goes through PSCI. `build.sh` produces `Image.gz` plus the appended stock DTBs in a header-v0 `boot.img`, offsets as postmarketOS uses (base 0, kernel 0x8000, ramdisk 0x1000000, tags 0x100, page 4096).
- **`tools/mkadt.py`:** splash `0x9d400000`, 1080×2340, A55/A75 clocks, `oneplus,fajita` compatible. `--highmem` puts the kernel in 3 GiB above 4 GiB, which every 6T variant has. `--mem-size` lets you raise that after checking.
- **`tools/fajita-dtb.py`:** `extract` the appended DTBs from your stock `boot.img`; `check` the live FDT against this port's memory and splash assumptions.
- **`xnu/patches/0082-fajita-board-retarget.patch`:** early boot band and console at the new framebuffer, touch-to-screen mapping for 2340 rows, keys on PM8998 GPIO5/6, RPMh at `0x179e0000` with an L28A vote (plus a fixed cmd-db fallback at `0x85fe0000`), SDM845 USB clock offsets, CPU name, 3700 mAh battery.
- **`xnu/patches/0083-fajita-dpu-command-mode-kick.patch`:** the kernel-side fallback described above.
- **`xnu/patches/0084-fajita-touch-synaptics-rmi4.patch`:** replaces the Goodix/FocalTech code with an RMI4 driver on sweet's GENI I²C engine. It covers page select, PDT scan (ignores duplicate entries, refuses a controller stuck in its bootloader), F01 no-sleep plus configured, the F12 register-descriptor parser (unit-tested on the host against Linux's semantics), and F12 object data scaled to 1080×2340. It reads only as many interrupt-status bytes as the PDT declares, and votes only the SDM845 QUP clocks (m-ahb, s-ahb, SE4). If the descriptors are unreadable it falls back to 10 objects at the F12 data base. The bus runs at 400 kHz as in the device tree (`fajitatouchhz=100` to slow it).

The kernel keeps sweet's machine config name `SWEET`. The patches only change board constants and drivers, so the CI workflow is unchanged.

## Doing it

You need an unlocked 6T, a Linux PC, and a GitHub fork for the kernel build. **`userdata` is erased.** Bootloaders, modem, persist and the other partitions are never touched. The 6T can always be restored with the MSM Download Tool, but back up `persist` first anyway.

### 0. Back up and get the device trees

Boot a recovery with root (TWRP or OrangeFox for fajita) with `fastboot boot`, then pull at least `persist`, `modemst1`, `modemst2`, `fsg`, `fsc` and your current `boot_a`/`boot_b`. Then:

```
python3 tools/fajita-dtb.py extract backup/boot_a.img dtb      # -> dtb/fajita-appended.dtb
adb shell 'cat /sys/firmware/fdt' > dtb/live.dtb                # from recovery, as root
python3 tools/fajita-dtb.py check dtb/live.dtb
```

`check` must end with `consistent with this port`. If the splash address or the memory bank differs on your phone, fix `FB_BASE` (`shim/console.h`, `shim/mmu.h`, `tools/mkadt.py`, patch 0082) or `--mem-size` before continuing.

### 1. Shim-only smoke test (no kernel yet, nothing written)

```
shim/build.sh
fastboot boot shim/out/shim-boot.img
```

With no payload, the shim shows its diagnostics screen and probes UFS read-only. What you see answers the first questions:

| Screen | Meaning |
| --- | --- |
| Red screen with `DPU autorefresh on` | Display path solved. Note the CTL/PP numbers. |
| Text appears only in pieces, or `DPU` says `not sticky` / `no tear check` | Autorefresh didn't take; the kernel's kick fallback will be needed. Send me the screen. |
| Splash stays frozen, nothing changes | Either ABL never passed control or the display never updated. `fastboot boot` of a stock kernel still working rules out the first. |
| `DPU autorefresh on, splash not fetched` | The engine refreshes, but from another buffer (`DPU PIPE n=FFFFFFFF`). Expect a frozen ABL screen; send me the DPU lines. |
| `DPU MDSS off, untouched` | ABL turned the display off before jumping. That needs a different approach. |

### 2. Kernel

Push this branch to your fork and run the workflow, as for sweet:

```
gh workflow run xnu.yml -R YOU/ios-sweet --ref fajita
gh run download RUN_ID -R YOU/ios-sweet -n xnu-kernel -D xnu/out
```

Check the build log for `Skipping patch 008…`. blacktop's script **skips** a patch that doesn't apply instead of failing, so make sure 0082–0084 were applied.

### 3. iOS root (same as sweet)

Build `ios16-root.img` from the iPhone X 16.7.16 IPSW with `tools/mkiosroot.sh` and `tools/sweet-root-tweaks.py`, exactly as in the README. The 6T's `userdata` is ~50 GB or more, so the 16 GB image fits.

### 4. Boot

```
python3 tools/mkadt.py shim/out/fajita.adt --highmem
python3 tools/mkpayload.py xnu/out/kernel.development.sweet shim/out/fajita.adt shim/out/payload.bin \
    --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetusb=2'
shim/build.sh ../shim/out/payload.bin
fastboot -S 512M flash userdata ios16-root.simg      # erases userdata
fastboot boot shim/out/shim-boot.img
```

- **Start with `cpus=1`.** Raise to `cpus=8` once SpringBoard is up.
- **`sweetusb=2`** brings up the USB Ethernet live log. `tools/sweet-usbnet.sh logs/boot.txt` on the PC shows the kernel console without filming the screen.
- **After a hang,** boot recovery and run `tools/sweet-log-pull.sh logs/boot.txt`. It finds `userdata` by name.
- **Touch starts 45 s after boot** (`sweettouchdelay`), so a hang there can't hide earlier logs.

## GPU (Adreno 630), branch `fajita-gpu`: work in progress

The approach and most of the plumbing come from [joranv1/ios-sailfish](https://github.com/joranv1/ios-sailfish), which brought up the Adreno 530 on the Pixel 1. Credit for the design goes to them: the SweetGPU IOKit user client, the `sweet_gpu.h` kernel API, the IOSurface fixes, `sweetenv` dylib injection, and Mesa freedreno running behind an `IOAcceleratorES` service. Patch `0085-fajita-gpu-plumbing.patch` carries that plumbing unchanged, plus a read-only cmd-db lookup for the GMU (`sweet_rpmh_cmd_db_aux`).

Patch `0086-fajita-adreno630.patch` adds the A630 hardware driver, `bsd/dev/sweet_gpu_a6xx.c`: GCC/GPUCC clocks and the CX GDSC, the GPU SMMU, the GMU with HFI (firmware, arc votes, queues), the command processor with the zap shader and a compute self-test, and the service mode for the userland. It is polling-only (no interrupts) and gets its firmware from the phone (below). With `sweetgpu=0` (the default) it does nothing, so the branch boots like `fajita`.

Firmware comes from your own phone and is never committed:

```
./fajita.sh gpu-fw              # prints the adb pull commands
./fajita.sh gpu-fw build/gpu-fw-src
SWEETGPU=1 ./fajita.sh phone    # boot image with the blobs + sweetgpu=1
```

`tools/fajita-gpu-fw.py` checks the files and squashes a split zap shader (`a630_zap.mdt` + `.b0N`) into `a630_zap.mbn`. `mkpayload.py --blob` (also from ios-sailfish) puts the files in the ADT as `GPU-SQE`, `GPU-GMU` and `GPU-ZAP`.

Bring-up levels for `sweetgpu=` (0 = off, the default): 1 clocks and power + GPU id, 2 adds IOMMU + GMU + HFI, 3 adds the command processor + zap shader + a self-test, 8 is service mode for the userland. Step up one level per boot; after a hang, the UFS log shows the last stage reached.

After the kernel driver: Turnip (Mesa's Vulkan driver) on the same interface, then a Metal driver bundle on top of Vulkan, registered through `IOAcceleratorES` `MetalPluginName`.

## Review

Every register address and protocol detail was checked against mainline Linux (and, for the display and clocks, OnePlus's downstream SDM845 kernel) by a separate review pass. Its fixes are in: SDM845-only QUP clock votes, no `CTL_START` while autorefresh runs, correct RMI4 bootloader checks (F01 bit 6, F34 bit 7), function base `0x00` accepted, PDT-sized interrupt reads, Linux-compatible descriptor truncation, and the splash-pipe check. The IMEM restart-reason address on the 6T turned out to differ from sweet's (`0x146bf65c`), so dropping that write was right.

## What will most likely need a second look

1. **Ramdisk placement.** The payload (~15 MB+) has to fit where ABL puts the ramdisk, below `hyp_mem` at `0x85700000`. If the shim says `ramdisk overlaps` or ABL refuses the image, the payload can be appended to the Image instead, as topaz does.
2. **Touch power-up.** These are unconfirmed: GPIO99 as reset (from the OnePlus downstream driver, not mainline), the L28A vote, and the order of the GPIO88 1.8 V switch. The log prints every PDT entry and the F12 layout it found.
3. **cmd-db locator** `0xc3f000c` comes from downstream SDM845. The fixed `0x85fe0000` fallback is there if it's empty.
4. **The GENI I²C firmware on SE4** is assumed to be loaded by the boot firmware, as Linux assumes. The driver checks and stops cleanly if it isn't.

## Not in this port (same as sweet)

There is no GPU on the base `fajita` branch (CPU rendering at ~20–30 fps; the `fajita-gpu` branch adds the A630 bring-up described above), and no Wi-Fi, Bluetooth, cellular, audio, cameras, sensors or Face ID. Security is mostly switched off, so don't put anything personal on it.
