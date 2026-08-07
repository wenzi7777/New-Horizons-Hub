#include "EspNowCommandDispatcher.h"

#include <cstdlib>
#include <cstring>
#include <esp_now.h>

#include "JsonUtils.h"

namespace nhos {

namespace {
// Mirrors New-Horizons-Gateway/newhorizons_gateway/udp_control.py's
// UDPCommandDispatcher.RESEND_INTERVAL_SEC / MAX_UNACKED_ATTEMPTS exactly
// (0.5s * 30 attempts = 15s max time-to-timeout).
constexpr uint32_t kResendIntervalMs = 500;
constexpr uint8_t kMaxAttempts = 30;

// Deadline for the (possibly slow, possibly multi-fragment) response once
// delivery is confirmed acked -- anchored to the ack event, not a
// continuation of the pre-ack attempt budget, since execution time and
// response-drain time are both variable. 10s comfortably covers the
// largest known default duration_ms (3000ms, ControlServer.cpp's
// calibration handlers) plus the paced response drain, while still
// bounding how long a stuck slot blocks this device's next command (only
// one command in flight per device by design -- see this header's own
// comment).
constexpr uint32_t kPostAckResponseTimeoutMs = 10000;

bool parseDeviceUid(const String& deviceUid, uint8_t out[6]) {
  if (deviceUid.length() != 12) return false;
  for (size_t i = 0; i < 6; ++i) {
    const String byteHex = deviceUid.substring(i * 2, i * 2 + 2);
    char* end = nullptr;
    const long value = strtol(byteHex.c_str(), &end, 16);
    if (end == byteHex.c_str() || *end != '\0') return false;
    out[i] = static_cast<uint8_t>(value);
  }
  return true;
}
}  // namespace

void EspNowCommandDispatcher::begin(EspNowHubManager* hubManager, HubUplinkClient* uplink) {
  hubManager_ = hubManager;
  uplink_ = uplink;
}

void EspNowCommandDispatcher::sendFragmentsTo(const uint8_t mac[6], const String& json) {
  // static, not a stack local: kEspNowMaxFragCount * sizeof(EspNowFragment)
  // is ~8KB, far more stack than this task has (EspNowPairing.h's
  // responseFrags_ comment documents a ~4KB stack array being enough to
  // corrupt the heap on this hardware). Safe as static because this only
  // ever runs on the main loop task -- sendCommand() is reached from
  // uplink.service(), and service() from loop() -- never re-entrantly and
  // never from an ESP-NOW recv callback.
  static EspNowFragment frags[kEspNowDataFragCount];
  const uint8_t count = EspNowFragmenter::fragment(
      reinterpret_cast<const uint8_t*>(json.c_str()), json.length(), 0,
      kEspNowFragTypeControl, frags, kEspNowDataFragCount);
  if (count == 0) {
    // Command payload itself too large to fragment -- would otherwise send
    // nothing at all and leave the device waiting until the retry budget
    // expired. Log it so this can never be a silent failure.
    Serial.printf("[cmd_dispatch] fragment_failed payload_len=%u max=%u\n",
                  static_cast<unsigned>(json.length()),
                  static_cast<unsigned>(kEspNowDataFragCount * kEspNowFragMaxPayload));
    return;
  }
  for (uint8_t i = 0; i < count; ++i) {
    esp_now_send(mac, frags[i].bytes, frags[i].len);
  }
}

void EspNowCommandDispatcher::rejectImmediately(const String& deviceUid, const String& requestId,
                                                 const String& command, const char* message) {
  if (uplink_ == nullptr) return;
  String result = "{";
  bool first = true;
  jsonStringField(result, "device_uid", deviceUid, first);
  jsonStringField(result, "request_id", requestId, first);
  jsonStringField(result, "command", command, first);
  jsonStringField(result, "status", "error", first);
  jsonStringField(result, "message", message, first);
  result += "}";
  uplink_->sendDeviceMessage("result", deviceUid, result);
}

void EspNowCommandDispatcher::sendCommand(const String& deviceUid, const String& payloadJson) {
  if (hubManager_ == nullptr || uplink_ == nullptr) return;

  const String requestId = jsonExtractString(payloadJson, "request_id", "");
  const String command = jsonExtractString(payloadJson, "command", "");

  uint8_t uidBytes[6] = {0};
  uint8_t mac[6] = {0};
  const bool known = parseDeviceUid(deviceUid, uidBytes) &&
                      hubManager_->macForDeviceUid(uidBytes, mac);
  if (!known) {
    // Mirrors New-Horizons-Gateway/newhorizons_gateway/main.py's
    // on_command() "device_not_connected_to_gateway" branch.
    rejectImmediately(deviceUid, requestId, command, "device_not_connected_to_hub");
    return;
  }

  int freeIdx = -1;
  for (uint8_t i = 0; i < kEspNowCommandMaxPending; ++i) {
    if (pending_[i].used && memcmp(pending_[i].mac, mac, 6) == 0) {
      rejectImmediately(deviceUid, requestId, command, "device_command_already_in_flight");
      return;
    }
    if (freeIdx < 0 && !pending_[i].used) freeIdx = i;
  }
  if (freeIdx < 0) {
    // Can't happen in practice -- kEspNowCommandMaxPending ==
    // kEspNowHubMaxDevices, and macForDeviceUid() only resolves MACs for
    // devices that already hold a Hub roster slot.
    rejectImmediately(deviceUid, requestId, command, "hub_command_roster_full");
    return;
  }

  PendingCommand& entry = pending_[freeIdx];
  entry.used = true;
  memcpy(entry.mac, mac, 6);
  entry.deviceUid = deviceUid;
  entry.requestId = requestId;
  entry.command = command;
  entry.payloadJson = payloadJson;
  entry.attempts = 1;
  entry.lastSentMs = millis();
  Serial.printf("[cmd_dispatch] send device_uid=%s mac=%02X:%02X:%02X:%02X:%02X:%02X cmd=%s "
                "payload_len=%u\n",
                deviceUid.c_str(), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], command.c_str(),
                static_cast<unsigned>(payloadJson.length()));
  sendFragmentsTo(mac, payloadJson);
}

void EspNowCommandDispatcher::service() {
  const uint32_t now = millis();
  for (uint8_t i = 0; i < kEspNowCommandMaxPending; ++i) {
    PendingCommand& entry = pending_[i];
    if (!entry.used) continue;
    if (entry.acked) {
      // Delivery confirmed -- never resend the raw command again, just
      // wait out a separate, longer deadline for the response itself.
      if (now - entry.ackedMs >= kPostAckResponseTimeoutMs) {
        failPending(i, "command_delivery_timeout");
      }
      continue;
    }
    if (entry.attempts >= kMaxAttempts) {
      failPending(i, "command_delivery_timeout");
      continue;
    }
    if (now - entry.lastSentMs >= kResendIntervalMs) {
      sendFragmentsTo(entry.mac, entry.payloadJson);
      entry.lastSentMs = now;
      ++entry.attempts;
      Serial.printf("[cmd_dispatch] resend device_uid=%s attempt=%u\n", entry.deviceUid.c_str(),
                    entry.attempts);
    }
  }
}

void EspNowCommandDispatcher::handleControlAck(const uint8_t mac[6]) {
  for (uint8_t i = 0; i < kEspNowCommandMaxPending; ++i) {
    PendingCommand& entry = pending_[i];
    if (entry.used && !entry.acked && memcmp(entry.mac, mac, 6) == 0) {
      entry.acked = true;
      entry.ackedMs = millis();
      Serial.printf("[cmd_dispatch] acked device_uid=%s\n", entry.deviceUid.c_str());
      return;
    }
  }
}

bool EspNowCommandDispatcher::hasPendingCommand() const {
  for (uint8_t i = 0; i < kEspNowCommandMaxPending; ++i) {
    if (pending_[i].used) return true;
  }
  return false;
}

void EspNowCommandDispatcher::handleControlResponse(uint8_t /*deviceIndex*/, const uint8_t mac[6],
                                                      const uint8_t* data, size_t len) {
  if (uplink_ == nullptr) return;
  int idx = -1;
  for (uint8_t i = 0; i < kEspNowCommandMaxPending; ++i) {
    if (pending_[i].used && memcmp(pending_[i].mac, mac, 6) == 0) {
      idx = i;
      break;
    }
  }
  if (idx < 0) return;  // unsolicited/late response -- nothing pending to match it to

  const String response(reinterpret_cast<const char*>(data), len);
  completePending(static_cast<uint8_t>(idx), response);
}

void EspNowCommandDispatcher::completePending(uint8_t idx, const String& response) {
  const PendingCommand entry = pending_[idx];  // copy before clearing
  pending_[idx] = PendingCommand();

  // response is ControlServer::ok()/error()'s shape:
  // {"ok":bool,"cmd":...,"message":...,"data":{...},"error":...}. Augment
  // it with device_uid/request_id/command/status the same way
  // New-Horizons-Gateway/newhorizons_gateway/main.py's handle_udp_control()
  // does for its UDP "result" path, so the backend sees an identical
  // result shape regardless of transport.
  bool ok = false;
  jsonExtractBool(response, "ok", ok);
  const String cmd = jsonExtractString(response, "cmd", entry.command);
  const String message = jsonExtractString(response, "message", "");
  String dataRaw;
  if (!jsonExtractObject(response, "data", dataRaw)) dataRaw = "{}";
  const String errorStr = jsonExtractString(response, "error", "");

  String result = "{";
  bool first = true;
  jsonStringField(result, "device_uid", entry.deviceUid, first);
  jsonStringField(result, "request_id", entry.requestId, first);
  jsonStringField(result, "command", cmd, first);
  jsonStringField(result, "status", ok ? "ok" : "error", first);
  jsonBoolField(result, "ok", ok, first);
  jsonStringField(result, "message", message, first);
  jsonRawField(result, "data", dataRaw, first);
  jsonStringField(result, "error", errorStr, first);
  result += "}";

  uplink_->sendDeviceMessage("result", entry.deviceUid, result);
}

void EspNowCommandDispatcher::failPending(uint8_t idx, const char* message) {
  const PendingCommand entry = pending_[idx];
  pending_[idx] = PendingCommand();
  rejectImmediately(entry.deviceUid, entry.requestId, entry.command, message);
}

}  // namespace nhos
