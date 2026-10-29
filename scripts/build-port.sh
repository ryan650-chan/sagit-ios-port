#!/bin/bash
# Full build: XNU -> kernelcache (with our kexts) -> AMFI step -> XnuBridge.efi
# -> boot image. Run from the repository root.
#
# Prerequisites (not in this repo, see README "准备"): the XNU source tree this
# patch stack applies to, the host toolchain, the unpacked iPadOS 18.7.10
# (22H374) components under build/apfs, and the four trust-cache / APFS report
# files under build/apfs.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

IMAGE=build/boot/boot-sagit.img
BRIDGE=build/bridge-sagit

echo "===== 0. kernel (XNU) ====="
# Only this step compiles the C sources; pack-drivers just repackages
# build/output/mach.development.*, so a silent failure here ships an old kernel.
BEFORE=$(sha256sum build/output/mach.development.sdm835.unstripped 2>/dev/null | cut -c1-16)
make build JOBS=3 >/dev/null 2>&1
if grep -q "error:" build/kernel-sdm835.log; then
	echo "KERNEL BUILD FAILED"
	grep "error:" build/kernel-sdm835.log | head -5
	exit 1
fi
AFTER=$(sha256sum build/output/mach.development.sdm835.unstripped | cut -c1-16)
echo "kernel $BEFORE -> $AFTER"
[ "$BEFORE" = "$AFTER" ] && echo "WARNING: kernel unchanged, nothing was recompiled"

echo
echo "===== 1. kernelcache ====="
make kernelcache COLLECTION=apfs KEXT_SET=docs/sdm835/ios18/stages/08-full JOBS=3 2>&1 | tail -3
ls build/output/Extensions | wc -l

echo
echo "===== 2. AMFI ====="
python3 - build/output/kernelcache.sdm835.apfs build/output/kernelcache.sdm835.apfs.amfi <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, 'scripts')
from bridge_amfi import prepare_no_apple_pmgr
print('amfi:', prepare_no_apple_pmgr(Path(sys.argv[1]), Path(sys.argv[2]))['output_sha256'])
PY

# Boot arguments the userland currently needs. See docs/ for what each one buys.
# Note: sagit-reboot-after=N adds a self reset back to recovery (useful while
# iterating, off by default). sagit-log* park the kernel console in the last
# 32 MiB of userdata so TWRP can dd it out.
ARGS="io=0x3000 vm-pagesize=16384 msgbuf=1048576 panic_on_crash_disabled=1 userspace_panic_disabled=1 ufs-force-writable=1 sagit-datavar=1 sagit-trace-exit=1 sagit-log=1 sagit-log-period=2 amfi=0xff cs_enforcement_disable=1 -v debug=0x14a imageboot-attach=file:///usr/standalone/firmware/Cryptex1/SystemOS.dmg imageboot-mountpoint=/System/Cryptexes imageboot-fstype=apfs imageboot-volume-uuid=4A77BA00-2826-44C4-B883-1FA92AC36880 imageboot-probe=/System/Cryptexes/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64 mirror-console=1"

echo
echo "===== 3. bridge (UEFI loader) ====="
rm -rf "$BRIDGE"
python3 scripts/build-bridge.py \
  --kernel build/output/kernelcache.sdm835.apfs.amfi \
  --apfs-report build/apfs/ios.apfs.group.json \
  --firmware-report build/apfs/prepared-firmware.json \
  --development-no-sep --development-unsealed-root \
  --development-watchdog-policy \
  --full-memory \
  --ufs-handoff \
  --sagit-ramvar-image build/apfs/ramvar/var.apfs \
  --display-handoff-profile tools/display-handoff.json \
  --touch-handoff-profile tools/touch-handoff.json \
  --trust-cache build/apfs/cryptex-os-trustcache.bin \
  --trust-cache build/apfs/cryptex-app-trustcache.bin \
  --extra-boot-args "$ARGS" \
  --output "$BRIDGE" 2>&1 | tail -3

echo
echo "===== 4. boot image ====="
python3 scripts/build-boot.py --bridge "$BRIDGE/XnuBridge.efi" --autoboot \
  --output "$IMAGE" 2>&1 | tail -2

python3 - "$BRIDGE" "$IMAGE" <<'PY'
import json, pathlib, sys
b = json.loads(pathlib.Path(sys.argv[1], 'build.json').read_text())
i = json.loads(pathlib.Path(sys.argv[2]).with_suffix('.json').read_text())
print('kernel:', b.get('kernel_sha256'))
print('image :', i.get('image_bytes'), i.get('image_sha256'))
PY

# The bridge directory is ~400 MB of intermediates; keep the image only.
rm -rf "$BRIDGE"
echo "done -> $IMAGE"
