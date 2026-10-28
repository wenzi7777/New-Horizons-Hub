#pragma once

#include <Arduino.h>
#include <WiFi.h>

#include "Config.h"
#include "SetupPortal.h"
#include "Storage.h"

namespace nhos {

class WifiManager {
 public:
  bool begin(Storage& storage, bool forceSetupPortal = false);
  void service();
  void suspend();
  void resume();
  bool isConnected() const;
  bool setupActive() const;
  bool hasCredentials() const;
  bool applyCredentials(const String& ssid, const String& password);
  void clearCredentials();
  String statusJson() const;
  void macBytes(uint8_t out[6]) const;

 private:
  bool connectStored();
  void startSetupAp();
  void stopSetupPortal();
  void serviceSetupPortal();
  SetupPortalHooks portalHooks();

  Storage* storage_ = nullptr;
  bool setupActive_ = false;
  bool suspended_ = false;
  bool portalStarted_ = false;
  uint32_t lastReconnectMs_ = 0;
  SetupPortal portal_;
};

}  // namespace nhos
