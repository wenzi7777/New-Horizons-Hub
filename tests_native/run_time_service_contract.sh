#!/usr/bin/env bash
# Builds and runs the Hub's ESP-NOW time service contract test against
# nhos_core (the code shared with the device firmware), and checks the Hub
# keeps no copy of a file that library provides -- a stale copy beside the
# sketch would shadow the library's.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HUB_DIR="$SCRIPT_DIR/../newhorizons_hub"
DEVICE_DIR="$SCRIPT_DIR/../../NewHorizonsOS-OTA/firmware/newhorizons_os"
CORE_DIR="$SCRIPT_DIR/../../NewHorizonsOS-OTA/firmware/libraries/nhos_core/src"
OUTPUT="$(mktemp "${TMPDIR:-/tmp}/nhos-hub-time.XXXXXX")"
trap 'rm -f "$OUTPUT"' EXIT

if [ ! -d "$CORE_DIR" ]; then
  echo "nhos_core not found at $CORE_DIR: check out NewHorizonsOS-OTA beside this repo" >&2
  exit 1
fi

for path in "$CORE_DIR"/*; do
  f="$(basename "$path")"
  if [ -e "$HUB_DIR/$f" ]; then
    echo "newhorizons_hub/$f shadows nhos_core's copy; delete it" >&2
    exit 1
  fi
done

# Still a byte-identical copy rather than library code: it includes the
# product's own Storage.h (README, "Shared code").
if ! cmp -s "$HUB_DIR/OtaManager.h" "$DEVICE_DIR/OtaManager.h"; then
  echo "OtaManager.h out of sync with NewHorizonsOS-OTA" >&2
  exit 1
fi

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -I"$CORE_DIR" \
  "$SCRIPT_DIR/test_time_service_contract.cpp" \
  "$CORE_DIR/ClockCore.cpp" \
  "$CORE_DIR/EspNowTimeWire.cpp" \
  "$CORE_DIR/EspNowFrame.cpp" \
  -o "$OUTPUT"

"$OUTPUT"
