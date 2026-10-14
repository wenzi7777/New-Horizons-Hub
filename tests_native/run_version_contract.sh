#!/usr/bin/env bash
# Runs the device firmware's version-order test against nhos_core's
# VersionCompare.cpp -- the one the Hub builds with -- with the shared vectors in
# NewHorizonsOS-OTA/spec/fixtures/versions.json -- the Hub decides "is this
# update newer" the same way a device does, betas included.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE_ROOT="$SCRIPT_DIR/../../NewHorizonsOS-OTA"
OUTPUT="$(mktemp "${TMPDIR:-/tmp}/nhos-hub-version.XXXXXX")"
trap 'rm -f "$OUTPUT"' EXIT

if [ ! -d "$DEVICE_ROOT" ]; then
  echo "NewHorizonsOS-OTA not checked out alongside; skipping version contract"
  exit 0
fi

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -I"$DEVICE_ROOT/firmware/libraries/nhos_core/src" \
  "$DEVICE_ROOT/tests_native/test_version_compare.cpp" \
  "$DEVICE_ROOT/firmware/libraries/nhos_core/src/VersionCompare.cpp" \
  -o "$OUTPUT"

"$OUTPUT" "$DEVICE_ROOT/spec/fixtures/versions.json"
