#!/usr/bin/env bash
# Runs the device firmware's version-order test against the Hub's own copy
# of VersionCompare.cpp, with the shared vectors in
# NewHorizonsOS-OTA/spec/fixtures/versions.json -- the Hub decides "is this
# update newer" the same way a device does, betas included.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HUB_DIR="$SCRIPT_DIR/../newhorizons_hub"
DEVICE_ROOT="$SCRIPT_DIR/../../NewHorizonsOS-OTA"
OUTPUT="$(mktemp "${TMPDIR:-/tmp}/nhos-hub-version.XXXXXX")"
trap 'rm -f "$OUTPUT"' EXIT

if [ ! -d "$DEVICE_ROOT" ]; then
  echo "NewHorizonsOS-OTA not checked out alongside; skipping version contract"
  exit 0
fi

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -I"$HUB_DIR" \
  "$DEVICE_ROOT/tests_native/test_version_compare.cpp" \
  "$HUB_DIR/VersionCompare.cpp" \
  -o "$OUTPUT"

"$OUTPUT" "$DEVICE_ROOT/spec/fixtures/versions.json"
