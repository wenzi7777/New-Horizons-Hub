#pragma once

// Hub-side ESP-NOW device roster + event-driven polling. See the plan
// (~/.claude/plans/linked-strolling-puppy.md) for why this replaced a
// fixed-schedule polling design: on real hardware, advancing to the next
// device on a fixed timer (rather than on actual frame completion) let
// slow/retried polls desync the schedule and caused persistent collisions
// with 4 concurrent devices. This version advances the instant the
// currently-polled device's frame completes, with a timeout as a fallback
// so one unresponsive device can't stall the rest of the roster.
//
// Channel handling: this Hub also runs a WiFi STA connection (for its
// backend uplink, see HubUplinkClient), and ESP-NOW must ride the SAME
// channel the STA interface is associated on -- forcing a different fixed
// channel here would fight the STA connection. All peers are registered
// with channel=0 ("use current channel"), and this manager never calls
// esp_wifi_set_channel() itself. Devices don't join a WiFi AP at all, so
// they instead have to discover which channel the Hub landed on (see
// EspNowPairing.h in firmware/newhorizons_os/ for the device-side scan).

#include <cstddef>
#include <cstdint>

#include "EspNowFrame.h"

namespace nhos {

constexpr uint8_t kEspNowHubMaxDevices = 4;

// Control-message magic bytes, distinct from kEspNowFragMagic so they're
// never confused with data fragments at the demux layer.
constexpr uint8_t kHubHelloMagic = 0xE1;   // device -> Hub: "I exist, register me"
constexpr uint8_t kHubPairedMagic = 0xE3;  // Hub -> device: "registered"
constexpr uint8_t kHubPollMagic = 0xE2;    // Hub -> device: "your turn now"
// OTA chunk ack, device -> Hub: raw 3-byte packet {magic, chunkIndexLow,
// chunkIndexHigh} -- not routed through the fragment/reassembler machinery
// at all (mirrors kHubHelloMagic/kHubPollMagic's own single-packet
// messages), since it needs to be recognized before the len==1 HELLO
// check and the frame-type-byte-based reassembler routing below. Handled
// by EspNowOtaRelay, not this class -- see handleEspNowRecv().
constexpr uint8_t kOtaChunkAckMagic = 0xE6;
constexpr size_t kOtaChunkAckLen = 3;

// Small reassembler size for device-initiated hub-request traffic
// (fetch_manifest/ota_relay_start JSON, kEspNowFragTypeHubRequest) --
// deliberately much smaller than kEspNowMaxFragCount*kEspNowFragMaxPayload
// (3840B, sized for OTA chunks/command responses), since these payloads
// are just small JSON asks, not firmware chunks.
constexpr size_t kHubRequestBufferBytes = 1024;

// Fallback only -- NOT a designed-in per-device fps budget. See header
// comment: achieved fps is an emergent, measured property of this design.
constexpr uint32_t kHubPollTimeoutUs = 30000;  // 30ms

using HubFrameCallback = void (*)(uint8_t deviceIndex, const uint8_t mac[6],
                                   const uint8_t* data, size_t len,
                                   void* userData);

class EspNowHubManager {
 public:
  // Does not touch the WiFi channel (see header comment) -- caller must
  // have already brought up WiFi/ESP-NOW (esp_now_init()) before this.
  bool begin();

  // Invoked from service() (main-loop context, never from the ESP-NOW
  // recv callback) whenever a complete *data* frame (kEspNowFragTypeData --
  // sensor data) has been reassembled from a registered device.
  void onFrameReady(HubFrameCallback callback, void* userData);

  // Step 4: same as onFrameReady, but for complete *control* frames
  // (kEspNowFragTypeControl -- a device's response to a command sent by
  // EspNowCommandDispatcher). Kept as a separate callback/buffer from data
  // frames so a command response is never mistaken for sensor data and
  // forwarded upstream as one.
  void onControlFrameReady(HubFrameCallback callback, void* userData);

  // Device-initiated hub-request traffic (kEspNowFragTypeHubRequest --
  // fetch_manifest/ota_relay_start JSON asks). Kept as its own
  // callback/buffer/reassembler, not folded into onControlFrameReady,
  // since kEspNowFragTypeControl's existing reassembler is sized and
  // timed for Hub-initiated EspNowCommandDispatcher traffic -- see
  // EspNowFrame.h's kEspNowFragTypeHubRequest comment.
  void onHubRequestFrameReady(HubFrameCallback callback, void* userData);

  // OTA chunk ack (device -> Hub, raw 3-byte packet, not a reassembled
  // frame -- see kOtaChunkAckMagic). Wired to EspNowOtaRelay.
  using OtaAckCallback = void (*)(const uint8_t mac[6], uint16_t chunkIndex, void* userData);
  void onOtaChunkAck(OtaAckCallback callback, void* userData);

  // Call every loop() iteration: drives the poll-advance/timeout state
  // machine and dispatches any completed frame to the registered callback.
  void service();

  // Wired to the global esp_now_recv callback in newhorizons_hub.ino
  // (esp_now callbacks must be free functions). Keep this cheap -- it must
  // not do network I/O (no WebSocket calls), only reassembly + buffering;
  // see the .cpp for why.
  void handleEspNowRecv(const uint8_t mac[6], const uint8_t* data, size_t len);

  uint8_t registeredCount() const;

  // Step 4 (reverse command channel): looks up a paired device's MAC by
  // its device_uid, learned by peeking at byte 4-9 of the first
  // reassembled NHO/Arduino/1 frame from each device (that header layout
  // is stable -- see firmware/newhorizons_os/PacketBuilder.cpp -- so this
  // doesn't require any change to the already-validated HELLO/PAIRED
  // handshake). Returns false if this device_uid hasn't been seen yet.
  bool macForDeviceUid(const uint8_t uid[6], uint8_t outMac[6]) const;

  // Step 5b (DirectWebUI): read-only enumeration of the roster, including
  // unused slots (caller checks `used`) so indices stay stable across
  // calls within one loop() iteration.
  struct PairedDeviceInfo {
    bool used = false;
    bool registered = false;
    uint8_t mac[6] = {0};
    uint8_t deviceUid[6] = {0};
    bool deviceUidKnown = false;
    uint32_t lastSeenMs = 0;
  };
  static constexpr uint8_t maxDevices() { return kEspNowHubMaxDevices; }
  PairedDeviceInfo slotInfo(uint8_t index) const;

 private:
  struct DeviceSlot {
    DeviceSlot()
        : reassembler(scratch, sizeof(scratch)),
          controlReassembler(controlScratch, sizeof(controlScratch)),
          hubRequestReassembler(hubRequestScratch, sizeof(hubRequestScratch)) {}

    bool used = false;
    bool registered = false;
    uint8_t mac[6] = {0};
    uint8_t scratch[kEspNowMaxFragCount * kEspNowFragMaxPayload];
    EspNowReassembler reassembler;
    // Separate reassembler for kEspNowFragTypeControl frames (command
    // responses). Found on real hardware: a shared reassembler let a
    // device's continuous POLL-driven data frames interleave with its
    // control response, and EspNowReassembler::onFragment()'s "a new
    // frame's first fragment always abandons whatever's in flight" rule
    // (by design, for the lossy data path) would corrupt/drop the control
    // response mid-reassembly, causing every EspNowCommandDispatcher
    // request to time out even though the device was actually responding.
    uint8_t controlScratch[kEspNowMaxFragCount * kEspNowFragMaxPayload];
    EspNowReassembler controlReassembler;
    // Same reasoning again, third time: device-initiated hub-request
    // traffic (fetch_manifest/ota_relay_start) gets its own reassembler
    // so it can't interleave with either data or Hub-initiated control
    // traffic. Deliberately smaller buffer -- see kHubRequestBufferBytes.
    uint8_t hubRequestScratch[kHubRequestBufferBytes];
    EspNowReassembler hubRequestReassembler;
    uint32_t lastSeenMs = 0;
    uint8_t deviceUid[6] = {0};
    bool deviceUidKnown = false;
  };

  int findOrCreateSlot(const uint8_t mac[6]);
  void registerPeerIfNeeded(DeviceSlot& slot);
  void pollNext();
  void sendPollTo(uint8_t idx);

  DeviceSlot slots_[kEspNowHubMaxDevices];
  uint8_t pollCursor_ = 0;
  int8_t awaitingIdx_ = -1;
  uint32_t pollSentUs_ = 0;

  HubFrameCallback frameCallback_ = nullptr;
  void* frameCallbackUserData_ = nullptr;
  HubFrameCallback controlFrameCallback_ = nullptr;
  void* controlFrameCallbackUserData_ = nullptr;
  HubFrameCallback hubRequestFrameCallback_ = nullptr;
  void* hubRequestFrameCallbackUserData_ = nullptr;
  OtaAckCallback otaAckCallback_ = nullptr;
  void* otaAckCallbackUserData_ = nullptr;

  // A completed frame is buffered here (copied out of the reassembler's
  // scratch buffer, which is only valid until that device's next
  // fragment) until service() can safely hand it to frameCallback_ from
  // main-loop context. If a newer frame completes before the previous one
  // is drained, it overwrites the buffer -- sensor data is lossy by
  // design (same philosophy as MatrixScanner's drop-oldest queue and
  // EspNowReassembler's own "new frame abandons old" rule).
  bool frameReady_ = false;
  uint8_t frameReadyDeviceIdx_ = 0;
  uint8_t frameReadyMac_[6] = {0};
  uint8_t frameBuffer_[kEspNowMaxFragCount * kEspNowFragMaxPayload];
  size_t frameBufferLen_ = 0;

  // Same buffering scheme as above, but for control-type frames (device
  // command responses). Kept as a separate buffer/flag so a command
  // response can never overwrite -- or be overwritten by -- in-flight
  // sensor data; unlike data frames, control responses are NOT lossy by
  // design (EspNowCommandDispatcher relies on seeing every response to
  // match request_id), but with one dispatch in flight per device at a
  // time this single-slot buffer is sufficient.
  bool controlFrameReady_ = false;
  uint8_t controlFrameReadyDeviceIdx_ = 0;
  uint8_t controlFrameReadyMac_[6] = {0};
  uint8_t controlFrameBuffer_[kEspNowMaxFragCount * kEspNowFragMaxPayload];
  size_t controlFrameBufferLen_ = 0;

  // Same buffering scheme again, for device-initiated hub-request frames.
  bool hubRequestFrameReady_ = false;
  uint8_t hubRequestFrameReadyDeviceIdx_ = 0;
  uint8_t hubRequestFrameReadyMac_[6] = {0};
  uint8_t hubRequestFrameBuffer_[kHubRequestBufferBytes];
  size_t hubRequestFrameBufferLen_ = 0;
};

}  // namespace nhos
