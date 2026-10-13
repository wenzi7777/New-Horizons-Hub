// The Hub's half of the ESP-NOW time service, end to end on the host: a
// Hub clock disciplined by ClockCore answers a device's TIME_REQ the way
// EspNowHubManager::serviceTimeRequests() does, and the device recovers
// the Hub's wall time from the reply.
#include <cassert>
#include <cstdint>
#include <iostream>

#include "ClockCore.h"
#include "EspNowFrame.h"
#include "EspNowTimeWire.h"

using namespace nhos;

namespace {

constexpr int64_t kEpoch = 1760000000LL * 1000000LL;

void testDeviceRecoversHubTime() {
  // Hub booted 500 s ago; its monotonic clock and the device's are
  // unrelated, which is the whole point of the exchange.
  ClockCore hub;
  ClockSample sntp;
  sntp.source = TimeSource::Sntp;
  sntp.monoUs = 0;
  sntp.offsetUs = kEpoch;
  sntp.uncertaintyUs = 10000;
  hub.submit(sntp, 0);

  const int64_t hubMonoAtDeviceZero = 500000000;  // device mono 0 == Hub mono 500 s
  const int64_t oneWayUs = 1500;

  EspNowTimeRequest req;
  req.seq = 9;
  req.t1MonoUs = 20000000;
  uint8_t reqBytes[kEspNowTimeReqLen];
  assert(encodeEspNowTimeRequest(req, reqBytes, sizeof(reqBytes)) == kEspNowTimeReqLen);

  // Hub side.
  EspNowTimeRequest got;
  assert(decodeEspNowTimeRequest(reqBytes, sizeof(reqBytes), &got));
  const int64_t hubRx = req.t1MonoUs + hubMonoAtDeviceZero + oneWayUs;
  const int64_t hubTx = hubRx + 800;  // loop() turnaround
  EspNowTimeResponse resp;
  resp.seq = got.seq;
  resp.t1MonoUs = got.t1MonoUs;
  resp.hubSynced = hub.synced();
  resp.hubSource = static_cast<uint8_t>(hub.source());
  resp.t2EpochUs = hub.epochUsAt(hubRx);
  resp.t3EpochUs = hub.epochUsAt(hubTx);
  uint8_t respBytes[kEspNowTimeRespLen];
  assert(encodeEspNowTimeResponse(resp, respBytes, sizeof(respBytes)) == kEspNowTimeRespLen);

  // Device side.
  EspNowTimeResponse back;
  assert(decodeEspNowTimeResponse(respBytes, sizeof(respBytes), &back));
  assert(back.hubSynced);
  assert(back.hubSource == static_cast<uint8_t>(TimeSource::Sntp));
  const int64_t t4 = (hubTx - hubMonoAtDeviceZero) + oneWayUs;
  const EspNowTimeExchange x = computeEspNowTimeExchange(back.t1MonoUs, back.t2EpochUs, back.t3EpochUs, t4);
  assert(x.delayUs == 2 * oneWayUs);
  // Device wall time = device mono + offset must equal the Hub's.
  const int64_t deviceNow = t4;
  assert(deviceNow + x.offsetUs == hub.epochUsAt(deviceNow + hubMonoAtDeviceZero));
}

void testOldDeviceFirmwareCannotMistakeTheReply() {
  // A device predating the time service only knows one-byte handshakes and
  // fragments; a TIME_RESP is neither.
  uint8_t respBytes[kEspNowTimeRespLen];
  encodeEspNowTimeResponse(EspNowTimeResponse(), respBytes, sizeof(respBytes));
  assert(sizeof(respBytes) != 1);
  uint8_t scratch[256];
  EspNowReassembler reassembler(scratch, sizeof(scratch));
  ReassembledFrame frame;
  assert(!reassembler.onFragment(respBytes, sizeof(respBytes), &frame));
}

}  // namespace

int main() {
  testDeviceRecoversHubTime();
  testOldDeviceFirmwareCannotMistakeTheReply();
  std::cout << "Hub time service contract tests passed\n";
  return 0;
}
