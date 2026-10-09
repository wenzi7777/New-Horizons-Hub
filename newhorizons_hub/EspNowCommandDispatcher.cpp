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
// response-drain time are both variable. A command carrying duration_ms
// (the calibration captures, which block for that long before answering)
// gets that much on top, so a long capture is not reported as a delivery
// timeout while the device is still measuring. Still bounded, so a stuck
// slot does not block this device's next command forever (only one
// command in flight per device by design -- see this header's own
// comment).
constexpr uint32_t kPostAckResponseTimeoutMs = 10000;

// The device's page envelope; see its EspNowResultPager.h.
constexpr char kPagePrefix[] = "{\"type\":\"result_chunk\"";

String pageRequestJson(long rid, long chunk) {
  String json = "{\"command\":\"result_page\",\"rid\":";
  json += rid;
  json += ",\"chunk\":";
  json += chunk;
  json += "}";
  return json;
}

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

// Mirrors NewHorizonsOS-OTA/.../EspNowStreamTransport.cpp's kSendWindowUs --
// same reasoning, and generous relative to kResendIntervalMs so a burst
// always completes well before its own resend would fire.
constexpr uint32_t kSendWindowUs = 15000;

}  // namespace

void EspNowCommandDispatcher::begin(EspNowHubManager* hubManager, HubUplinkClient* uplink) {
  hubManager_ = hubManager;
  uplink_ = uplink;
}

EspNowCommandDispatcher::QueueResult EspNowCommandDispatcher::queueFragmentsTo(
    const uint8_t mac[6], const String& json) {
  if (outSent_ < outCount_) {
    // A burst is still going out, and there is only one outbound buffer
    // because there is only one radio. Overwriting it here would truncate
    // the burst already in flight -- the receiver would wait for fragments
    // that are never sent and fall back on the retry budget. The caller
    // leaves the command due for a resend instead.
    return QueueResult::Busy;
  }
  // outFrags_ is a member rather than a stack local: kEspNowDataFragCount *
  // sizeof(EspNowFragment) is ~4KB, far more stack than this task has
  // (EspNowPairing.h's responseFrags_ comment documents a ~4KB stack array
  // being enough to corrupt the heap on this hardware). Safe as a member
  // because this only ever runs on the main loop task -- sendCommand() is
  // reached from uplink.service(), and service() from loop() -- never
  // re-entrantly and never from an ESP-NOW recv callback.
  const uint8_t count = EspNowFragmenter::fragment(
      reinterpret_cast<const uint8_t*>(json.c_str()), json.length(), 0,
      kEspNowFragTypeControl, outFrags_, kEspNowDataFragCount);
  if (count == 0) {
    // Command payload itself too large to fragment. This used to be logged
    // and then treated as sent, so the caller only heard about it as a
    // command_delivery_timeout 15 s later; the caller now fails it at once.
    Serial.printf("[cmd_dispatch] fragment_failed payload_len=%u max=%u\n",
                  static_cast<unsigned>(json.length()),
                  static_cast<unsigned>(kEspNowDataFragCount * kEspNowFragMaxPayload));
    return QueueResult::TooLarge;
  }
  memcpy(outMac_, mac, 6);
  outCount_ = count;
  outSent_ = 0;
  outFragIntervalUs_ = kSendWindowUs / count;
  outNextDueUs_ = micros();
  servicePacedSend(outNextDueUs_);  // first fragment goes immediately
  return QueueResult::Queued;
}

void EspNowCommandDispatcher::servicePacedSend(uint32_t nowUs) {
  if (outSent_ >= outCount_) {
    return;
  }
  if (static_cast<int32_t>(nowUs - outNextDueUs_) < 0) {
    return;
  }
  esp_now_send(outMac_, outFrags_[outSent_].bytes, outFrags_[outSent_].len);
  ++outSent_;
  outNextDueUs_ += outFragIntervalUs_;
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
  long durationMs = 0;
  jsonExtractInt(payloadJson, "duration_ms", durationMs);
  entry.responseTimeoutMs = kPostAckResponseTimeoutMs +
                            (durationMs > 0 ? static_cast<uint32_t>(durationMs) : 0);
  Serial.printf("[cmd_dispatch] send device_uid=%s mac=%02X:%02X:%02X:%02X:%02X:%02X cmd=%s "
                "payload_len=%u\n",
                deviceUid.c_str(), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], command.c_str(),
                static_cast<unsigned>(payloadJson.length()));
  const QueueResult queued = queueFragmentsTo(mac, payloadJson);
  if (queued == QueueResult::TooLarge) {
    pending_[freeIdx] = PendingCommand();
    rejectImmediately(deviceUid, requestId, command, "command_too_large");
    return;
  }
  if (queued == QueueResult::Busy) {
    // Deferred behind another device's burst. Backdate it so service()
    // sends it on the next tick rather than waiting a whole resend
    // interval, and do not count the attempt -- nothing went out.
    entry.attempts = 0;
    entry.lastSentMs = millis() - kResendIntervalMs;
  }
}

void EspNowCommandDispatcher::service() {
  // Drain any burst still going out before considering a resend: starting a
  // second burst on top of an unfinished one is exactly the pile-up the
  // pacing exists to avoid.
  servicePacedSend(micros());
  if (outSent_ < outCount_) {
    return;
  }

  const uint32_t now = millis();
  for (uint8_t i = 0; i < kEspNowCommandMaxPending; ++i) {
    PendingCommand& entry = pending_[i];
    if (!entry.used) continue;
    if (entry.acked) {
      // Delivery confirmed -- never resend the raw command again, just
      // wait out a separate, longer deadline for the response itself.
      if (now - entry.ackedMs >= entry.responseTimeoutMs) {
        failPending(i, "command_delivery_timeout");
      }
      continue;
    }
    if (entry.attempts >= kMaxAttempts) {
      failPending(i, entry.paging ? "result_page_timeout" : "command_delivery_timeout");
      continue;
    }
    if (now - entry.lastSentMs >= kResendIntervalMs) {
      if (queueFragmentsTo(entry.mac, entry.payloadJson) != QueueResult::Queued) {
        break;  // radio busy with another burst; try again next tick
      }
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
    // A page ask is not acked: it is cheap and safe to repeat, so it is
    // simply re-sent until its page arrives. That also keeps a late ack
    // for an earlier send from passing for this one's.
    if (entry.used && !entry.acked && !entry.paging && memcmp(entry.mac, mac, 6) == 0) {
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

  // Copied before anything else: once the next page is asked for, the
  // device may overwrite the frame buffer `data` points into.
  const String response(reinterpret_cast<const char*>(data), len);
  if (response.startsWith(kPagePrefix)) {
    handlePage(static_cast<uint8_t>(idx), response);
    return;
  }
  // A plain reply while paging is the device turning a page ask down
  // (result_page_expired); it ends the request with that error.
  completePending(static_cast<uint8_t>(idx), response);
}

void EspNowCommandDispatcher::handlePage(uint8_t idx, const String& response) {
  PendingCommand& entry = pending_[idx];
  long rid = -1;
  long chunk = -1;
  long chunks = 0;
  jsonExtractInt(response, "rid", rid);
  jsonExtractInt(response, "chunk", chunk);
  jsonExtractInt(response, "chunks", chunks);
  if (!entry.paging) {
    if (chunk != 0 || chunks < 1) return;  // stray page from an older reply
    entry.paging = true;
    entry.pageRid = rid;
    entry.chunks = chunks;
    entry.pagingStartedMs = millis();
    Serial.printf("[cmd_dispatch] paged_reply device_uid=%s rid=%ld pages=%ld\n",
                  entry.deviceUid.c_str(), rid, chunks);
  } else if (rid != entry.pageRid || chunk != entry.nextChunk) {
    return;  // a repeat of a page already forwarded
  }

  const bool last = chunk + 1 >= entry.chunks;
  // Ask for the next page before forwarding this one: the device builds
  // and sends it while the (blocking) WSS send below runs.
  if (!last) {
    entry.nextChunk = chunk + 1;
    entry.payloadJson = pageRequestJson(entry.pageRid, entry.nextChunk);
    entry.acked = false;
    entry.attempts = 1;
    entry.lastSentMs = millis();
    if (queueFragmentsTo(entry.mac, entry.payloadJson) != QueueResult::Queued) {
      entry.attempts = 0;
      entry.lastSentMs = millis() - kResendIntervalMs;
    }
  }

  // The page goes up as the device wrote it; the Backend joins the
  // "data" slices by request_id and parses the result.
  String message = "{";
  bool first = true;
  jsonStringField(message, "device_uid", entry.deviceUid, first);
  jsonStringField(message, "request_id", entry.requestId, first);
  jsonStringField(message, "command", entry.command, first);
  jsonRawField(message, "page", response, first);
  message += "}";
  uplink_->sendDeviceMessage("result_chunk", entry.deviceUid, message);

  if (last) {
    Serial.printf("[cmd_dispatch] paged_reply_done device_uid=%s pages=%ld elapsed_ms=%u\n",
                  entry.deviceUid.c_str(), entry.chunks,
                  static_cast<unsigned>(millis() - entry.pagingStartedMs));
    pending_[idx] = PendingCommand();
  }
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
