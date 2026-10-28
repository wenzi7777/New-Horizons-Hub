#pragma once

// Hub setup portal: the SoftAP setup page (nhos_core SetupPortal, product
// "hub", variant "full") a Hub runs when it has no Wi-Fi saved -- first
// boot, or after a factory reset. Once this form is submitted and the Hub
// connects, no WebServer runs for the rest of normal operation (the old
// always-on DirectWebUI dashboard was removed: a second WebServer's worth of
// heap on a no-PSRAM board, duplicating what the Hub<->Backend WebSocket
// already does).
//
// Unlike a device, a Hub dials out to a specific Backend, so this form also
// collects *where* to connect (target_mode / manual_url / auth_token): there
// is no later "already connected, ask Desktop" step to defer that to.
//
// Gateway ID is deliberately NOT a field: it is generated (nh-hub-XXXXXX)
// so the form stays close to "just get online". Renaming it, or changing the
// Backend later, is done from the Desktop app's Manage Hub panel (a
// `set_config` gateway_command) once the Hub is online.
//
// When the saved Wi-Fi stops working, WifiManager opens the same page in its
// Wi-Fi-only variant instead (see WifiManager::startSetupAp()).
//
// GCU V2.3.D has no setup button (NHOS_BOARD_HAS_BUTTON=0), so getting back
// here is a factory reset: from this page, by quick power cycles (see
// newhorizons_hub.ino), or remotely with a `factory_reset` gateway_command.

#include <Arduino.h>

#include "HubConfig.h"
#include "SetupPortal.h"
#include "Storage.h"

namespace nhos {

class HubConfigPortal {
 public:
  // Starts the SoftAP + web server. Call once, when newhorizons_hub.ino
  // decides Wi-Fi credentials aren't set yet.
  void begin(Storage* storage, HubConfig* hubConfig);

  // Call every loop() iteration while active().
  void service();

  bool active() const { return portal_.active(); }

 private:
  SetupPortalHooks hooks();
  SetupPortalSave save(WebServer& server, String& errors, String& ssid, String& password);
  const char* probeBackend();
  void commit(const String& ssid, const String& password);
  void factoryReset();
  static String generateGatewayId();

  Storage* storage_ = nullptr;
  HubConfig* hubConfig_ = nullptr;
  SetupPortal portal_;
  // What save() accepted, written to NVS only once the Wi-Fi test passes.
  String pendingTargetMode_;
  String pendingManualUrl_;
  String pendingAuthToken_;
};

}  // namespace nhos
