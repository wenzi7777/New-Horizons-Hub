#pragma once

#include <Arduino.h>

#include "BoardConfig.h"
#include "PacketWireConstants.h"

namespace nhos {

static constexpr char kProductName[] = "NHOS Arduino";
static constexpr char kProtocolName[] = "NHO/Arduino/1";
// This Hub firmware and the device firmware (New-Horizons-OS repo) can
// both target the same physical board (same NHOS_BOARD_NAME), which would
// otherwise make it possible to accidentally flash one's OTA release onto
// the other -- OtaManager::parseManifest() only checks `protocol` and
// `model` against a manifest, and those two would be identical. The
// " (Hub)" suffix keeps this Config.h's copy distinguishable from the
// device firmware's own Config.h even though both started as the same
// file (see this repo's README for the manual-sync convention).
static constexpr char kHardwareModel[] = NHOS_BOARD_NAME " (Hub)";
// The pre-rename name OTA manifests still carry (see BoardConfig.h).
static constexpr char kOtaManifestModel[] = NHOS_BOARD_OTA_MODEL " (Hub)";
static constexpr char kFirmwareVersion[] = "v0.12.0";

static constexpr uint16_t kRows = NHOS_BOARD_ROWS;
static constexpr uint16_t kCols = NHOS_BOARD_COLS;
static constexpr uint16_t kMaxSensors = kRows * kCols;
static constexpr uint8_t kImuSampleFloats = 7 + (NHOS_BOARD_HAS_MAG ? 3 : 0);

// Ports and the packet header length: PacketWireConstants.h, generated from
// the device firmware's spec/wire.json. The Hub relays device packets
// opaquely (it reads only the UID at offset 4), so that is all it needs.
static constexpr uint32_t kHeartbeatIntervalMs = 5000;

static constexpr uint16_t kDefaultTargetFps = 60;
static constexpr uint16_t kMaxTargetFps = 120;
static constexpr uint16_t kDefaultSettleUs = 20;
static constexpr uint8_t kDefaultSendEveryNFrames = 1;
static constexpr uint8_t kStandardScanRingFrames = 3;
static constexpr uint8_t kExtendedScanRingFrames = 5;
static constexpr size_t kScanRingFrames = kStandardScanRingFrames;
static constexpr size_t kMaxScanRingFrames = kExtendedScanRingFrames;

static constexpr uint32_t kWifiReconnectMs = 10000;
static constexpr uint32_t kBootWifiSetupWindowMs = 3000;
static constexpr uint8_t kSafeModeBootFailures = 3;
static constexpr size_t kDefaultLogMaxBytes = 12 * 1024;
static constexpr size_t kExtendedLogMaxBytes = 24 * 1024;

// Both setup portals (HubConfigPortal and WifiManager's fallback) use
// these. They differ from the device firmware's NHOS / nhos.os on purpose:
// a site can have both broadcasting at once.
static constexpr char kDefaultApSsidPrefix[] = "NHHub";
static constexpr char kDefaultApPassword[] = "";
static constexpr char kSetupPortalDomain[] = "nhos.hub";
static constexpr uint16_t kSetupPortalPort = 80;
// A leftover fallback: every caller passes the Hub's own manifest URL
// (newhorizons_hub.ino). Points at this board's device firmware, whose
// manifest model (no " (Hub)" suffix) the Hub refuses anyway.
static constexpr char kDefaultUpdateManifestUrl[] = NHOS_BOARD_OTA_MANIFEST_BASE "-latest.json";

enum class RunMode : uint8_t {
  Normal = 0,
  Maintenance = 1,
  SafeMaintenance = 2,
};

struct RuntimeConfig {
  uint16_t targetFps = kDefaultTargetFps;
  uint16_t settleUs = kDefaultSettleUs;
  uint8_t sendEveryNFrames = kDefaultSendEveryNFrames;
  bool imuEnabled = true;
  bool indicatorsEnabled = true;
};

}  // namespace nhos
