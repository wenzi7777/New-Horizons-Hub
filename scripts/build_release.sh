#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SKETCH="${ROOT}/newhorizons_hub"
OUT_DIR="${ROOT}/build"
BUILD_PATH="${OUT_DIR}/compile"
RELEASE_DIR="${ROOT}/releases/artifacts"
MANIFEST_DIR="${ROOT}/releases"
FQBN="${FQBN:-esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=min_spiffs}"
VERSION="${VERSION:-v0.9.0}"
# stable (default) or beta; beta versions carry a suffix, e.g. v0.9.0-beta.1.
CHANNEL="${CHANNEL:-stable}"
# Must match Config.h's kOtaManifestModel (the pre-rename VD-CTL/R name)
# exactly: Hub firmware older than the TIA-CTL rename only accepts that.
MODEL="${MODEL:-VD-CTL/R v2.3.D GCU LTS (Hub)}"
BASE_URL="${BASE_URL:-https://raw.githubusercontent.com/wenzi7777/New-Horizons-Hub/main/releases/artifacts}"
# Requires releases/notes/${VERSION}.md to exist -- see releases/README.md.
CHANGELOG_URL="${CHANGELOG_URL:-https://raw.githubusercontent.com/wenzi7777/New-Horizons-Hub/main/releases/notes/${VERSION}.md}"

mkdir -p "${OUT_DIR}" "${BUILD_PATH}" "${RELEASE_DIR}" "${MANIFEST_DIR}"

if [[ ! -f "${ROOT}/releases/notes/${VERSION}.md" ]]; then
  echo "Missing releases/notes/${VERSION}.md -- write a release note before cutting a release." >&2
  exit 1
fi

arduino-cli compile \
  --fqbn "${FQBN}" \
  --build-path "${BUILD_PATH}" \
  --build-property "compiler.cpp.extra_flags=-DNHOS_BOARD_GCU_V23D_LTS" \
  "${SKETCH}" \
  --output-dir "${OUT_DIR}"

main_bin="$(find "${OUT_DIR}" -maxdepth 1 -name '*.bin' ! -name '*bootloader*' ! -name '*partitions*' ! -name '*.merged.bin' -print -quit)"
if [[ -z "${main_bin}" ]]; then
  echo "No firmware .bin emitted under ${OUT_DIR}" >&2
  exit 1
fi

target="${RELEASE_DIR}/newhorizons-hub-gcu-v23d-lts-${VERSION}.bin"
cp "${main_bin}" "${target}"
echo "${target}"

# Which manifests this lands in depends on CHANNEL (stable/beta), see
# publish_track() in generate_arduino_manifest.py: stable writes
# hub-gcu-v23d-lts-latest.json (and -beta.json unless a newer beta is
# there), beta writes only hub-gcu-v23d-lts-beta.json.
python3 "${ROOT}/scripts/generate_arduino_manifest.py" \
  --firmware "${target}" \
  --track hub-gcu-v23d-lts \
  --manifest-dir "${MANIFEST_DIR}" \
  --channel "${CHANNEL}" \
  --model "${MODEL}" \
  --version "${VERSION}" \
  --base-url "${BASE_URL}" \
  --changelog-url "${CHANGELOG_URL}"
