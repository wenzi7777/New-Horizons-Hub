#!/usr/bin/env bash
# Builds and runs the Hub's ESP-NOW time service contract test, and checks
# the files shared with the device firmware are still byte-identical
# (the ones that are meant to be; README "Shared files" lists the rest).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HUB_DIR="$SCRIPT_DIR/../newhorizons_hub"
DEVICE_DIR="$SCRIPT_DIR/../../NewHorizonsOS-OTA/firmware/newhorizons_os"
OUTPUT="$(mktemp "${TMPDIR:-/tmp}/nhos-hub-time.XXXXXX")"
trap 'rm -f "$OUTPUT"' EXIT

if [ -d "$DEVICE_DIR" ]; then
  for f in ClockCore.h ClockCore.cpp EspNowTimeWire.h EspNowTimeWire.cpp EspNowFrame.h EspNowFrame.cpp \
           VersionCompare.h VersionCompare.cpp UpdateChannel.h \
           JsonUtils.h JsonUtils.cpp PowerStatusJson.h PowerStatusJson.cpp OtaManager.h OtaTls.h; do
    if ! cmp -s "$HUB_DIR/$f" "$DEVICE_DIR/$f"; then
      echo "shared file out of sync with NewHorizonsOS-OTA: $f" >&2
      exit 1
    fi
  done
else
  echo "NewHorizonsOS-OTA not checked out alongside; skipping shared-file check"
fi

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -I"$HUB_DIR" \
  "$SCRIPT_DIR/test_time_service_contract.cpp" \
  "$HUB_DIR/ClockCore.cpp" \
  "$HUB_DIR/EspNowTimeWire.cpp" \
  "$HUB_DIR/EspNowFrame.cpp" \
  -o "$OUTPUT"

"$OUTPUT"
