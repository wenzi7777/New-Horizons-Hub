#include "WifiManager.h"

#include "Config.h"
#include "JsonUtils.h"

#include <esp_mac.h>

namespace nhos {

bool WifiManager::begin(Storage& storage, bool forceSetupPortal) {
  storage_ = &storage;
  suspended_ = false;
  if (forceSetupPortal) {
    Serial.println(F("wifi_setup_requested_by_action_button"));
    startSetupAp();
    return false;
  }
  if (!hasCredentials()) {
    Serial.println(F("wifi_sta_no_credentials"));
    startSetupAp();
    return false;
  }
  if (connectStored()) {
    setupActive_ = false;
    Serial.print(F("wifi_sta_connected ip="));
    Serial.println(WiFi.localIP());
    return true;
  }
  startSetupAp();
  return false;
}

void WifiManager::service() {
  if (suspended_) {
    return;
  }
  if (setupActive_) {
    serviceSetupPortal();
    return;
  }
  if (isConnected() || millis() - lastReconnectMs_ < kWifiReconnectMs) {
    return;
  }
  lastReconnectMs_ = millis();
  connectStored();
}

void WifiManager::suspend() {
  stopSetupPortal();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  suspended_ = true;
}

void WifiManager::resume() {
  if (!suspended_ || !storage_) {
    return;
  }
  suspended_ = false;
  if (!hasCredentials()) {
    startSetupAp();
    return;
  }
  if (!connectStored()) {
    startSetupAp();
  }
}

bool WifiManager::isConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

bool WifiManager::setupActive() const {
  return setupActive_;
}

bool WifiManager::hasCredentials() const {
  return storage_ && !storage_->getString("wifi_ssid", "").isEmpty();
}

bool WifiManager::applyCredentials(const String& ssid, const String& password) {
  if (!storage_ || ssid.isEmpty()) {
    return false;
  }
  storage_->putString("wifi_ssid", ssid);
  storage_->putString("wifi_pass", password);
  stopSetupPortal();
  WiFi.mode(WIFI_STA);
  bool connected = connectStored();
  if (!connected) {
    Serial.println(F("wifi_sta_connect_failed restarting_setup_ap"));
    startSetupAp();
  } else {
    Serial.print(F("wifi_sta_connected ip="));
    Serial.println(WiFi.localIP());
  }
  return connected;
}

void WifiManager::clearCredentials() {
  if (!storage_) {
    return;
  }
  storage_->putString("wifi_ssid", "");
  storage_->putString("wifi_pass", "");
  startSetupAp();
}

String WifiManager::statusJson() const {
  String out = "{";
  out += "\"connected\":";
  out += isConnected() ? "true" : "false";
  out += ",\"setup_active\":";
  out += setupActive_ ? "true" : "false";
  out += ",\"suspended\":";
  out += suspended_ ? "true" : "false";
  out += ",\"rssi\":";
  out += isConnected() ? WiFi.RSSI() : 0;
  out += ",\"ip\":\"";
  out += isConnected() ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  out += "\"}";
  return out;
}

void WifiManager::macBytes(uint8_t out[6]) const {
  esp_read_mac(out, ESP_MAC_WIFI_STA);
}

bool WifiManager::connectStored() {
  if (!storage_) {
    return false;
  }
  if (!hasCredentials()) {
    Serial.println(F("wifi_sta_no_credentials"));
    return false;
  }
  const String ssid = storage_->getString("wifi_ssid", "");
  const String password = storage_->getString("wifi_pass", "");
  WiFi.mode(WIFI_STA);
  Serial.print(F("wifi_sta_connect_start ssid="));
  Serial.println(ssid);
  WiFi.begin(ssid.c_str(), password.c_str());
  const uint32_t start = millis();
  while (millis() - start < 8000) {
    if (isConnected()) {
      return true;
    }
    delay(50);
  }
  return false;
}

void WifiManager::startSetupAp() {
  stopSetupPortal();
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char ssid[48];
  snprintf(
      ssid,
      sizeof(ssid),
      "%s-%02X%02X%02X%02X%02X%02X",
      kDefaultApSsidPrefix,
      mac[0],
      mac[1],
      mac[2],
      mac[3],
      mac[4],
      mac[5]);
  Serial.println(F("wifi_setup_ap_starting"));
  // The saved Wi-Fi failed but the Hub is otherwise configured: offer only
  // the Wi-Fi half of the setup page (variant wifi_only).
  SetupPortalIdentity identity;
  identity.product = "hub";
  identity.variant = "wifi_only";
  identity.board = kHardwareModel;
  identity.version = kFirmwareVersion;
  portal_.begin(ssid, kSetupPortalDomain, identity, portalHooks());
  portalStarted_ = true;
  setupActive_ = true;
  Serial.print(F("wifi_setup_ap_started ssid="));
  Serial.print(ssid);
  Serial.print(F(" ip="));
  Serial.print(WiFi.softAPIP());
  Serial.print(F(" portal=http://"));
  Serial.println(kSetupPortalDomain);
}

void WifiManager::stopSetupPortal() {
  const bool apWasActive = portalStarted_ || setupActive_;
  if (portalStarted_) {
    portal_.stop();
    portalStarted_ = false;
  }
  setupActive_ = false;
  if (apWasActive) {
    WiFi.softAPdisconnect(true);
  }
}

void WifiManager::serviceSetupPortal() {
  if (!portalStarted_) {
    return;
  }
  portal_.service();
}

SetupPortalHooks WifiManager::portalHooks() {
  SetupPortalHooks hooks;
  hooks.info = [this](String& out) {
    bool first = false;
    jsonStringField(out, "saved_ssid", storage_ ? storage_->getString("wifi_ssid", "") : String(), first);
    jsonStringField(out, "hub_id", storage_ ? storage_->getString("hub_gateway_id", "") : String(), first);
  };
  hooks.save = [this](WebServer& server, String& errors, String& ssid, String& password) {
    ssid = server.arg("ssid");
    ssid.trim();
    password = server.arg("password");
    if (ssid.isEmpty()) {
      errors = "\"ssid\":\"required\"";
      return SetupPortalSave::Invalid;
    }
    // Same network, password left blank: keep the saved one.
    if (password.isEmpty() && storage_ && ssid == storage_->getString("wifi_ssid", "")) {
      password = storage_->getString("wifi_pass", "");
    }
    return SetupPortalSave::TestWifi;
  };
  hooks.onConnected = [this](const String& ssid, const String& password) {
    if (storage_) {
      storage_->putString("wifi_ssid", ssid);
      storage_->putString("wifi_pass", password);
    }
  };
  return hooks;
}

}  // namespace nhos
