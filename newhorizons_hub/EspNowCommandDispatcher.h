#pragma once

// Hub-side half of the reverse command channel (Step 4). Ports
// New-Horizons-Gateway/newhorizons_gateway/udp_control.py's resend model to
// ESP-NOW: the backend's {"type":"command",...} arrives via
// HubUplinkClient::onCommand(), gets fragmented as a kEspNowFragTypeControl
// frame and unicast to the target device, resent every kResendIntervalMs
// until a response frame is reassembled (routed here via
// EspNowHubManager::onControlFrameReady()) or the retry budget is
// exhausted. The result is reported upstream via
// HubUplinkClient::sendDeviceMessage("result", ...), matching the shape
// New-Horizons-Gateway/newhorizons_gateway/main.py's handle_udp_control()
// builds for its own UDP path so the backend sees an identical result
// shape regardless of which transport a device is using.
//
// Only one command may be in flight per device at a time: unlike the UDP
// path (which gets an immediate {"type":"ack",...} plus a request_id-
// tagged result), ControlServer::serviceEspNowCommand()'s response has no
// request_id to disambiguate against (see its header comment) -- so a
// second concurrent dispatch for the same device couldn't be matched to
// the right response. A concurrent dispatch is rejected with an
// immediate error result instead of silently overwriting the first.

#include <Arduino.h>

#include "EspNowFrame.h"
#include "EspNowHubManager.h"
#include "HubUplinkClient.h"

namespace nhos {

constexpr uint8_t kEspNowCommandMaxPending = kEspNowHubMaxDevices;

class EspNowCommandDispatcher {
 public:
  void begin(EspNowHubManager* hubManager, HubUplinkClient* uplink);

  // Wired to HubUplinkClient::onCommand(). deviceUid is 12 uppercase hex
  // chars, no separators (ControlServer::deviceUidString()'s format).
  // payloadJson is forwarded to the device unmodified -- it already has
  // the shape ControlServer::serviceEspNowCommand() expects directly (a
  // "command" field plus params, no outer transport envelope).
  void sendCommand(const String& deviceUid, const String& payloadJson);

  // Call every loop() iteration: resends unanswered commands and times
  // out ones that have exhausted their retry budget.
  void service();

  // Wired to EspNowHubManager::onControlFrameReady().
  void handleControlResponse(uint8_t deviceIndex, const uint8_t mac[6],
                              const uint8_t* data, size_t len);

  // Wired to EspNowHubManager::onControlAck(). Marks the pending command
  // for this device as delivered -- service() stops resending the raw
  // command and switches to waiting out a separate, longer deadline for
  // the (possibly slow) response instead. See EspNowPairing.h's
  // kEspNowControlAckMagic comment for why this exists.
  void handleControlAck(const uint8_t mac[6]);

  // True if ANY device currently has a command in flight (sent, acked, or
  // awaiting response). Hub-side analog to
  // NewHorizonsOS-OTA/firmware/newhorizons_os/EspNowPairing.h's
  // hasPendingCommandWork(): wired into newhorizons_hub.ino's
  // onHubFrameReady() to pause sensor-data forwarding to the uplink for the
  // same airtime-contention reason -- except here the contention is the
  // Hub's own blocking WSS send (HubUplinkClient::sendSensorPacket())
  // stalling loop(), not a device's radio.
  bool hasPendingCommand() const;

 private:
  struct PendingCommand {
    bool used = false;
    uint8_t mac[6] = {0};
    String deviceUid;
    String requestId;
    String command;
    String payloadJson;
    uint32_t lastSentMs = 0;
    uint8_t attempts = 0;
    bool acked = false;
    uint32_t ackedMs = 0;
  };

  // Queues a fragment burst for paced transmission. Never sends the whole
  // burst in one go: esp_now_send()'s internal queue is small, and the
  // device side reports ~1-2 radio-level failures per 22 fragments when a
  // burst is not spread out (see EspNowPairing.h on the device). Commands
  // used to be single-fragment, so this never bit -- installing an app
  // package makes multi-fragment commands routine.
  // False when a burst is already draining and this one was NOT queued --
  // there is one outbound buffer because there is one radio, and
  // overwriting it would truncate the burst in flight.
  bool queueFragmentsTo(const uint8_t mac[6], const String& json);
  void servicePacedSend(uint32_t nowUs);
  void rejectImmediately(const String& deviceUid, const String& requestId,
                          const String& command, const char* message);
  void completePending(uint8_t idx, const String& response);
  void failPending(uint8_t idx, const char* message);

  EspNowHubManager* hubManager_ = nullptr;
  HubUplinkClient* uplink_ = nullptr;
  PendingCommand pending_[kEspNowCommandMaxPending];

  // One outbound burst at a time. Shared rather than per-device because the
  // radio is shared: only one fragment can be on the air regardless of how
  // many devices have a command pending. Mirrors
  // NewHorizonsOS-OTA/.../EspNowStreamTransport's pacing state machine.
  EspNowFragment outFrags_[kEspNowDataFragCount];
  uint8_t outMac_[6] = {0};
  uint8_t outCount_ = 0;
  uint8_t outSent_ = 0;
  uint32_t outFragIntervalUs_ = 0;
  uint32_t outNextDueUs_ = 0;
};

}  // namespace nhos
