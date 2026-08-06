#pragma once

// Hub-specific settings that used to be the hardcoded kGatewayId/kAuthToken/
// kTargetMode/kManualUrl constants in newhorizons_hub.ino (Step 1-4). Step 5
// replaces those with this NVS-backed config, filled in by
// HubConfigPortal's setup form instead of edited in source before flashing.
//
// Deliberately key-value via Storage::getString/putString (Preferences),
// not the JSON-blob-file pattern DeviceConfig.h uses -- that pattern earns
// its complexity from DeviceConfig's dozen-plus nested structs, but a Hub
// only has four scalar fields, and newhorizons_hub.ino already established
// the key-value convention here (it seeds wifi_ssid/wifi_pass the same
// way).

#include <Arduino.h>

#include "HubUplinkClient.h"
#include "Storage.h"

namespace nhos {

struct HubConfigData {
  String gatewayId;
  UplinkTargetMode targetMode = UplinkTargetMode::Production;
  String manualUrl;
  String authToken;
};

class HubConfig {
 public:
  void load(Storage& storage);

  // False until a gateway_id has been set via the portal -- this is what
  // newhorizons_hub.ino checks to decide whether to boot straight into
  // normal operation or into HubConfigPortal.
  bool isConfigured() const { return !data_.gatewayId.isEmpty(); }

  const HubConfigData& data() const { return data_; }

  // Validates and persists all four fields together (matches how the
  // portal's single form submits them together). `manualUrl` is only
  // required when targetMode == Manual, but is stored either way so a
  // user switching back to Manual later doesn't lose what they typed.
  bool apply(Storage& storage, const String& gatewayId, const String& targetModeStr,
             const String& manualUrl, const String& authToken);

  // Mirrors New-Horizons-Gateway/newhorizons_gateway/config_store.py's
  // GATEWAY_ID_PATTERN exactly (^[A-Za-z0-9._-]{1,64}$) so a Hub's
  // gateway_id is always acceptable to the same backend a Gateway talks
  // to.
  static bool validGatewayId(const String& id);
  static bool parseTargetMode(const String& value, UplinkTargetMode& out);
  static String targetModeToString(UplinkTargetMode mode);

 private:
  HubConfigData data_;
};

}  // namespace nhos
