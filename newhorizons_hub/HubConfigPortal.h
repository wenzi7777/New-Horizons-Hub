#pragma once

// Hub setup portal: a SoftAP + captive-portal form that is the ONLY web
// server the Hub ever runs -- once this form is submitted and the Hub
// connects, no WebServer runs for the rest of normal operation (the
// previous always-on DirectWebUI dashboard was removed: it cost a second
// full WebServer/route table's worth of heap on a no-PSRAM board just to
// duplicate what the Hub<->Backend WebSocket connection can already do).
// This mirrors the terminal device firmware's WifiManager SoftAP portal
// exactly in spirit -- "run a web form only while offline" -- except a Hub
// dials out to a specific Backend URL rather than being passively
// discovered over broadcast, so unlike a device's Wi-Fi-only portal, this
// form also has to collect *where* to connect (target_mode/manual_url/
// auth_token) since there is no later "already connected, ask Desktop"
// step to defer that to.
//
// Gateway ID is deliberately NOT a field here -- it's auto-generated
// (nh-hub-XXXXXX, see HubConfigPortal.cpp) so this form stays as close to
// "just get online" as a WS-dialing device can get. Renaming it, or
// changing target_mode/manual_url/auth_token later, is done remotely from
// the Desktop app's Manage Hub panel (a `set_config` gateway_command sent
// over the same WS connection this portal exists to establish) once the
// Hub is online -- see HubUplinkClient::onGatewayCommand().
//
// GCU V2.3.D has no physical setup button (NHOS_BOARD_HAS_BUTTON=0), so
// re-entering this portal after first boot is done via a "Factory reset"
// link on the portal page itself (clears wifi_ssid/wifi_pass AND the
// Hub's NVS keys, then reboots) rather than a button-hold gesture, or
// remotely via a `factory_reset` gateway_command while still online.

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>

#include "HubConfig.h"
#include "Storage.h"

namespace nhos {

class HubConfigPortal {
 public:
  // Starts the SoftAP + web server. Call once, when newhorizons_hub.ino
  // decides Wi-Fi credentials aren't set yet.
  void begin(Storage* storage, HubConfig* hubConfig);

  // Call every loop() iteration while active().
  void service();

  bool active() const { return active_; }

 private:
  void configureRoutes();
  void handleRoot();
  void handleSave();
  void handleFactoryReset();
  void handleRedirect();
  String page(const String& message, bool success) const;
  String wifiNetworkOptionsHtml() const;
  String htmlEscape(const String& value) const;
  static String generateGatewayId();

  Storage* storage_ = nullptr;
  HubConfig* hubConfig_ = nullptr;
  bool active_ = false;
  bool routesConfigured_ = false;
  DNSServer dnsServer_;
  WebServer portalServer_{80};
};

}  // namespace nhos
