#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

#include "ClockCore.h"

namespace nhos {

// The Hub's own wall clock, which it serves to paired devices over ESP-NOW
// (EspNowHubManager answers their TIME_REQ from it -- EspNowTimeWire.h).
//
// Sources, best first:
//   * SNTP, as soon as Wi-Fi is up. Re-synced every kHubNtpIntervalS: every
//     device behind this Hub inherits its drift, so the default 3 h would
//     let them all wander ~200 ms together.
//   * The Desktop backend, for Hubs on a LAN with no Internet route: a
//     `ping` over the uplink, answered by a `pong` carrying server_time_ms,
//     timed NTP-style from this side (offset = server - midpoint).
//
// Shares ClockCore (slewing, source priority, uncertainty) with the device
// firmware's ClockService, so both ends discipline time the same way.
// ClockCore is only touched from loop(); the SNTP callback (tcpip task)
// parks its reading in a mailbox.
class HubClock {
 public:
  static constexpr uint32_t kHubNtpIntervalS = 600;
  // How long SNTP gets after Wi-Fi comes up before the backend is asked.
  static constexpr uint32_t kSntpGraceMs = 120000;
  static constexpr uint32_t kBackendPingIntervalMs = 60000;
  // A backend round trip slower than this is too asymmetric to be useful.
  static constexpr int64_t kMaxBackendRttUs = 2000000;

  using PingSender = bool (*)(void* ctx);

  // Idempotent; call once Wi-Fi is connected.
  void beginSntp();
  void setPingSender(PingSender sender, void* ctx) {
    pingSender_ = sender;
    pingSenderCtx_ = ctx;
  }
  // From the uplink's pong handler; `t4MonoUs` stamped on receipt.
  void onBackendPong(int64_t serverTimeMs, int64_t t4MonoUs);
  void service(uint32_t nowMs);
  // `clock_sync` gateway command: re-ask both sources now.
  void requestSync();

  bool synced() const { return core_.synced(); }
  TimeSource source() const { return core_.source(); }
  int64_t epochUsAt(int64_t monoUs) const { return core_.epochUsAt(monoUs); }
  uint32_t uncertaintyMsAt(int64_t monoUs) const;
  String statusJson() const;

  static int64_t monoUs();
  // tcpip task (SNTP).
  void onSntpTime(int64_t epochUs, int64_t monoUs);

 private:
  bool sntpFresh(uint32_t nowMs) const;
  void sendPing(uint32_t nowMs);

  ClockCore core_;

  bool sntpStarted_ = false;
  uint32_t sntpStartedMs_ = 0;
  uint32_t sntpSyncs_ = 0;
  uint32_t lastSntpMs_ = 0;
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  volatile bool sntpPending_ = false;
  ClockSample sntpSample_;

  PingSender pingSender_ = nullptr;
  void* pingSenderCtx_ = nullptr;
  bool pingOutstanding_ = false;
  int64_t pingT1Us_ = 0;
  uint32_t lastPingMs_ = 0;
  bool pingNow_ = false;
  uint32_t backendSyncs_ = 0;
  int64_t lastBackendRttUs_ = -1;
};

}  // namespace nhos
