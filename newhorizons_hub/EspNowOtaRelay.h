#pragma once

// Hub-side OTA relay for ESP-NOW-only devices (see EspNowOtaReceiver.h in
// NewHorizonsOS-OTA for the device-side counterpart, and
// ~/.claude/plans/linked-strolling-puppy.md's "New Horizons Direct 模式下
// 的裝置 OTA" section for the full design). A device with no WiFi/internet
// access can't fetch its own OTA manifest/firmware -- this class does that
// on its behalf: (1) fetches a manifest URL the device asks for and
// forwards the raw JSON back, (2) once the device decides it needs an
// update, streams the firmware binary from GitHub and re-fragments it into
// kEspNowFragTypeOta chunks sent stop-and-wait with an explicit
// chunk_index/ack per chunk (mirrors EspNowCommandDispatcher's
// kResendIntervalMs/kMaxAttempts retry model).
//
// Deliberately global-single-session, not per-device: only one device's
// relay may be active Hub-wide at a time (a second request while one is in
// progress gets an immediate "relay_already_active" rejection). This
// bounds the Hub's extra resource cost (one HTTP connection, one
// in-flight chunk buffer, one ack-timing loop) to a fixed constant
// regardless of how many of the roster's devices need updating -- the
// explicit stability requirement this feature was built for. Relay
// chunks are sent as independent unicast esp_now_send() calls to the
// target device's MAC, completely outside EspNowHubManager's own
// sensor-data polling cursor (same coexistence pattern
// EspNowCommandDispatcher already uses, real-hardware validated) -- other
// devices' sensor polling is untouched while a relay is in progress.
//
// This never touches Update.* -- that targets the HUB's OWN app0/app1
// partitions (see OtaManager.h/.cpp) and reusing it here would corrupt
// the Hub's own OTA state. This class only reads HTTP bytes and
// re-fragments them; the DEVICE calls Update.write() on the bytes it
// receives, and is solely responsible for its own SHA256 verification --
// the Hub relay doesn't verify anything, it's a dumb byte pipe.

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "EspNowFrame.h"
#include "EspNowHubManager.h"

namespace nhos {

// 4-byte sub-header (chunk_index u16 + chunk_len u16) prepended before
// handing the record to EspNowFragmenter, so payload stays within
// kEspNowMaxFragCount*kEspNowFragMaxPayload (3840B) total.
constexpr size_t kOtaChunkSubHeaderLen = 4;
constexpr size_t kOtaChunkPayloadBytes =
    kEspNowDataFragCount * kEspNowFragMaxPayload - kOtaChunkSubHeaderLen;

// Mirrors EspNowCommandDispatcher's own resend model exactly (0.5s * 30
// attempts = 15s max time-to-timeout per chunk).
constexpr uint32_t kOtaChunkResendIntervalMs = 500;
constexpr uint8_t kOtaChunkMaxAttempts = 30;

// A max-size chunk record fragments into up to kEspNowMaxFragCount (16)
// ESP-NOW packets -- firing all of them back-to-back in one esp_now_send()
// burst is the exact ESP_ERR_ESPNOW_NO_MEM packet-drop bug this codebase
// already found and fixed twice elsewhere (EspNowStreamTransport.cpp's
// kSendWindowUs, EspNowPairing.cpp's responseFrags_/
// kEspNowResponseSendWindowUs) -- confirmed by real-hardware OTA relay
// testing 2026-08-06: an un-paced burst here made most 16-fragment chunks
// take many resend cycles to get through, and a 363-chunk relay that
// should take ~1-4 minutes took 17+ minutes and was still running.
// Fragment sends are paced across service() calls the same way, spreading
// this window across however many fragments the current chunk needs.
constexpr uint32_t kOtaChunkSendWindowUs = 15000;

class EspNowOtaRelay {
 public:
  void begin(EspNowHubManager* hubManager);

  // Wired (via the .ino's hub-request demux, see newhorizons_hub.ino) to
  // EspNowHubManager::onHubRequestFrameReady() for hub_req=="fetch_manifest".
  void handleFetchManifestRequest(const uint8_t mac[6], const String& manifestUrl);

  // Same demux, for hub_req=="ota_relay_start".
  void handleRelayStartRequest(const uint8_t mac[6], const String& url,
                                const String& sha256, size_t size);

  // Call every loop() iteration: drives chunk read/send/resend/timeout.
  // Each tick does at most one small (<=kOtaChunkPayloadBytes) HTTP read
  // and one esp_now_send() -- never blocks for the whole transfer, so it
  // can't starve EspNowHubManager's sensor-data servicing of other
  // devices. (Exception: handleRelayStartRequest() itself does a
  // one-time, bounded HTTP connect+first-read, matching this codebase's
  // existing precedent for one-off blocking waits, e.g.
  // WifiManager::connectStored()'s 8s connect loop.)
  void service();

  // Wired to EspNowHubManager::onOtaChunkAck().
  void handleChunkAck(const uint8_t mac[6], uint16_t chunkIndex);

  bool sessionActive() const { return active_; }

 private:
  void sendHubRequestReply(const uint8_t mac[6], const String& json);
  void sendCurrentChunk();
  void finishSession(const char* reason);
  bool readNextChunkFromHttp();

  EspNowHubManager* hubManager_ = nullptr;

  bool active_ = false;
  uint8_t targetMac_[6] = {0};
  String sourceUrl_;  // for logging only

  WiFiClientSecure client_;
  HTTPClient http_;
  bool httpOpen_ = false;
  int httpContentLength_ = 0;
  size_t httpBytesRead_ = 0;

  uint8_t chunkRecord_[kOtaChunkSubHeaderLen + kOtaChunkPayloadBytes];
  size_t chunkPayloadLen_ = 0;
  uint16_t currentChunkIndex_ = 0;
  uint16_t totalChunks_ = 0;
  bool chunkAwaitingAck_ = false;
  uint32_t chunkSentMs_ = 0;
  uint8_t chunkAttempts_ = 0;

  // Paced fragment-burst state for the current chunk (initial send or
  // resend) -- see kOtaChunkSendWindowUs. sendCurrentChunk() only
  // fragments the record and arms this; service() is what actually calls
  // esp_now_send(), one fragment per due tick.
  EspNowFragment chunkFrags_[kEspNowDataFragCount];
  uint8_t chunkFragCount_ = 0;
  uint8_t chunkFragsSent_ = 0;
  uint32_t chunkFragIntervalUs_ = 0;
  uint32_t chunkNextFragDueUs_ = 0;

  // Set by handleChunkAck() (raw ESP-NOW recv-callback context, small
  // stack -- no I/O allowed there, see its own comment) as a lightweight
  // flag; service() (main-loop context) is what actually reads the next
  // chunk from HTTP and calls esp_now_send().
  bool ackPendingAdvance_ = false;
};

}  // namespace nhos
