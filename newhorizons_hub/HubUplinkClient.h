#pragma once

// WiFi STA + TLS WebSocket client to the Desktop Backend, mirroring
// New-Horizons-Gateway/newhorizons_gateway/upstream_wss.py's wire protocol
// exactly (hello / gateway_hello_ack handshake, gateway_status heartbeat,
// sensor data as a "NHPKT1:" + base64 text frame) so the backend
// (New-Horizons-Desktop/backend/newhorizons_backend/gateway_ws.py) needs
// no changes at all -- it already treats "gateway" as a generic
// authenticated WS packet source.
//
// Uses WebSocketsClient (Links2004/arduinoWebSockets) -- real-hardware
// heap measurement on GCU V2.3.D (no PSRAM) showed >5x headroom over the
// firmware's own low-heap danger threshold with this library, so there
// was no need to fall back to esp_websocket_client.

#include <Arduino.h>
#include <WebSocketsClient.h>

namespace nhos {

// Mirrors New-Horizons-Gateway/newhorizons_gateway/config_store.py's
// target_mode enum and URL resolution exactly, so Hub config UX (Phase 5)
// can reuse the same mental model as the existing Gateway.
enum class UplinkTargetMode : uint8_t {
  Production = 0,
  Local = 1,
  Manual = 2,
};

// Wired from HubUplinkClient::onCommand(). deviceUid is 12 uppercase hex
// chars, no separators (matches ControlServer::deviceUidString()).
// payloadJson is the raw JSON object from the backend's `payload` field
// (contains "command", normally "request_id", and any command-specific
// params) -- passed through unparsed so the caller (EspNowCommandDispatcher)
// can forward it to the device as-is.
using HubCommandCallback = void (*)(const String& deviceUid,
                                     const String& payloadJson, void* userData);

// Wired from HubUplinkClient::onGatewayCommand(). Distinct from
// HubCommandCallback above: this targets the Hub process itself (settings
// change, factory reset), not a downstream ESP-NOW device, so there is no
// device_uid -- routing is implicit in which Hub's WS session received it.
// `payloadJson` is the raw JSON object from the backend's `payload` field
// (contains "command" plus any command-specific params), passed through
// unparsed so the caller (newhorizons_hub.ino's handleGatewayCommand())
// can parse only what it needs.
using HubGatewayCommandCallback = void (*)(const String& requestId,
                                            const String& payloadJson, void* userData);

class HubUplinkClient {
 public:
  // `manualUrl` is only used when mode == Manual. `gatewayId` is this
  // Hub's identity (see plan section: currently devices report through
  // the Hub using their own device_uid, the Hub itself doesn't need a
  // distinct backend-side "hub" concept -- gateway_id is enough).
  void begin(UplinkTargetMode mode, const String& manualUrl,
             const String& gatewayId, const String& authToken);

  // Call every loop() iteration.
  void service();

  bool isConnected() const { return connected_; }

  // Forwards reassembled sensor data upstream unmodified, framed exactly
  // like upstream_wss.py::_send_binary(). Silently dropped if not
  // connected -- sensor data is lossy by design, matching the rest of
  // this codebase's philosophy (no local queue/retry for data frames).
  void sendSensorPacket(const uint8_t* data, size_t len);

  // gateway_status heartbeat, coalesced like upstream_wss.py's
  // coalesce_type="gateway_status" (only the latest call before the next
  // send actually goes out). `devicesJsonArray` must be a JSON array of
  // {"device_uid":"...","connected":true} objects -- gateway_ws.py's
  // record_gateway_summary() only populates its device_uid -> sender
  // routing table (which publish_command() needs to find this device at
  // all) from a gateway_status message's state.devices list, exactly like
  // a real Gateway's own periodic status does (see
  // New-Horizons-Gateway/newhorizons_gateway/main.py's send_gateway_status
  // call and state.py's snapshot()). Found missing on real hardware: Step
  // 1-3 validation only exercised sensor-data forwarding, which never
  // exercises this registration path, so a command sent from the Desktop
  // UI silently had nowhere to route to until this was added.
  //
  // Every call also piggybacks this Hub's actual current WiFi/ESP-NOW
  // channel (read via esp_wifi_get_channel()) into the same payload --
  // see hub_channel_watch.py's header comment for why: the backend uses
  // this purely to *detect* (never assign) channel collisions between
  // Hubs an admin has grouped into the same "environment", since
  // esp_wifi_set_channel() was found to be a no-op once a Hub's WiFi STA
  // is associated to a real access point.
  //
  // `pairedDevicesDetailJsonArray` is a second, richer array (each item:
  // {"device_uid":...,"mac":...,"status":"paired"|"pending"}) surfaced to
  // the Desktop UI as-is (the now-removed DirectWebUI used to render this
  // same data locally) -- unlike `devicesJsonArray` above this is display
  // data, not a routing table, so unregistered/UID-unknown slots are
  // included too.
  void sendGatewayStatus(uint8_t pairedDeviceCount, const String& devicesJsonArray,
                          const String& pairedDevicesDetailJsonArray);

  // Invoked (from service(), main-loop context) whenever the backend sends
  // a {"type":"command","device_uid":...,"payload":{...}} message -- see
  // gateway_ws.py's on_command handling and
  // New-Horizons-Gateway/newhorizons_gateway/main.py's on_command() for the
  // wire shape this mirrors.
  void onCommand(HubCommandCallback callback, void* userData);

  // Sends a device_* message upstream, mirroring
  // upstream_wss.py::send_device_message()'s msg_type->upstream type
  // mapping exactly ("hello"->"device_hello", "status"->"device_status",
  // "update_progress"->"device_update_progress", "result"->"device_result").
  // `payloadJson` is embedded as-is (already-serialized JSON object).
  void sendDeviceMessage(const char* msgType, const String& deviceUid,
                          const String& payloadJson);

  // Invoked (from service(), main-loop context) whenever the backend sends
  // a {"type":"gateway_command","request_id":...,"payload":{...}} message
  // -- the Hub-targeted counterpart to onCommand() above. Backend-initiated
  // (Desktop's Manage Hub panel -> service.py::publish_gateway_command()),
  // used for `set_config`/`factory_reset` since those are the only
  // operations that must reach the Hub process itself rather than being
  // servable by the Backend alone (see plan section "移除 Hub 常駐
  // WebUI..." for why LAN scan/migrate/status don't need this channel).
  void onGatewayCommand(HubGatewayCommandCallback callback, void* userData);

  // Reply to a gateway_command, mirroring sendDeviceMessage()'s shape but
  // without a device_uid: {"type":"gateway_command_result","request_id":...,
  // "result":{"ok":...,"message":...}}.
  void sendGatewayCommandResult(const String& requestId, bool ok, const String& message);

 private:
  void connectNow();
  void handleWsEvent(WStype_t type, uint8_t* payload, size_t length);
  static void wsEventTrampoline(WStype_t type, uint8_t* payload, size_t length);

  WebSocketsClient webSocket_;
  String host_;
  uint16_t port_ = 443;
  String path_;
  bool useTls_ = true;
  String gatewayId_;
  String authToken_;
  // Stashed from begin() purely so service()'s gateway_status heartbeat can
  // report them back (as target_mode/server_url, reusing the same payload
  // keys a real Gateway's own heartbeat already uses) -- found on real
  // hardware that without this, the Desktop Manage Hub panel's settings
  // form had no way to know this Hub's actual current target/URL and
  // silently defaulted to Production with an empty URL, which would have
  // reconfigured a working manual-mode Hub out from under itself the first
  // time someone clicked "Save & Reboot" without also re-entering these.
  UplinkTargetMode targetMode_ = UplinkTargetMode::Production;
  String manualUrl_;
  bool connected_ = false;
  bool helloSent_ = false;

  bool statusPending_ = false;
  uint8_t pendingPairedDeviceCount_ = 0;
  String pendingDevicesJsonArray_ = "[]";
  String pendingPairedDevicesDetailJsonArray_ = "[]";
  uint32_t lastStatusSentMs_ = 0;

  HubCommandCallback commandCallback_ = nullptr;
  void* commandCallbackUserData_ = nullptr;

  HubGatewayCommandCallback gatewayCommandCallback_ = nullptr;
  void* gatewayCommandCallbackUserData_ = nullptr;

  static HubUplinkClient* instance_;
};

}  // namespace nhos
