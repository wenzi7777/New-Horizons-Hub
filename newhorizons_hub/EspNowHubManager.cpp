#include "EspNowHubManager.h"

#include <Arduino.h>
#include <cstring>
#include <esp_now.h>
#include <esp_wifi.h>

#include "Config.h"

namespace nhos {

namespace {
// NHO/Arduino/1's device UID sits at header byte 4 (magic=0-1, version=2,
// flags=3, uid=4-9) -- see PacketBuilder.cpp's build()/buildHeartbeat(),
// both of which lay out every frame this way. Reading it here means Step
// 4's command routing doesn't need to touch the already-validated
// HELLO/PAIRED handshake at all.
constexpr size_t kDeviceUidOffset = 4;

// EspNowFrame.h's fragment header layout: magic(1) + version(1) +
// frameType(1) + ... -- see EspNowFragHeaderLen's own comment there.
constexpr size_t kEspNowFragTypeOffset = 2;

// PHY rate for each paired device's ESP-NOW link. Mirrors
// EspNowPairing.cpp's own kEspNowPeerPhyMode/kEspNowPeerPhyRate on the
// device side -- see that file's comment for the real-hardware validation
// this value is based on (firmware/spikes/README.md in the NewHorizonsOS-OTA
// repo, "PHY rate" section, 2026-08-06). The data-carrying direction is
// device->Hub, so this side mainly governs the Hub's own POLL/PAIRED
// control traffic back to the device -- kept symmetric anyway since it's a
// cheap, non-fatal call.
constexpr wifi_phy_mode_t kEspNowPeerPhyMode = WIFI_PHY_MODE_HT20;
constexpr wifi_phy_rate_t kEspNowPeerPhyRate = WIFI_PHY_RATE_MCS3_LGI;
}  // namespace

bool EspNowHubManager::begin() {
  // Defensive: HT rates need 802.11n enabled on this interface. STA mode's
  // default protocol bitmask is expected to already include it, but set it
  // explicitly rather than have the per-peer rate-config call below fail
  // for an unobvious reason.
  const esp_err_t protoErr = esp_wifi_set_protocol(
      WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  if (protoErr != ESP_OK) {
    Serial.printf("[hub] esp_wifi_set_protocol -> %d\n", protoErr);
  }

  if (esp_now_init() != ESP_OK) {
    return false;
  }
  return true;
}

void EspNowHubManager::onFrameReady(HubFrameCallback callback, void* userData) {
  frameCallback_ = callback;
  frameCallbackUserData_ = userData;
}

void EspNowHubManager::onControlFrameReady(HubFrameCallback callback, void* userData) {
  controlFrameCallback_ = callback;
  controlFrameCallbackUserData_ = userData;
}

int EspNowHubManager::findOrCreateSlot(const uint8_t mac[6]) {
  for (uint8_t i = 0; i < kEspNowHubMaxDevices; ++i) {
    if (slots_[i].used && memcmp(slots_[i].mac, mac, 6) == 0) {
      return i;
    }
  }
  for (uint8_t i = 0; i < kEspNowHubMaxDevices; ++i) {
    if (!slots_[i].used) {
      slots_[i].used = true;
      memcpy(slots_[i].mac, mac, 6);
      return i;
    }
  }
  return -1;  // roster full
}

void EspNowHubManager::registerPeerIfNeeded(DeviceSlot& slot) {
  if (slot.registered) {
    // Already registered -- still ack, since the device may be re-sending
    // HELLO because it thinks it isn't registered (e.g. after a Hub
    // restart wiped the peer table but the device's own copy of "am I
    // registered" is stale -- observed during spike testing).
  } else {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, slot.mac, 6);
    peer.channel = 0;  // follow whatever channel the STA interface is on
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
      return;
    }
    slot.registered = true;

    esp_now_rate_config_t rateConfig = {};
    rateConfig.phymode = kEspNowPeerPhyMode;
    rateConfig.rate = kEspNowPeerPhyRate;
    rateConfig.ersu = false;
    rateConfig.dcm = false;
    const esp_err_t rateErr = esp_now_set_peer_rate_config(slot.mac, &rateConfig);
    if (rateErr != ESP_OK) {
      // Non-fatal by design -- falls back to the default ESP-NOW rate
      // rather than blocking this device's pairing.
      Serial.printf("[hub] esp_now_set_peer_rate_config -> %d, using default rate\n", rateErr);
    }
  }
  const uint8_t payload[1] = {kHubPairedMagic};
  esp_now_send(slot.mac, payload, sizeof(payload));
}

void EspNowHubManager::handleEspNowRecv(const uint8_t mac[6], const uint8_t* data,
                                         size_t len) {
  const int idx = findOrCreateSlot(mac);
  if (idx < 0) return;
  DeviceSlot& slot = slots_[idx];
  slot.lastSeenMs = millis();

  if (len == 1 && data[0] == kHubHelloMagic) {
    registerPeerIfNeeded(slot);
    return;
  }

  // Peek the frame-type byte (header offset 2 -- see EspNowFrame.h's
  // header layout comment) to route to the right reassembler *before*
  // feeding it any fragment, since onFragment() itself decides based on
  // whichever reassembler instance it's called on. See DeviceSlot's
  // controlReassembler comment for why this split exists.
  const bool isControlFragment = len > kEspNowFragTypeOffset && data[kEspNowFragTypeOffset] == kEspNowFragTypeControl;
  EspNowReassembler& reassembler = isControlFragment ? slot.controlReassembler : slot.reassembler;
  if (isControlFragment) {
    Serial.printf("[hub] control_fragment_rx idx=%d len=%u\n", idx, static_cast<unsigned>(len));
  }

  ReassembledFrame frame;
  if (reassembler.onFragment(data, len, &frame)) {
    if (frame.frameType == kEspNowFragTypeControl) {
      Serial.printf("[hub] control_frame_reassembled idx=%d len=%u\n", idx,
                    static_cast<unsigned>(frame.len));
      // Command-response traffic (EspNowCommandDispatcher), not sensor
      // data -- doesn't carry an NHO/Arduino/1 header, so device_uid
      // learning and the data-poll-cycle advance below don't apply here.
      if (frame.len <= sizeof(controlFrameBuffer_)) {
        memcpy(controlFrameBuffer_, frame.data, frame.len);
        controlFrameBufferLen_ = frame.len;
        controlFrameReadyDeviceIdx_ = static_cast<uint8_t>(idx);
        memcpy(controlFrameReadyMac_, slot.mac, 6);
        controlFrameReady_ = true;
      }
      return;
    }

    if (!slot.deviceUidKnown && frame.len >= kPacketHeaderLen) {
      memcpy(slot.deviceUid, frame.data + kDeviceUidOffset, 6);
      slot.deviceUidKnown = true;
    }
    if (frame.len <= sizeof(frameBuffer_)) {
      memcpy(frameBuffer_, frame.data, frame.len);
      frameBufferLen_ = frame.len;
      frameReadyDeviceIdx_ = static_cast<uint8_t>(idx);
      memcpy(frameReadyMac_, slot.mac, 6);
      frameReady_ = true;
    }
    // Event-driven advance: don't wait for a timer, move on to the next
    // device the instant the one we were waiting on finishes.
    if (awaitingIdx_ == idx) {
      pollNext();
    }
  }
}

void EspNowHubManager::pollNext() {
  awaitingIdx_ = -1;
  for (uint8_t tries = 0; tries < kEspNowHubMaxDevices; ++tries) {
    const uint8_t idx = pollCursor_;
    pollCursor_ = (pollCursor_ + 1) % kEspNowHubMaxDevices;
    if (slots_[idx].used && slots_[idx].registered) {
      sendPollTo(idx);
      return;
    }
  }
  // No registered devices -- idle. service() will keep retrying cheaply
  // until a HELLO registers someone.
}

void EspNowHubManager::sendPollTo(uint8_t idx) {
  const uint8_t payload[1] = {kHubPollMagic};
  esp_now_send(slots_[idx].mac, payload, sizeof(payload));
  awaitingIdx_ = idx;
  pollSentUs_ = micros();
}

void EspNowHubManager::service() {
  if (awaitingIdx_ >= 0) {
    const uint32_t elapsed = micros() - pollSentUs_;
    if (elapsed > kHubPollTimeoutUs) {
      pollNext();  // timeout fallback -- don't let one bad link stall everyone
    }
  } else {
    pollNext();
  }

  if (frameReady_ && frameCallback_ != nullptr) {
    frameCallback_(frameReadyDeviceIdx_, frameReadyMac_, frameBuffer_,
                    frameBufferLen_, frameCallbackUserData_);
    frameReady_ = false;
  }

  if (controlFrameReady_ && controlFrameCallback_ != nullptr) {
    controlFrameCallback_(controlFrameReadyDeviceIdx_, controlFrameReadyMac_,
                           controlFrameBuffer_, controlFrameBufferLen_,
                           controlFrameCallbackUserData_);
    controlFrameReady_ = false;
  }
}

bool EspNowHubManager::macForDeviceUid(const uint8_t uid[6], uint8_t outMac[6]) const {
  for (uint8_t i = 0; i < kEspNowHubMaxDevices; ++i) {
    const DeviceSlot& slot = slots_[i];
    if (slot.used && slot.deviceUidKnown && memcmp(slot.deviceUid, uid, 6) == 0) {
      memcpy(outMac, slot.mac, 6);
      return true;
    }
  }
  return false;
}

EspNowHubManager::PairedDeviceInfo EspNowHubManager::slotInfo(uint8_t index) const {
  PairedDeviceInfo info;
  if (index >= kEspNowHubMaxDevices) return info;
  const DeviceSlot& slot = slots_[index];
  info.used = slot.used;
  info.registered = slot.registered;
  memcpy(info.mac, slot.mac, 6);
  memcpy(info.deviceUid, slot.deviceUid, 6);
  info.deviceUidKnown = slot.deviceUidKnown;
  info.lastSeenMs = slot.lastSeenMs;
  return info;
}

uint8_t EspNowHubManager::registeredCount() const {
  uint8_t count = 0;
  for (uint8_t i = 0; i < kEspNowHubMaxDevices; ++i) {
    if (slots_[i].used && slots_[i].registered) ++count;
  }
  return count;
}

}  // namespace nhos
