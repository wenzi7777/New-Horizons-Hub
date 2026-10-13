#include "HubClock.h"

#include <esp_sntp.h>
#include <esp_timer.h>
#include <sys/time.h>

namespace {
nhos::HubClock* g_hubClock = nullptr;
constexpr char kNtpServer[] = "pool.ntp.org";
constexpr char kFallbackNtpServer[] = "time.google.com";
// SNTP compensates its own round trip but reports no error estimate.
constexpr uint32_t kSntpUncertaintyUs = 10000;
}  // namespace

// Replaces ESP-IDF's weak default, which would settimeofday() -- a hard
// step -- on every sync. Runs on the tcpip task. Same hook as the device
// firmware's ClockSources.cpp.
extern "C" void sntp_sync_time(struct timeval* tv) {
  const int64_t monoUs = esp_timer_get_time();
  if (g_hubClock != nullptr && tv != nullptr) {
    g_hubClock->onSntpTime(static_cast<int64_t>(tv->tv_sec) * 1000000LL + tv->tv_usec, monoUs);
  }
  sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
}

namespace nhos {

int64_t HubClock::monoUs() { return esp_timer_get_time(); }

void HubClock::beginSntp() {
  if (sntpStarted_) {
    return;
  }
  sntpStarted_ = true;
  sntpStartedMs_ = millis();
  g_hubClock = this;
  sntp_set_sync_interval(kHubNtpIntervalS * 1000UL);
  configTime(0, 0, kNtpServer, kFallbackNtpServer);
}

void HubClock::onSntpTime(int64_t epochUs, int64_t mono) {
  ClockSample sample;
  sample.source = TimeSource::Sntp;
  sample.monoUs = mono;
  sample.offsetUs = epochUs - mono;
  sample.uncertaintyUs = kSntpUncertaintyUs;
  portENTER_CRITICAL(&lock_);
  sntpSample_ = sample;
  sntpPending_ = true;
  portEXIT_CRITICAL(&lock_);
}

void HubClock::onBackendPong(int64_t serverTimeMs, int64_t t4MonoUs) {
  if (!pingOutstanding_) {
    return;
  }
  pingOutstanding_ = false;
  const int64_t rtt = t4MonoUs - pingT1Us_;
  lastBackendRttUs_ = rtt;
  if (rtt < 0 || rtt > kMaxBackendRttUs) {
    return;
  }
  ClockSample sample;
  sample.source = TimeSource::Backend;
  // The server's reading belongs to the midpoint of the round trip; its
  // millisecond resolution adds up to 1 ms on top of the path asymmetry.
  sample.monoUs = pingT1Us_ + rtt / 2;
  sample.offsetUs = serverTimeMs * 1000LL - sample.monoUs;
  sample.uncertaintyUs = static_cast<uint32_t>(rtt / 2) + 1000;
  if (core_.submit(sample, monoUs()) != ClockSubmitResult::Ignored) {
    ++backendSyncs_;
  }
}

bool HubClock::sntpFresh(uint32_t nowMs) const {
  return sntpSyncs_ != 0 && nowMs - lastSntpMs_ < 2UL * kHubNtpIntervalS * 1000UL + 60000UL;
}

void HubClock::sendPing(uint32_t nowMs) {
  if (pingSender_ == nullptr) {
    return;
  }
  // Stamped before the send: the uplink's sendTXT() blocks on TLS, and
  // that time is part of the round trip either way.
  const int64_t t1 = monoUs();
  if (pingSender_(pingSenderCtx_)) {
    pingT1Us_ = t1;
    pingOutstanding_ = true;
  }
  lastPingMs_ = nowMs;
}

void HubClock::service(uint32_t nowMs) {
  if (sntpPending_) {
    ClockSample sample;
    portENTER_CRITICAL(&lock_);
    sample = sntpSample_;
    sntpPending_ = false;
    portEXIT_CRITICAL(&lock_);
    core_.submit(sample, monoUs());
    ++sntpSyncs_;
    lastSntpMs_ = nowMs;
  }

  if (pingNow_) {
    pingNow_ = false;
    sendPing(nowMs);
    return;
  }
  // The backend is only the fallback: asked once SNTP has had its chance
  // and is not delivering.
  if (!sntpStarted_ || sntpFresh(nowMs) || nowMs - sntpStartedMs_ < kSntpGraceMs) {
    return;
  }
  if (lastPingMs_ == 0 || nowMs - lastPingMs_ >= kBackendPingIntervalMs) {
    sendPing(nowMs);
  }
}

void HubClock::requestSync() {
  if (sntpStarted_) {
    sntp_restart();
  }
  pingNow_ = true;
}

uint32_t HubClock::uncertaintyMsAt(int64_t mono) const {
  const uint32_t us = core_.uncertaintyUsAt(mono);
  return us == UINT32_MAX ? UINT32_MAX : (us + 999) / 1000;
}

String HubClock::statusJson() const {
  const int64_t now = monoUs();
  const bool isSynced = core_.synced();
  const uint32_t nowMs = millis();
  String json = "{\"source\":\"";
  json += timeSourceName(core_.source());
  json += "\",\"synced\":";
  json += isSynced ? "true" : "false";
  json += ",\"epoch_ms\":";
  json += isSynced ? String(static_cast<unsigned long long>(core_.epochUsAt(now) / 1000)) : String("null");
  json += ",\"age_ms\":";
  json += isSynced ? String(static_cast<unsigned long>((now - core_.lastSyncMonoUs()) / 1000)) : String("null");
  json += ",\"uncertainty_ms\":";
  json += isSynced ? String(static_cast<unsigned long>(uncertaintyMsAt(now))) : String("null");
  json += ",\"last_adjust_ms\":";
  json += String(static_cast<long>(core_.lastAdjustUs() / 1000));
  json += ",\"adjust_count\":";
  json += String(static_cast<unsigned long>(core_.adjustCount()));
  json += ",\"step_count\":";
  json += String(static_cast<unsigned long>(core_.stepCount()));
  json += ",\"sntp\":{\"started\":";
  json += sntpStarted_ ? "true" : "false";
  json += ",\"server\":\"";
  json += kNtpServer;
  json += "\",\"interval_s\":";
  json += String(static_cast<unsigned long>(kHubNtpIntervalS));
  json += ",\"syncs\":";
  json += String(static_cast<unsigned long>(sntpSyncs_));
  json += ",\"age_ms\":";
  json += sntpSyncs_ != 0 ? String(nowMs - lastSntpMs_) : String("null");
  json += "},\"backend\":{\"syncs\":";
  json += String(static_cast<unsigned long>(backendSyncs_));
  json += ",\"last_rtt_ms\":";
  json += lastBackendRttUs_ >= 0 ? String(static_cast<long>(lastBackendRttUs_ / 1000)) : String("null");
  json += "}}";
  return json;
}

}  // namespace nhos
