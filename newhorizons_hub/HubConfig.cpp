#include "HubConfig.h"

namespace nhos {

void HubConfig::load(Storage& storage) {
  data_.gatewayId = storage.getString("hub_gateway_id", "");
  UplinkTargetMode mode = UplinkTargetMode::Production;
  parseTargetMode(storage.getString("hub_target_mode", "production"), mode);
  data_.targetMode = mode;
  data_.manualUrl = storage.getString("hub_manual_url", "");
  data_.authToken = storage.getString("hub_auth_token", "");
}

bool HubConfig::apply(Storage& storage, const String& gatewayId, const String& targetModeStr,
                       const String& manualUrl, const String& authToken) {
  if (!validGatewayId(gatewayId)) {
    return false;
  }
  UplinkTargetMode mode = UplinkTargetMode::Production;
  if (!parseTargetMode(targetModeStr, mode)) {
    return false;
  }
  if (mode == UplinkTargetMode::Manual && manualUrl.isEmpty()) {
    return false;
  }

  storage.putString("hub_gateway_id", gatewayId);
  storage.putString("hub_target_mode", targetModeToString(mode));
  storage.putString("hub_manual_url", manualUrl);
  storage.putString("hub_auth_token", authToken);

  data_.gatewayId = gatewayId;
  data_.targetMode = mode;
  data_.manualUrl = manualUrl;
  data_.authToken = authToken;
  return true;
}

bool HubConfig::validGatewayId(const String& id) {
  const size_t len = id.length();
  if (len < 1 || len > 64) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    const char c = id.charAt(i);
    const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (!alnum && c != '.' && c != '_' && c != '-') {
      return false;
    }
  }
  return true;
}

bool HubConfig::parseTargetMode(const String& value, UplinkTargetMode& out) {
  if (value == "production") {
    out = UplinkTargetMode::Production;
    return true;
  }
  if (value == "local") {
    out = UplinkTargetMode::Local;
    return true;
  }
  if (value == "manual") {
    out = UplinkTargetMode::Manual;
    return true;
  }
  return false;
}

String HubConfig::targetModeToString(UplinkTargetMode mode) {
  switch (mode) {
    case UplinkTargetMode::Local:
      return "local";
    case UplinkTargetMode::Manual:
      return "manual";
    case UplinkTargetMode::Production:
    default:
      return "production";
  }
}

}  // namespace nhos
