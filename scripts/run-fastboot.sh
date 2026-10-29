#!/bin/bash
# Boot the built image in RAM. Nothing is written to any partition: a power
# cycle puts the phone back to whatever it had before.
#
#   ./scripts/run-fastboot.sh            # boot
#   ./scripts/run-fastboot.sh --pull-log # boot, wait for the self reset, pull the console
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

ADB=${ADB:-adb}
FASTBOOT=${FASTBOOT:-fastboot}
IMAGE=build/boot/boot-sagit.img

[ -f "$IMAGE" ] || { echo "no $IMAGE - run scripts/build-port.sh first"; exit 1; }

echo "=== fastboot boot $IMAGE ==="
"$FASTBOOT" boot "$IMAGE" || exit 1

if [ "${1:-}" != "--pull-log" ]; then
	echo "booted. power + volume-down -> recovery to read the log window."
	exit 0
fi

echo "=== waiting for the kernel's own reset into recovery ==="
for i in $(seq 1 140); do
	sleep 3
	state=$("$ADB" get-state 2>/dev/null | tr -d '\r')
	[ "$state" = "recovery" ] && { echo "recovery up after $((i*3))s"; break; }
done
[ "${state:-}" = "recovery" ] || { echo "TIMEOUT: no recovery device"; exit 1; }

python3 tools/sagit-log-pull.py --adb "$ADB" --tail 3
