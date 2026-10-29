#!/usr/bin/env bash
# Local toolchain adaptation: this host ships gcc-aarch64-linux-gnu
# (binary prefix aarch64-linux-gnu-); upstream expects aarch64-none-linux-gnu-.
set -euo pipefail
exec /usr/bin/aarch64-linux-gnu-gcc -std=gnu17 "$@"
