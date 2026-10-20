#!/usr/bin/env bash
# Configures, builds and runs the omthx tests on Linux. Same job as scripts\test.ps1 on Windows.
#   bash scripts/test.sh [--debug] [doctest-filter]
set -euo pipefail
preset=linux-release
build_args=()
if [[ ${1:-} == --debug ]]; then
  preset=linux-debug
  build_args+=(--debug)
  shift
fi
filter=${1:-}
root=$(cd "$(dirname "$0")/.." && pwd)
bash "$root/scripts/build.sh" "${build_args[@]}"
if [[ -n $filter ]]; then
  "$root/build/$preset/omthx_tests" "--test-case=$filter"
else
  "$root/build/$preset/omthx_tests"
fi