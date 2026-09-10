// New Horizons Hub -- see README.md and
// ~/.claude/plans/linked-strolling-puppy.md for design context.
//
// This Hub runs a WebServer ONLY inside HubConfigPortal's offline SoftAP
// setup form -- once that form is saved and the Hub connects, no
// WebServer runs for the rest of normal operation (the previous always-on
// DirectWebUI management page was removed: a no-PSRAM board doesn't need
// to run a second full WebServer/route table alongside HubUplinkClient's
// WS/TLS connection just to duplicate what that connection can already
// carry). All ongoing Hub management (status, settings changes, paired
// devices, LAN scan/migrate, factory reset) is driven remotely from the
// Desktop app's Manage Hub panel via `gateway_command`/`gateway_command_result`
// messages over the same WS connection -- see HubUplinkClient::onGatewayCommand()
// and handleGatewayCommand() below.

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "BoardPins.h"
#include "EspNowCommandDispatcher.h"
#include "EspNowHubManager.h"
#include "EspNowOtaRelay.h"
#include "HubConfig.h"
#include "HubConfigPortal.h"
#include "HubUplinkClient.h"
#include "JsonUtils.h"
#include "LedController.h"
#include "OtaManager.h"
#include "PowerManager.h"
#include "Storage.h"
#include "WifiManager.h"

namespace {

// This repo's own release manifest -- deliberately NOT Config.h's
// kDefaultUpdateManifestUrl (that one is a leftover default meant for
// device firmware, points at a different repo entirely). See Config.h's
// kHardwareModel comment for how OtaManager::parseManifest()'s
// model_mismatch check guards against ever applying the wrong one even if
// this URL is misconfigured.
constexpr char kHubUpdateManifestUrl[] =
    "https://raw.githubusercontent.com/wenzi7777/New-Horizons-Hub/main/releases/hub-gcu-v23d-lts-latest.json";

nhos::Storage storage;
nhos::WifiManager wifi;
nhos::LedController leds;
nhos::HubConfig hubConfig;
nhos::HubConfigPortal configPortal;
nhos::EspNowHubManager hubManager;
nhos::HubUplinkClient uplink;
nhos::EspNowCommandDispatcher commandDispatcher;
nhos::EspNowOtaRelay otaRelay;
nhos::OtaManager ota;
nhos::PowerManager power;

bool portalMode = false;
bool uplinkStarted = false;

// GCU V2.3.D has no physical setup/factory-reset button (a future board
// revision adds one) -- until then, this is the recovery path for "Wi-Fi
// credentials are wrong / Backend unreachable / user just wants back into
// setup" with no other way to reach the Hub: power the Hub off and back on
// kQuickBootThreshold times in a row, each cycle interrupted before
// kQuickBootGraceMs of stable uptime. Tracked via a persisted (NVS, not RTC
// -- RTC memory doesn't survive a true power loss, only sleep/soft-reset)
// boot counter, incremented once per boot and cleared once the Hub has
// stayed powered long enough that the current boot clearly isn't part of a
// quick-cycle sequence. Both numbers are starting guesses (fast enough to
// need deliberate action, slow enough a normal boot doesn't trip it by
// accident) -- tune from real-hardware use, not a promise.
constexpr uint8_t kQuickBootThreshold = 5;
constexpr uint32_t kQuickBootGraceMs = 5000;
constexpr char kQuickBootCountKey[] = "hub_qb_count";  // Preferences 15-char limit

uint32_t bootMs = 0;
bool quickBootCounterCleared = false;

void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  hubManager.handleEspNowRecv(info->src_addr, data, static_cast<size_t>(len));
}

uint32_t framesForwarded = 0;
uint32_t framesDroppedForCommand = 0;
uint32_t lastFrameLogMs = 0;
bool sensorForwardWasPaused = false;

void onHubFrameReady(uint8_t /*deviceIndex*/, const uint8_t* /*mac*/,
                      const uint8_t* data, size_t len, void* /*userData*/) {
  if (commandDispatcher.hasPendingCommand()) {
    // Sensor data is lossy by design (see EspNowStreamTransport.h) -- drop
    // this frame rather than let sendSensorPacket()'s blocking WSS send
    // stall loop() during the command's ack/resend window. Real-hardware
    // testing found this blocking send (up to ~55-60/sec under normal
    // streaming) was starving EspNowCommandDispatcher::service()'s resend
    // timing and EspNowHubManager's own ESP-NOW recv processing, causing
    // command_delivery_timeout well into steady-state operation even
    // though the same command succeeded reliably right after boot.
    if (!sensorForwardWasPaused) {
      sensorForwardWasPaused = true;
      Serial.println("[hub] sensor_forward_paused reason=command_pending");
    }
    ++framesDroppedForCommand;
    return;
  }
  if (sensorForwardWasPaused) {
    sensorForwardWasPaused = false;
    Serial.printf("[hub] sensor_forward_resumed frames_dropped=%lu\n",
                  static_cast<unsigned long>(framesDroppedForCommand));
  }
  uplink.sendSensorPacket(data, len);
  ++framesForwarded;
  const uint32_t now = millis();
  if (now - lastFrameLogMs >= 2000) {
    lastFrameLogMs = now;
    Serial.printf("[hub] forwarded_frames=%lu last_len=%u uplink_connected=%d\n",
                  static_cast<unsigned long>(framesForwarded),
                  static_cast<unsigned>(len), uplink.isConnected() ? 1 : 0);
  }
}

// Step 4 reverse command channel: device command responses arrive here
// (kEspNowFragTypeControl frames, kept separate from onHubFrameReady's
// sensor-data path -- see EspNowHubManager.h's onControlFrameReady()).
void onHubControlFrameReady(uint8_t deviceIndex, const uint8_t* mac, const uint8_t* data,
                             size_t len, void* /*userData*/) {
  commandDispatcher.handleControlResponse(deviceIndex, mac, data, len);
}

// Device-initiated hub-request traffic (kEspNowFragTypeHubRequest --
// fetch_manifest/ota_relay_start JSON asks, see EspNowOtaRelay.h). This
// demux is the only place that inspects `hub_req` -- add new hub_req
// values here if this channel ever carries anything besides OTA relay
// asks.
void onHubRequestFrameReady(uint8_t /*deviceIndex*/, const uint8_t* mac, const uint8_t* data,
                             size_t len, void* /*userData*/) {
  const String payload(reinterpret_cast<const char*>(data), len);
  String hubReq;
  nhos::jsonExtractString(payload, "hub_req", hubReq);
  if (hubReq == "fetch_manifest") {
    String manifestUrl;
    nhos::jsonExtractString(payload, "manifest_url", manifestUrl);
    otaRelay.handleFetchManifestRequest(mac, manifestUrl);
    return;
  }
  if (hubReq == "ota_relay_start") {
    String url;
    String sha256;
    long size = 0;
    nhos::jsonExtractString(payload, "url", url);
    nhos::jsonExtractString(payload, "sha256", sha256);
    nhos::jsonExtractInt(payload, "size", size);
    otaRelay.handleRelayStartRequest(mac, url, sha256, size > 0 ? static_cast<size_t>(size) : 0);
    return;
  }
  Serial.printf("[hub] unknown_hub_req req=%s\n", hubReq.c_str());
}

// OTA chunk ack (device -> Hub, raw 3-byte packet). See
// EspNowOtaRelay::handleChunkAck()'s comment for why this stays minimal
// here too -- it's called straight from EspNowHubManager::handleEspNowRecv(),
// which runs on the raw ESP-NOW recv callback (small stack, no I/O).
void onOtaChunkAckReceived(const uint8_t mac[6], uint16_t chunkIndex, void* /*userData*/) {
  otaRelay.handleChunkAck(mac, chunkIndex);
}

// Control-command delivery ack (device -> Hub, raw 1-byte packet). Same
// minimal-handler pattern as onOtaChunkAckReceived above -- called straight
// from the raw ESP-NOW recv callback.
void onControlAckReceived(const uint8_t mac[6], void* /*userData*/) {
  commandDispatcher.handleControlAck(mac);
}

// Backend -> Hub command, forwarded from HubUplinkClient's WS text-frame
// parsing to the dispatcher for ESP-NOW delivery + retry.
void onUplinkCommand(const String& deviceUid, const String& payloadJson, void* /*userData*/) {
  commandDispatcher.sendCommand(deviceUid, payloadJson);
}

// Clears every NVS key HubConfigPortal::handleFactoryReset() also clears --
// duplicated (not shared via a helper) because the two live in separate
// translation units and this is the entire body; see HubConfigPortal.cpp
// for the SoftAP-reachable equivalent used when the Hub can't reach the
// Backend at all.
void performFactoryReset() {
  storage.putString("wifi_ssid", "");
  storage.putString("wifi_pass", "");
  storage.putString("hub_gateway_id", "");
  storage.putString("hub_target_mode", "");
  storage.putString("hub_manual_url", "");
  storage.putString("hub_auth_token", "");
}

// Must run before anything else in setup() touches kQuickBootCountKey.
// Returns true exactly once this boot's count hits kQuickBootThreshold --
// the caller is responsible for performFactoryReset() + entering the
// portal; this function only tracks the count itself.
bool consumeQuickBootFactoryResetTrigger() {
  const uint32_t count = storage.getUInt(kQuickBootCountKey, 0) + 1;
  if (count >= kQuickBootThreshold) {
    storage.putUInt(kQuickBootCountKey, 0);
    return true;
  }
  storage.putUInt(kQuickBootCountKey, count);
  return false;
}

// Call every loop() tick (both portal and normal mode). Once the Hub has
// stayed powered for kQuickBootGraceMs without a reset, this boot no
// longer counts toward a quick-cycle sequence -- clear it so an unrelated
// future power cycle (e.g. moving the device) starts counting from zero
// rather than compounding with whatever was left over.
void serviceQuickBootCounterClear() {
  if (quickBootCounterCleared) return;
  if (millis() - bootMs < kQuickBootGraceMs) return;
  storage.putUInt(kQuickBootCountKey, 0);
  quickBootCounterCleared = true;
}

// Backend -> Hub-itself command (the WS session identifies which Hub, so
// no device_uid), forwarded from HubUplinkClient's `gateway_command`
// parsing. Only `set_config`/`factory_reset` need to reach this far --
// status/paired-devices display data rides the gateway_status heartbeat
// instead (see buildPairedDevicesDetailJson()), and LAN
// scan/migrate/cancel are served by the Backend directly or via the
// existing per-device command path (see plan section "移除 Hub 常駐
// WebUI...").
void handleGatewayCommand(const String& requestId, const String& payloadJson, void* /*userData*/) {
  String command;
  nhos::jsonExtractString(payloadJson, "command", command);

  if (command == "set_config") {
    String gatewayId;
    String targetMode;
    String manualUrl;
    String authToken;
    nhos::jsonExtractString(payloadJson, "gateway_id", gatewayId);
    nhos::jsonExtractString(payloadJson, "target_mode", targetMode);
    nhos::jsonExtractString(payloadJson, "manual_url", manualUrl);
    nhos::jsonExtractString(payloadJson, "auth_token", authToken);
    if (gatewayId.isEmpty()) gatewayId = hubConfig.data().gatewayId;
    if (!hubConfig.apply(storage, gatewayId, targetMode, manualUrl, authToken)) {
      uplink.sendGatewayCommandResult(requestId, false, "invalid_config");
      return;
    }
    uplink.sendGatewayCommandResult(requestId, true, "Applied, rebooting");
    delay(200);
    ESP.restart();
    return;
  }

  if (command == "factory_reset") {
    performFactoryReset();
    uplink.sendGatewayCommandResult(requestId, true, "Factory reset done, rebooting");
    delay(200);
    ESP.restart();
    return;
  }

  uplink.sendGatewayCommandResult(requestId, false, "unknown_command");
}

// Backend registers a device_uid -> gateway routing entry only from a
// gateway_status message's state.devices list (see
// HubUplinkClient::sendGatewayStatus()'s header comment) -- this builds
// that list from the Hub's live ESP-NOW roster every time it's sent.
String buildPairedDevicesJson() {
  String out = "[";
  bool any = false;
  for (uint8_t i = 0; i < nhos::EspNowHubManager::maxDevices(); ++i) {
    const nhos::EspNowHubManager::PairedDeviceInfo info = hubManager.slotInfo(i);
    if (!info.used || !info.registered || !info.deviceUidKnown) continue;
    char uidBuf[13];
    snprintf(uidBuf, sizeof(uidBuf), "%02X%02X%02X%02X%02X%02X", info.deviceUid[0],
             info.deviceUid[1], info.deviceUid[2], info.deviceUid[3], info.deviceUid[4],
             info.deviceUid[5]);
    if (any) out += ",";
    any = true;
    bool first = true;
    out += "{";
    nhos::jsonStringField(out, "device_uid", String(uidBuf), first);
    nhos::jsonBoolField(out, "connected", true, first);
    out += "}";
  }
  out += "]";
  return out;
}

// Richer display list for the Desktop UI's Manage Hub panel -- unlike
// buildPairedDevicesJson()'s routing table above, this includes
// not-yet-registered/UID-unknown slots too (status "pending"). The
// staleness check below is now belt-and-suspenders -- EspNowHubManager
// itself actively reaps (frees) slots past kEspNowHubSlotStaleMs (see
// EspNowHubManager::reapStaleSlots(), called every service() tick before
// this function runs each loop()), so a stale slot's `used` is already
// false by the time slotInfo() is read here in the common case. Kept
// anyway as a cheap defensive check, now pointed at the single shared
// constant instead of its own separately-tracked copy.
String buildPairedDevicesDetailJson() {
  String out = "[";
  bool any = false;
  const uint32_t now = millis();
  for (uint8_t i = 0; i < nhos::EspNowHubManager::maxDevices(); ++i) {
    const nhos::EspNowHubManager::PairedDeviceInfo info = hubManager.slotInfo(i);
    if (!info.used) continue;
    if (now - info.lastSeenMs > nhos::kEspNowHubSlotStaleMs) continue;
    if (any) out += ",";
    any = true;
    char macBuf[18];
    snprintf(macBuf, sizeof(macBuf), "%02X:%02X:%02X:%02X:%02X:%02X", info.mac[0], info.mac[1],
             info.mac[2], info.mac[3], info.mac[4], info.mac[5]);
    String uid;
    if (info.deviceUidKnown) {
      char uidBuf[13];
      snprintf(uidBuf, sizeof(uidBuf), "%02X%02X%02X%02X%02X%02X", info.deviceUid[0],
               info.deviceUid[1], info.deviceUid[2], info.deviceUid[3], info.deviceUid[4],
               info.deviceUid[5]);
      uid = uidBuf;
    }
    bool first = true;
    out += "{";
    nhos::jsonStringField(out, "device_uid", uid, first);
    nhos::jsonStringField(out, "mac", String(macBuf), first);
    nhos::jsonStringField(out, "status", info.registered ? "paired" : "pending", first);
    out += "}";
  }
  out += "]";
  return out;
}

void startUplinkIfReady() {
  // hubConfig.isConfigured() should always be true by the time Wi-Fi comes
  // up now (HubConfigPortal's single form writes Wi-Fi + Hub settings
  // together, see HubConfigPortal::handleSave()) -- this guard just avoids
  // registering against Production with an empty gateway_id in the
  // never-should-happen case that NVS has Wi-Fi creds but no Hub config
  // (e.g. a downgrade from an older firmware version).
  if (uplinkStarted || !wifi.isConnected() || !hubConfig.isConfigured()) return;
  const nhos::HubConfigData& cfg = hubConfig.data();
  uplink.begin(cfg.targetMode, cfg.manualUrl, cfg.gatewayId, cfg.authToken);
  uplinkStarted = true;
}

// Boot-time only, mirrors newhorizons_os.ino's serviceAutoOta() -- no
// manual trigger from the Desktop UI for v1 (see the plan). Unlike the
// device side there's no per-unit "autoApplyOnBoot" config flag to check first:
// a Hub has no matrix/IMU workload to interrupt, so checking on every boot
// is unconditional whenever WiFi is up and we're not sitting in the setup
// portal.
void serviceAutoOta() {
  Serial.println(F("[hub] auto_ota_enabled"));
  leds.setSignal(nhos::LedSignal::OtaActive);
  leds.service(millis());
  const bool applied = ota.autoApplyIfNewer(kHubUpdateManifestUrl);
  if (!applied) {
    if (ota.lastPhase() == "current") {
      Serial.println(F("[hub] auto_ota_no_update"));
      return;
    }
    Serial.print(F("[hub] auto_ota_apply_failed status="));
    Serial.println(ota.lastStatusJson());
    leds.showEvent(nhos::LedSignal::OtaError);
    leds.service(millis());
    return;
  }
  leds.showEvent(nhos::LedSignal::OtaSuccess);
  leds.service(millis());
  delay(100);
  ESP.restart();
}

// Reflects current connectivity every loop() tick -- cheap to call
// repeatedly (setSignal() is just an assignment), and simpler than trying
// to thread LED updates through every call site that changes Wi-Fi/uplink
// state.
void updateRuntimeLed() {
  if (!wifi.isConnected()) {
    leds.setSignal(nhos::LedSignal::WifiConnecting);
    return;
  }
  if (!uplinkStarted || !uplink.isConnected()) {
    leds.setSignal(nhos::LedSignal::UplinkDegraded);
    return;
  }
  // Deliberately NOT showing charge state on this LED. With no fuel gauge
  // on GCU V2.3.D the BQ25180 cannot tell "charging" from "no battery
  // fitted" (hence ChargeState::ChargingOrMissing), and a Hub is a
  // mains-powered relay that usually has no battery at all -- so driving
  // the LED from charge state would leave the common case sitting on a
  // permanent amber instead of Online. Connectivity is what this single
  // pixel is for.
  leds.setSignal(nhos::LedSignal::Online);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  bootMs = millis();

  storage.begin();
  leds.begin();
  leds.setSignal(nhos::LedSignal::Boot);

  // Charger bring-up happens here, ahead of both early returns below
  // (quick-boot factory reset, and "no Wi-Fi credentials yet" portal mode):
  // sitting in the setup portal is precisely when the Hub is most likely to
  // be plugged into USB, and it must charge there too.
  //
  // The BQ25180 charges on its own with reset-default registers, so this is
  // not what makes charging work -- it is what makes it charge on *our*
  // terms (4.2 V VBAT, the profile's ICHG, input limit and safety timer)
  // instead of the chip's defaults.
  Wire.begin(nhos::kI2cSda, nhos::kI2cScl, NHOS_BOARD_I2C_HZ);
  power.begin(storage.getString("charge_profile", "slow"));
  // Full status rather than just profileName(): profileName() returns the
  // *requested* profile whether or not the register writes landed, and a
  // Hub has no power UI anywhere else (no Desktop field, no LED), so this
  // boot line is the only place a failed charger bring-up is visible.
  // `configured` and `last_config_error` are the fields to read.
  Serial.printf("[hub] power=%s\n", power.statusJson().c_str());

  // Checked before anything else reads/writes Hub config -- see
  // consumeQuickBootFactoryResetTrigger()'s comment above. A quick-cycle
  // trigger takes priority over (and produces the same outcome as) the
  // ordinary "no Wi-Fi credentials yet" portal entry just below, so it's
  // handled first and returns early.
  if (consumeQuickBootFactoryResetTrigger()) {
    Serial.println("[hub] quick_boot_factory_reset_triggered");
    performFactoryReset();
    // Re-load into the in-memory struct too -- hubConfig.load() hasn't run
    // yet this boot, so this just seeds it with the fresh (now-cleared)
    // NVS state, same as any other first-time boot.
    hubConfig.load(storage);
    leds.showEvent(nhos::LedSignal::Error);
    portalMode = true;
    configPortal.begin(&storage, &hubConfig);
    leds.setSignal(nhos::LedSignal::WifiSetup);
    return;
  }

  hubConfig.load(storage);

  // GCU V2.3.D has no physical setup button -- "no Wi-Fi credentials yet"
  // is the only OTHER trigger for entering this portal (besides the
  // quick-boot-cycle one above); re-entry after first boot is via the
  // portal's own Factory Reset link, or remotely via a `factory_reset`
  // gateway_command while still online (see HubConfigPortal.h and
  // handleGatewayCommand() below). The portal's
  // single form collects Wi-Fi credentials AND Hub settings
  // (gateway_id/target_mode/etc.) together, so this one check gates both.
  //
  // NOTE: can't use wifi.hasCredentials() here -- it reads through the
  // WifiManager's storage_ member, which WifiManager::begin() hasn't set
  // yet at this point (begin() is only called below, after this check
  // decides whether to enter the portal at all). Query storage directly
  // instead; this mirrors hasCredentials()'s own implementation exactly.
  // (Found on real hardware: without this, the Hub always looped back
  // into HubConfigPortal on every boot, even right after a successful
  // portal save -- hasCredentials() was silently always returning false.)
  if (storage.getString("wifi_ssid", "").isEmpty()) {
    portalMode = true;
    configPortal.begin(&storage, &hubConfig);
    leds.setSignal(nhos::LedSignal::WifiSetup);
    return;
  }

  const bool wifiConnected = wifi.begin(storage);
  leds.setSignal(wifiConnected ? nhos::LedSignal::Online : nhos::LedSignal::WifiSetup);

  ota.begin(storage);
  if (wifiConnected) {
    serviceAutoOta();  // may ESP.restart() and never return
  }

  if (!hubManager.begin()) {
    Serial.println("[hub] esp_now_init FAILED");
  }
  esp_now_register_recv_cb(onEspNowRecv);
  hubManager.onFrameReady(onHubFrameReady, nullptr);
  hubManager.onControlFrameReady(onHubControlFrameReady, nullptr);
  hubManager.onHubRequestFrameReady(onHubRequestFrameReady, nullptr);
  hubManager.onOtaChunkAck(onOtaChunkAckReceived, nullptr);
  hubManager.onControlAck(onControlAckReceived, nullptr);
  commandDispatcher.begin(&hubManager, &uplink);
  otaRelay.begin(&hubManager);
  uplink.onCommand(onUplinkCommand, nullptr);
  uplink.onGatewayCommand(handleGatewayCommand, nullptr);

  startUplinkIfReady();

  // esp_wifi_get_mac() reads straight from eFuse -- this is the MAC
  // devices need in DeviceConfig.transport.hubMac to pair with this Hub
  // over ESP-NOW.
  uint8_t mac[6] = {0};
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
  Serial.printf("[hub] my_mac=%s\n", macStr);
  uint8_t currentChannel = 0;
  wifi_second_chan_t secondChannel = WIFI_SECOND_CHAN_NONE;
  esp_wifi_get_channel(&currentChannel, &secondChannel);
  Serial.printf("[hub] wifi_channel=%u\n", currentChannel);
  Serial.printf("[hub] gateway_id=%s\n", hubConfig.data().gatewayId.c_str());

  Serial.println("[hub] ready");
}

void loop() {
  serviceQuickBootCounterClear();

  if (portalMode) {
    configPortal.service();
    power.service(millis());
    leds.service(millis());
    return;
  }

  wifi.service();
  startUplinkIfReady();
  hubManager.service();
  commandDispatcher.service();
  otaRelay.service();
  uplink.service();
  uplink.sendGatewayStatus(hubManager.registeredCount(), buildPairedDevicesJson(),
                            buildPairedDevicesDetailJson());
  power.service(millis());
  updateRuntimeLed();
  leds.service(millis());
}
