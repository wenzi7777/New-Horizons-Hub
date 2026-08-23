#!/usr/bin/env bash
# Builds and runs the Hub relay contract test without Arduino hardware/core.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HUB_DIR="$SCRIPT_DIR/../newhorizons_hub"
OUTPUT="$(mktemp "${TMPDIR:-/tmp}/nhos-hub-v5-relay.XXXXXX")"
trap 'rm -f "$OUTPUT"' EXIT

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  "$SCRIPT_DIR/test_v5_relay_contract.cpp" \
  "$HUB_DIR/EspNowFrame.cpp" \
  -o "$OUTPUT"

"$OUTPUT"
