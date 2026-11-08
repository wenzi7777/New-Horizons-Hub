#!/usr/bin/env bash
# Checks the current Hub release ships its notes in all three languages:
# releases/notes/vX.Y.Z.md (Traditional Chinese, the URL the manifest
# carries) plus .ja.md and .en.md beside it (required from v0.12.0).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HUB_ROOT="$SCRIPT_DIR/.."
VERSION="$(sed -n 's/.*kFirmwareVersion\[\] = "\(v[^"]*\)".*/\1/p' "$HUB_ROOT/newhorizons_hub/Config.h")"
if [ -z "$VERSION" ]; then
  echo "kFirmwareVersion not found in newhorizons_hub/Config.h" >&2
  exit 1
fi

status=0
for suffix in md ja.md en.md; do
  note="$HUB_ROOT/releases/notes/${VERSION}.${suffix}"
  if [ ! -f "$note" ]; then
    echo "missing release notes: releases/notes/${VERSION}.${suffix}" >&2
    status=1
  elif [ "$(head -n 1 "$note")" != "# ${VERSION}" ]; then
    echo "releases/notes/${VERSION}.${suffix} must start with '# ${VERSION}'" >&2
    status=1
  fi
done
[ "$status" -eq 0 ] && echo "release notes ${VERSION}: zh/ja/en present"
exit "$status"
