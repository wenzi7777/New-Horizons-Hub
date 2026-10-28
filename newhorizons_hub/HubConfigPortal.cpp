#include "HubConfigPortal.h"

#include <WiFi.h>
#include <esp_mac.h>
#include <esp_random.h>

#include "Config.h"
#include "JsonUtils.h"

namespace nhos {

namespace {
// Same as HubUplinkClient.cpp's production target; only used to check the
// Backend answers before the Hub reboots into normal operation.
constexpr char kProductionHost[] = "isensing-s1.u-aizu.ac.jp";
constexpr uint16_t kProductionPort = 443;
constexpr uint32_t kProbeTimeoutMs = 2000;
}  // namespace

void HubConfigPortal::begin(Storage* storage, HubConfig* hubConfig) {
  storage_ = storage;
  hubConfig_ = hubConfig;

  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char ssid[48];
  snprintf(ssid, sizeof(ssid), "%s-%02X%02X%02X%02X%02X%02X", kDefaultApSsidPrefix, mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);

  Serial.println(F("[hub_config_portal] starting"));
  SetupPortalIdentity identity;
  identity.product = "hub";
  identity.variant = "full";
  identity.board = kHardwareModel;
  identity.version = kFirmwareVersion;
  portal_.begin(ssid, kSetupPortalDomain, identity, hooks());
  Serial.print(F("[hub_config_portal] started ssid="));
  Serial.print(ssid);
  Serial.print(F(" portal=http://"));
  Serial.println(kSetupPortalDomain);
}

void HubConfigPortal::service() {
  portal_.service();
}

SetupPortalHooks HubConfigPortal::hooks() {
  SetupPortalHooks hooks;
  hooks.info = [this](String& out) {
    bool first = false;
    jsonStringField(out, "saved_ssid", storage_->getString("wifi_ssid", ""), first);
    jsonStringField(out, "hub_id", storage_->getString("hub_gateway_id", ""), first);
    jsonStringField(out, "target_mode", storage_->getString("hub_target_mode", "production"), first);
    jsonStringField(out, "manual_url", storage_->getString("hub_manual_url", ""), first);
    // Never the token itself: anyone can join this open AP.
    jsonBoolField(out, "token_saved", !storage_->getString("hub_auth_token", "").isEmpty(), first);
  };
  hooks.save = [this](WebServer& server, String& errors, String& ssid, String& password) {
    return save(server, errors, ssid, password);
  };
  hooks.probe = [this]() { return probeBackend(); };
  hooks.onConnected = [this](const String& ssid, const String& password) { commit(ssid, password); };
  hooks.factoryReset = [this]() { factoryReset(); };
  return hooks;
}

SetupPortalSave HubConfigPortal::save(WebServer& server, String& errors, String& ssid, String& password) {
  ssid = server.arg("ssid");
  ssid.trim();
  password = server.arg("password");
  String targetMode = server.arg("target_mode");
  String manualUrl = server.arg("manual_url");
  manualUrl.trim();
  String authToken = server.arg("auth_token");
  authToken.trim();

  auto addError = [&errors](const char* field, const char* code) {
    if (!errors.isEmpty()) errors += ',';
    errors += '"';
    errors += field;
    errors += "\":\"";
    errors += code;
    errors += '"';
  };
  if (ssid.isEmpty()) addError("ssid", "required");
  UplinkTargetMode parsedMode;
  if (!HubConfig::parseTargetMode(targetMode, parsedMode)) {
    addError("form", "other_fail");
  } else if (parsedMode == UplinkTargetMode::Manual) {
    if (manualUrl.isEmpty()) {
      addError("url", "required");
    } else if (!manualUrl.startsWith("ws://") && !manualUrl.startsWith("wss://")) {
      addError("url", "bad_url");
    }
  }
  if (hubConfig_ == nullptr) addError("form", "other_fail");
  if (!errors.isEmpty()) {
    return SetupPortalSave::Invalid;
  }

  // Blank fields keep what is saved: the page never shows the password or
  // the token, so blank is the only way to say "unchanged".
  if (password.isEmpty() && ssid == storage_->getString("wifi_ssid", "")) {
    password = storage_->getString("wifi_pass", "");
  }
  if (authToken.isEmpty()) {
    authToken = storage_->getString("hub_auth_token", "");
  }
  pendingTargetMode_ = targetMode;
  pendingManualUrl_ = manualUrl;
  pendingAuthToken_ = authToken;
  return SetupPortalSave::TestWifi;
}

// A plain TCP connect, not the WebSocket/TLS handshake: enough to tell
// "server reachable" from "wrong URL or no route", without the ~40 KB a TLS
// session needs. Local mode means this machine's own 127.0.0.1, which a Hub
// can't test, so it is skipped.
const char* HubConfigPortal::probeBackend() {
  String host;
  uint16_t port = 0;
  if (pendingTargetMode_ == "production") {
    host = kProductionHost;
    port = kProductionPort;
  } else if (pendingTargetMode_ == "manual") {
    const bool tls = pendingManualUrl_.startsWith("wss://");
    String rest = pendingManualUrl_.substring(tls ? 6 : 5);
    const int slash = rest.indexOf('/');
    if (slash >= 0) rest = rest.substring(0, slash);
    const int colon = rest.indexOf(':');
    host = colon >= 0 ? rest.substring(0, colon) : rest;
    port = colon >= 0 ? static_cast<uint16_t>(rest.substring(colon + 1).toInt()) : (tls ? 443 : 80);
  } else {
    return nullptr;
  }
  WiFiClient client;
  const bool reachable = client.connect(host.c_str(), port, kProbeTimeoutMs);
  client.stop();
  Serial.printf("[hub_config_portal] probe %s:%u %s\n", host.c_str(), port, reachable ? "ok" : "failed");
  return reachable ? nullptr : "backend_unreachable";
}

void HubConfigPortal::commit(const String& ssid, const String& password) {
  String gatewayId = storage_->getString("hub_gateway_id", "");
  if (!HubConfig::validGatewayId(gatewayId)) {
    gatewayId = generateGatewayId();
  }
  if (!hubConfig_->apply(*storage_, gatewayId, pendingTargetMode_, pendingManualUrl_, pendingAuthToken_)) {
    Serial.println(F("[hub_config_portal] could not save hub settings"));
  }
  // Same keys WifiManager reads, so this portal doesn't need a WifiManager
  // instance of its own just to persist two strings.
  storage_->putString("wifi_ssid", ssid);
  storage_->putString("wifi_pass", password);
}

String HubConfigPortal::generateGatewayId() {
  // Mirrors the Desktop frontend's own Gateway-ID suggestion pattern
  // ('nh-hub-'+Math.random().toString(16).slice(2,8)). `nh-hub-` (not the
  // Gateway software's `nh-gateway-`) keeps the two client types apart in
  // the Desktop UI's gateway_id column.
  char buf[16];
  snprintf(buf, sizeof(buf), "nh-hub-%06x", static_cast<unsigned>(esp_random() & 0xFFFFFFu));
  return String(buf);
}

// Keep in step with newhorizons_hub.ino's performFactoryReset().
void HubConfigPortal::factoryReset() {
  storage_->putString("wifi_ssid", "");
  storage_->putString("wifi_pass", "");
  storage_->putString("hub_gateway_id", "");
  storage_->putString("hub_target_mode", "");
  storage_->putString("hub_manual_url", "");
  storage_->putString("hub_auth_token", "");
  if (hubConfig_ != nullptr) {
    hubConfig_->setUpdateChannel(*storage_, kUpdateChannelStable);
  }
}

}  // namespace nhos
