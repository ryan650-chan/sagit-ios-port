# sagit-ios-port

[中文](README.md) | **English**

A research port that runs the iPadOS 18.7.10 userland on a Xiaomi Mi 6 (sagit / Snapdragon 835) by booting XNU on bare metal.

> This is a research project, not a daily-driver system. It only ever runs in memory via
> `fastboot boot` and **writes to no partition** — a power cycle puts you back on Android.
> Security mechanisms (AMFI, sandbox, data protection) are effectively disabled,
> so **do not store any private data on it**.

---

## Status (2026-10-30)

| Stage | State |
|---|---|
| XNU boot | ✅ 16K pages, 6× A55, GICv3, UART + framebuffer console |
| Storage | ✅ self-built APFS data volume (mounted at `/private/var`, read-write) + UFS driver |
| Userland bring-up | ✅ launchd → daemons |
| Display | ✅ 1080×1920 DSI panel, software CoreAnimation, **frame submission confirmed** |
| SpringBoard | ✅ runs and draws the lock screen |
| Touch | ✅ Synaptics S3330 / RMI4 F12, ten contacts, physical taps and swipes work |
| Unlock | ❌ stuck at "Press Home to open"; the body has no Home key, workaround in progress |
| Wi-Fi / Bluetooth / cellular / audio / camera / GPU | ❌ not implemented |

It stops at the lock screen. The screen lights up, submits frames and responds to touch, but it never reaches the home screen.

---

## How it boots

```
fastboot boot  →  XnuBridge.efi (UEFI)  →  XNU  →  launchd  →  SpringBoard
```

The parts that matter:

1. **Device tree forgery.** A hand-written, iPhone-like device tree is handed to XNU so the kernel believes it is booting on Apple hardware.
2. **Data volume.** Space is carved out of the tail of `userdata` and formatted as an APFS container mounted at `/private/var`. The image is produced by `scripts/build-apfs.py`; the volume UUID is pinned in `SAGIT_DATA_UUID`.
3. **System volume.** No partition is flashed. `imageboot-attach` attaches `SystemOS.dmg` (taken from an official IPSW) as a cryptex at `/System/Cryptexes`.
4. **Cryptex framework overlay.** iOS 18 moved a large number of frameworks into the cryptex volume. After boot the kernel uses bindfs to union `/System/Cryptexes/usr/lib`, `.../Frameworks` and `.../PrivateFrameworks` on top of the matching paths on the system volume. **Skip this and `dlopen` fails in bulk: `pkd` cannot find `PlugInKitDaemon.framework`, crash-loops every 10 seconds, and SpringBoard never submits a frame.**
5. **AppleKeyStore stub.** `AppleKeyStore` is closed-source and SEP-backed, so this project ships its own implementation (no passcode, system keybag always loaded, device always reported as unlocked). Without it, all of `libaks` fails.
6. **Kernel log.** `sagit-log=1` makes the kernel stream its console ring buffer into the last 32 MiB of `userdata`; the log is then read back with `dd` from TWRP, because this device has no usable serial port.

---

## Layout

```
drivers/            kexts we wrote (display, input, storage, bus, power, security, …)
boot/bridge/        sources for XnuBridge.efi: UEFI boot, device tree generation, cryptex handoff
patches/xnu/        patches against XNU + series (application order)
patches/<other>/    grouped by target: llvm, go-apfs, edk2, linux, apfsprogs
scripts/            build and packaging tools (APFS image, kernelcache packaging, bridge, boot image)
tools/              display / touch / storage handoff configs, log puller
device_trees/       device tree sources
docs/               per-subsystem investigation and validation records (touch, display, UFS, power, GPU, …)
AGENTS.md           working agreements: red lines, build traps, where changes belong, how to collect evidence
```

Patch order is listed line by line in `patches/xnu/series` (sorted by filename; entries starting with `z` must come last). Targets outside `patches/xnu/` follow the same rule within their own group.

## Prerequisites

The repository contains **no Apple binaries**. You supply:

- **An XNU source tree.** Apply the patches listed in `patches/xnu/series` in that order to the matching XNU version.
- **iPadOS 18.7.10 firmware (22H374, iPad7,11 / iPad7,12).** Download the IPSW from Apple's CDN and extract `kernelcache.release.ipad7c`, `SystemOS.dmg`, `Cryptex1,AppOS` and the root filesystem into `build/apfs/` (exact paths are in `scripts/build-port.sh`).
- **A host build environment.** Linux with an LLVM/Clang toolchain, plus Python 3 for the `apfs` helper scripts.

## Build

```sh
./scripts/build-port.sh
```

Four steps: compile XNU → package the kernelcache (including this repo's kexts) → AMFI processing → produce `XnuBridge.efi` and `build/boot/boot-sagit.img`.

Note that **only the first step actually compiles C sources**; the rest merely repackage. The build script greps the compile log for `error:` and compares the kernel hash, so a stale kernel cannot be shipped silently.

## Run

```sh
./scripts/run-fastboot.sh              # boot only
./scripts/run-fastboot.sh --pull-log   # boot → wait for self-reset → pull the log from TWRP
```

The boot argument `sagit-reboot-after=<seconds>` makes the kernel reboot into recovery on its own after the given delay, which is convenient while iterating. It is off by default.

## Reading the log

There is no serial console, so the log takes a different route:

1. The kernel writes its msgbuf ring buffer into a fixed window in the last 32 MiB of `userdata`.
2. From TWRP, `tools/sagit-log-pull.py` reads that window with `dd` and decodes it.

```sh
python3 tools/sagit-log-pull.py --adb adb --tail 3
```

The window can carry more than logs: `sagit_dump_file()` copies an arbitrary file to 8 MiB above the window, which is how binaries from the read-only system volume get exfiltrated for analysis (`usermanagerd` was reverse-engineered this way).

## Known issues / next steps

- **Touch (fixed, kept for the record).** `SynapticsTouch` had never started: the manifest carried the sensor but not `GenericI2C` (its provider), `QcomI2C` (the QUP5 controller), `QcomGCC` + `GenericClockController` (BLSP1 bus clocks) or `QcomGPIO` (the GPIO125 interrupt). Two more gates followed: the driver matched before the I2C controller probed, so the first query always failed (it now starts anyway and retries once a second), and the default handoff mode assumes firmware released the touch reset and kept its rail powered, while EDK2 touches neither GPIO89 nor GPIO50 (the EFI preflight now settles pins, rail and reset; the GCC clocks stay with the kernel). Physical taps and swipes work.
- **Unlock.** The body has no Home key, so the lock screen waits on "Press Home to open". `SBDontLockEver` only prevents later locking, not the first unlock after boot. Volume up plus volume down is currently mapped to AC Home and is being verified against SpringBoard; if it is ignored, the alternatives are `FingerprintHID` (front fingerprint sensor as a Home button) or intervening in SpringBoard's lock state directly.
- **`watchdogd`** is still crash-looping (SIGTRAP); it does not affect frame submission.
- **Resolution trade-off.** We are 16:9 (1080×1920). Switching to an iPhone X-class, all-screen device identity would make iOS lay out for 1125×2436, cutting off the bottom 516 pixels (including the home indicator), so the current build keeps a Home-button device identity.

## License and disclaimer

- This repository contains only code and documentation written for this project. Apple's XNU sources are released under the APSL, and the kext binaries come from your own IPSW; neither is redistributed here.
- Original license headers are preserved at the top of each source file (GPL-2.0 / BSD-2-Clause / APSL).
- Not affiliated with or endorsed by Apple. Provided for kernel and driver porting research only; do not use it anywhere security matters.

## A note on repository layout

This is a standalone research repository and does **not** follow the UniXNU device-repository layout (no `board.toml` / `porting.toml` / `feeds.toml`, and the drivers are not split into separate component repositories). The patch directory and the conventions in `AGENTS.md` follow their habits so a future migration stays cheap; at this stage getting things to run matters more than matching a directory tree.
