#!/usr/bin/env bash
# Prepare and build the pinned EDK2/sagit checkout.
set -euo pipefail
task_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
task_build="$task_root/build/edk2-sagit"
mkdir -p "$task_build/host-bin" "$task_build/logs"
task_repo="$task_root/boot/edk2-msm8998"
python3 "$task_root/scripts/prepare-edk2-sagit.py"
if [[ ! -x "$task_build/venv/bin/python3" ]]; then
    uv venv --python 3.10 "$task_build/venv"
    uv pip install --python "$task_build/venv/bin/python" pillow==12.3.0 GitPython==3.2.0
fi
ln -sfn "$task_root/scripts/edk2-host-gcc.sh" "$task_build/host-bin/gcc"
ln -sfn "$task_root/scripts/edk2-host-gcc.sh" "$task_build/host-bin/cc"
ln -sfn "$task_root/scripts/edk2-target-gcc.sh" "$task_build/host-bin/aarch64-none-linux-gnu-gcc"
# Local toolchain adaptation: this host ships the AArch64 cross binutils under
# the aarch64-linux-gnu- prefix, while CROSS_COMPILE below expects
# aarch64-none-linux-gnu-. Alias every cross tool to the expected name; the
# compiler itself keeps the wrapper above (it injects -std=gnu17 for EDK2).
for tool in /usr/bin/aarch64-linux-gnu-*; do
    tool_name=${tool##*/}
    [[ "$tool_name" == aarch64-linux-gnu-gcc ]] && continue
    ln -sfn "$tool" "$task_build/host-bin/aarch64-none-linux-gnu-${tool_name#aarch64-linux-gnu-}"
done
export PATH="$task_build/host-bin:$task_build/venv/bin:$PATH"
export CROSS_COMPILE=aarch64-none-linux-gnu-
export PYTHON_COMMAND=python3
task_cpus=$(python3 -c 'import os; print(",".join(map(str, sorted(os.sched_getaffinity(0))[:3])))')
taskset -c "$task_cpus" bash "$task_root/boot/edk2-msm8998/build.sh" \
    --device sagit --release DEBUG --outputdir "$task_build" "$@" \
    2>&1 | tee "$task_build/logs/build.log"
cp "$task_repo/workspace/Build/sagit/DEBUG_GCC5/FV/MSM8998PKG_UEFI.fd" "$task_build/uefi-sagit.fd"
python3 - "$task_root" "$task_build/boot-sagit.img" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]) / 'scripts'))
from mi6_boot import assemble, parse_image
path = Path(sys.argv[2])
image = parse_image(path.read_bytes())
path.write_bytes(assemble(image, b'', stamp=True))
PY
(cd "$task_build" && sha256sum boot-sagit.img uefi-sagit.fd > SHA256SUMS)
