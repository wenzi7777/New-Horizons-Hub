#include "HubUplinkClient.h"

#include <WiFi.h>
#include <cstring>
#include <esp_wifi.h>

#include "Config.h"
#include "JsonUtils.h"

namespace nhos {

namespace {
// Mirrors New-Horizons-Gateway/newhorizons_gateway/config_store.py exactly.
constexpr char kProductionHost[] = "isensing-s1.u-aizu.ac.jp";
constexpr uint16_t kProductionPort = 443;
constexpr char kProductionPath[] = "/newhorizons/gateway/ws";
constexpr char kLocalHost[] = "127.0.0.1";
constexpr uint16_t kLocalPort = 5051;
constexpr char kLocalPath[] = "/newhorizons/gateway/ws";

// Must match PACKET_TEXT_PREFIX in both upstream_wss.py and gateway_ws.py
// exactly -- the backend branches on this literal string.
constexpr char kPacketTextPrefix[] = "NHPKT1:";

constexpr uint32_t kGatewayStatusIntervalMs = 5000;
constexpr uint32_t kReconnectIntervalMs = 5000;

// Duplicates HubConfig::targetModeToString()'s mapping rather than
// including HubConfig.h -- that header already includes this one (for
// UplinkTargetMode), so including it back here would be circular.
const char* targetModeToString(UplinkTargetMode mode) {
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

// base64 alphabet -- Arduino has no built-in base64 encoder in scope here,
// and pulling in a whole library for this one call isn't worth it.
size_t base64Encode(const uint8_t* data, size_t len, char* out, size_t outCapacity) {
  static const char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const size_t needed = ((len + 2) / 3) * 4;
  if (needed >= outCapacity) return 0;  // leave room for a NUL terminator

  size_t o = 0;
  size_t i = 0;
  for (; i + 3 <= len; i += 3) {
    const uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                        (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
    out[o++] = kTable[(n >> 18) & 0x3F];
    out[o++] = kTable[(n >> 12) & 0x3F];
    out[o++] = kTable[(n >> 6) & 0x3F];
    out[o++] = kTable[n & 0x3F];
  }
  const size_t remaining = len - i;
  if (remaining == 1) {
    const uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    out[o++] = kTable[(n >> 18) & 0x3F];
    out[o++] = kTable[(n >> 12) & 0x3F];
    out[o++] = '=';
    out[o++] = '=';
  } else if (remaining == 2) {
    const uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                        (static_cast<uint32_t>(data[i + 1]) << 8);
    out[o++] = kTable[(n >> 18) & 0x3F];
    out[o++] = kTable[(n >> 12) & 0x3F];
    out[o++] = kTable[(n >> 6) & 0x3F];
    out[o++] = '=';
  }
  out[o] = '\0';
  return o;
}

}  // namespace

HubUplinkClient* HubUplinkClient::instance_ = nullptr;

void HubUplinkClient::begin(UplinkTargetMode mode, const String& manualUrl,
                             const String& gatewayId, const String& authToken) {
  gatewayId_ = gatewayId;
  authToken_ = authToken;
  targetMode_ = mode;
  manualUrl_ = manualUrl;
  instance_ = this;

  switch (mode) {
    case UplinkTargetMode::Local:
      host_ = kLocalHost;
      port_ = kLocalPort;
      path_ = kLocalPath;
      useTls_ = false;
      break;
    case UplinkTargetMode::Manual: {
      // Minimal manual URL parse: "ws(s)://host[:port]/path". Good enough
      // for the manual/dev-override case; production/local don't go
      // through this path at all.
      String url = manualUrl;
      useTls_ = url.startsWith("wss://");
      const int schemeLen = useTls_ ? 6 : 5;
      url = url.substring(schemeLen);
      const int slashIdx = url.indexOf('/');
      String hostPort = slashIdx >= 0 ? url.substring(0, slashIdx) : url;
      path_ = slashIdx >= 0 ? url.substring(slashIdx) : "/";
      const int colonIdx = hostPort.indexOf(':');
      if (colonIdx >= 0) {
        host_ = hostPort.substring(0, colonIdx);
        port_ = static_cast<uint16_t>(hostPort.substring(colonIdx + 1).toInt());
      } else {
        host_ = hostPort;
        port_ = useTls_ ? 443 : 80;
      }
      break;
    }
    case UplinkTargetMode::Production:
    default:
      host_ = kProductionHost;
      port_ = kProductionPort;
      path_ = kProductionPath;
      useTls_ = true;
      break;
  }

  Serial.printf("[hub_uplink] target host=%s port=%u path=%s tls=%d\n", host_.c_str(), port_,
                path_.c_str(), useTls_ ? 1 : 0);
  connectNow();
}

void HubUplinkClient::connectNow() {
  webSocket_.onEvent(&HubUplinkClient::wsEventTrampoline);
  webSocket_.setReconnectInterval(kReconnectIntervalMs);
  if (!authToken_.isEmpty()) {
    webSocket_.setAuthorization(("Bearer " + authToken_).c_str());
  }
  if (useTls_) {
    webSocket_.beginSSL(host_.c_str(), port_, path_.c_str());
  } else {
    webSocket_.begin(host_.c_str(), port_, path_.c_str());
  }
}

void HubUplinkClient::wsEventTrampoline(WStype_t type, uint8_t* payload, size_t length) {
  if (instance_ != nullptr) {
    instance_->handleWsEvent(type, payload, length);
  }
}

void HubUplinkClient::handleWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      connected_ = true;
      Serial.println(F("[hub_uplink] ws_connected"));
      // Matches upstream_wss.py's hello message shape (gateway_ws.py reads
      // gateway_id from this to register the session), plus two fields the
      // real Gateway software's hello never sends: `client_type` lets the
      // backend tell a Hub apart from a Gateway (previously there was no
      // wire-level self-identification at all -- both looked identical to
      // service.py's register_gateway(), which even defaulted every
      // gateway_name to "New Horizons Gateway"). `version` mirrors what
      // upstream_wss.py's own hello already sends for a real Gateway.
      String hello = "{";
      bool helloFirst = true;
      jsonStringField(hello, "type", "hello", helloFirst);
      jsonStringField(hello, "gateway_id", gatewayId_, helloFirst);
      jsonStringField(hello, "client_type", "hub", helloFirst);
      jsonStringField(hello, "version", kFirmwareVersion, helloFirst);
      hello += "}";
      webSocket_.sendTXT(hello);
      helloSent_ = true;
      break;
    }
    case WStype_DISCONNECTED:
      if (connected_) Serial.println(F("[hub_uplink] ws_disconnected"));
      connected_ = false;
      helloSent_ = false;
      break;
    case WStype_TEXT: {
      if (payload == nullptr) break;
      const String message(reinterpret_cast<const char*>(payload), length);
      String msgType;
      if (!jsonExtractString(message, "type", msgType)) break;

      if (msgType == "command" && commandCallback_ != nullptr) {
        // Mirrors gateway_ws.py's outbound command shape:
        // {"type":"command","device_uid":...,"request_id":...,"payload":{...}}
        // -- device_uid may also be embedded in payload, matching
        // upstream_wss.py::_receive_loop()'s fallback lookup.
        String deviceUid;
        String payloadJson;
        const bool havePayload = jsonExtractObject(message, "payload", payloadJson);
        if (!jsonExtractString(message, "device_uid", deviceUid) || deviceUid.isEmpty()) {
          if (havePayload) jsonExtractString(payloadJson, "device_uid", deviceUid);
        }
        if (!deviceUid.isEmpty() && havePayload) {
          commandCallback_(deviceUid, payloadJson, commandCallbackUserData_);
        }
        break;
      }

      if (msgType == "gateway_command" && gatewayCommandCallback_ != nullptr) {
        // {"type":"gateway_command","request_id":...,"payload":{...}} --
        // targets this Hub process itself, no device_uid involved.
        String requestId;
        String payloadJson;
        jsonExtractString(message, "request_id", requestId);
        if (jsonExtractObject(message, "payload", payloadJson)) {
          gatewayCommandCallback_(requestId, payloadJson, gatewayCommandCallbackUserData_);
        }
        break;
      }
      break;
    }
    default:
      break;
  }
}

void HubUplinkClient::onCommand(HubCommandCallback callback, void* userData) {
  commandCallback_ = callback;
  commandCallbackUserData_ = userData;
}

void HubUplinkClient::sendDeviceMessage(const char* msgType, const String& deviceUid,
                                         const String& payloadJson) {
  if (!connected_ || !helloSent_) return;

  String upstreamType = "device_status";
  if (strcmp(msgType, "hello") == 0) {
    upstreamType = "device_hello";
  } else if (strcmp(msgType, "update_progress") == 0) {
    upstreamType = "device_update_progress";
  } else if (strcmp(msgType, "result") == 0) {
    upstreamType = "device_result";
  }

  String message = "{";
  bool first = true;
  jsonStringField(message, "type", upstreamType, first);
  jsonStringField(message, "gateway_id", gatewayId_, first);
  jsonStringField(message, "device_uid", deviceUid, first);
  jsonRawField(message, "payload", payloadJson, first);
  message += "}";
  webSocket_.sendTXT(message);
}

void HubUplinkClient::sendSensorPacket(const uint8_t* data, size_t len) {
  if (!connected_ || !helloSent_) return;

  // base64 expands ~4/3; kEspNowMaxFragCount*kEspNowFragMaxPayload (3840B)
  // is the largest frame this Hub ever reassembles, so size generously
  // beyond that rather than against this specific call's `len`.
  static char base64Buf[5200];
  const size_t encodedLen = base64Encode(data, len, base64Buf, sizeof(base64Buf));
  if (encodedLen == 0) return;  // frame too large for the buffer -- drop it

  static String frame;
  frame = kPacketTextPrefix;
  frame += base64Buf;
  webSocket_.sendTXT(frame);
}

void HubUplinkClient::sendGatewayStatus(uint8_t pairedDeviceCount, const String& devicesJsonArray,
                                         const String& pairedDevicesDetailJsonArray) {
  pendingPairedDeviceCount_ = pairedDeviceCount;
  pendingDevicesJsonArray_ = devicesJsonArray.isEmpty() ? String("[]") : devicesJsonArray;
  pendingPairedDevicesDetailJsonArray_ =
      pairedDevicesDetailJsonArray.isEmpty() ? String("[]") : pairedDevicesDetailJsonArray;
  statusPending_ = true;
}

void HubUplinkClient::onGatewayCommand(HubGatewayCommandCallback callback, void* userData) {
  gatewayCommandCallback_ = callback;
  gatewayCommandCallbackUserData_ = userData;
}

void HubUplinkClient::sendGatewayCommandResult(const String& requestId, bool ok,
                                                const String& message) {
  if (!connected_ || !helloSent_) return;
  String result = "{";
  bool resultFirst = true;
  jsonBoolField(result, "ok", ok, resultFirst);
  jsonStringField(result, "message", message, resultFirst);
  result += "}";

  String out = "{";
  bool first = true;
  jsonStringField(out, "type", "gateway_command_result", first);
  jsonStringField(out, "request_id", requestId, first);
  jsonRawField(out, "result", result, first);
  out += "}";
  webSocket_.sendTXT(out);
}

void HubUplinkClient::service() {
  webSocket_.loop();

  if (!connected_) return;

  const uint32_t now = millis();
  if (statusPending_ && now - lastStatusSentMs_ >= kGatewayStatusIntervalMs) {
    lastStatusSentMs_ = now;
    statusPending_ = false;
    String state = "{";
    bool stateFirst = true;
    jsonRawField(state, "devices", pendingDevicesJsonArray_, stateFirst);
    state += "}";

    // Piggyback this Hub's actual current channel on the same heartbeat --
    // see hub_channel_watch.py's header comment. No separate request/
    // response round trip: the backend only uses this to detect (never
    // assign) collisions between same-"environment" Hubs.
    uint8_t currentChannel = 0;
    wifi_second_chan_t secondChannel = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&currentChannel, &secondChannel);

    // Also piggyback this Hub's own STA MAC -- previously the only way to
    // find it was reading the Hub's serial log or its own DirectWebUI page
    // (both need physical/network access to the Hub itself), even though a
    // device needs exactly this value in its TransportConfig.hubMac to pair.
    // Surfacing it here lets the Desktop UI show it directly.
    uint8_t macBytes[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, macBytes);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", macBytes[0], macBytes[1],
             macBytes[2], macBytes[3], macBytes[4], macBytes[5]);

    // Also piggyback this Hub's current LAN IP -- otherwise the only way
    // to find it (to actually open DirectWebUI in a browser) is reading
    // the serial log, which needs USB access to the Hub itself. Surfacing
    // it here lets the Desktop UI offer a direct "open" link.
    const String ip = WiFi.localIP().toString();

    String payload = "{";
    bool payloadFirst = true;
    jsonUnsignedField(payload, "paired_device_count", pendingPairedDeviceCount_, payloadFirst);
    jsonUnsignedField(payload, "channel", currentChannel, payloadFirst);
    jsonStringField(payload, "mac", String(macStr), payloadFirst);
    jsonStringField(payload, "ip", ip, payloadFirst);
    jsonRawField(payload, "state", state, payloadFirst);
    // Richer display list for the Desktop UI's Manage Hub panel -- see
    // sendGatewayStatus()'s header comment for how this differs from
    // `state.devices` above (that one is a routing table, this one is
    // display data and includes not-yet-registered slots too).
    jsonRawField(payload, "paired_devices", pendingPairedDevicesDetailJsonArray_, payloadFirst);
    // Reuses the exact payload keys a real Gateway's own heartbeat already
    // sends (service.py's record_gateway_summary() forwards both
    // generically, no backend change needed) so the Desktop Manage Hub
    // panel's settings form can pre-fill the Hub's actual current target
    // instead of silently defaulting to Production -- see the member
    // comment on targetMode_/manualUrl_ for what that bug looked like.
    // auth_token is deliberately never echoed back here.
    jsonStringField(payload, "target_mode", targetModeToString(targetMode_), payloadFirst);
    if (targetMode_ == UplinkTargetMode::Manual) {
      jsonStringField(payload, "server_url", manualUrl_, payloadFirst);
    }
    payload += "}";

    String status = "{";
    bool first = true;
    jsonStringField(status, "type", "gateway_status", first);
    jsonStringField(status, "gateway_id", gatewayId_, first);
    jsonRawField(status, "payload", payload, first);
    status += "}";
    webSocket_.sendTXT(status);
  }
}

}  // namespace nhos
