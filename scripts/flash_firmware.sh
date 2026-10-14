#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-/dev/cu.usbserial-10}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SKETCH="${ROOT}/newhorizons_hub"
# Code shared with the device firmware: the nhos_core library in a
# NewHorizonsOS-OTA checkout beside this repo (README, "Shared code").
CORE_LIB="${CORE_LIB:-${ROOT}/../NewHorizonsOS-OTA/firmware/libraries/nhos_core}"
if [[ ! -f "${CORE_LIB}/library.properties" ]]; then
  echo "No nhos_core at ${CORE_LIB}; check out NewHorizonsOS-OTA beside this repo (or set CORE_LIB)." >&2
  exit 1
fi
BUILD_PATH="${ROOT}/build/compile"
FQBN="${FQBN:-esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=min_spiffs}"

mkdir -p "${BUILD_PATH}"

arduino-cli compile \
  --library "${CORE_LIB}" \
  --fqbn "${FQBN}" \
  --build-path "${BUILD_PATH}" \
  --build-property "compiler.cpp.extra_flags=-DNHOS_BOARD_GCU_V23D_LTS" \
  "${SKETCH}"

arduino-cli upload \
  -p "${PORT}" \
  --fqbn "${FQBN}" \
  --input-dir "${BUILD_PATH}" \
  "${SKETCH}"
