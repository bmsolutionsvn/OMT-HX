#!/usr/bin/env bash
# Configures and builds OMT-HX on Linux. With --shared it also builds libomthx.so, which the OBS plugin loads.
#   bash scripts/build.sh [--debug] [--shared]
set -euo pipefail
preset=linux-release
shared=OFF
for arg in "$@"; do
  case $arg in
    --debug) preset=linux-debug ;;
    --shared) shared=ON ;;
    *) echo "usage: $0 [--debug] [--shared]" >&2; exit 2 ;;
  esac
done
root=$(cd "$(dirname "$0")/.." && pwd)
if git -C "$root" rev-parse --git-dir >/dev/null 2>&1; then
  git -C "$root" submodule update --init
fi
cd "$root"
cmake --preset "$preset" "-DOMTHX_SHARED=$shared"
cmake --build --preset "$preset"