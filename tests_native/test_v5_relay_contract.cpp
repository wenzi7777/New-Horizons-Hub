// Host-native regression for the Hub's opaque ESP-NOW data relay.  This
// deliberately exercises only EspNowFrame: Hub-specific Arduino callbacks
// forward the reassembled byte buffer without parsing its body.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../newhorizons_hub/EspNowFrame.h"

namespace {

int gFailures = 0;

#define CHECK(condition)                                                    \
  do {                                                                      \
    if (!(condition)) {                                                     \
      std::fprintf(stderr, "FAIL %s:%d: %s\\n", __FILE__, __LINE__, #condition); \
      ++gFailures;                                                          \
    }                                                                       \
  } while (0)

constexpr size_t kPacketHeaderLen = 24;
constexpr size_t kUidOffset = 4;
constexpr size_t kMatrixCells = 14 * 14;
constexpr size_t kMatrixBytes = kMatrixCells * sizeof(float);
constexpr size_t kMagBytes = 3 * sizeof(float);
constexpr size_t kV5BatteryBytes = 6;
constexpr uint8_t kPacketFlagBattery = 0x02;
constexpr uint8_t kPacketFlagMag = 0x04;
constexpr uint8_t kPacketFlagExtensions = 0x20;

void putU16(std::vector<uint8_t>& data, size_t offset, uint16_t value) {
  data[offset] = static_cast<uint8_t>(value & 0xff);
  data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void putU32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
  for (size_t i = 0; i < sizeof(value); ++i) {
    data[offset + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xff);
  }
}

void putU64(std::vector<uint8_t>& data, size_t offset, uint64_t value) {
  for (size_t i = 0; i < sizeof(value); ++i) {
    data[offset + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xff);
  }
}

std::vector<uint8_t> makeV5MaximumDataFrame() {
  // Task 2 keeps v5 inside the Hub's 16 * 240B data-frame budget.  Fill the
  // remaining budget with syntactically valid TLVs so this catches both a
  // future buffer shrink and any accidental body-layout assumption.
  std::vector<uint8_t> packet(nhos::kEspNowDataFrameBytes, 0);
  putU16(packet, 0, 0xA55A);
  packet[2] = 5;
  packet[3] = kPacketFlagMag | kPacketFlagBattery | kPacketFlagExtensions;
  const uint8_t uid[6] = {0x3C, 0xDC, 0x75, 0x45, 0xCC, 0xD0};
  std::memcpy(packet.data() + kUidOffset, uid, sizeof(uid));
  putU32(packet, 10, 0x11223344);
  putU64(packet, 14, 1700000000123ULL);
  putU16(packet, 22, static_cast<uint16_t>(packet.size() - kPacketHeaderLen));

  size_t offset = kPacketHeaderLen;
  for (size_t i = 0; i < kMatrixCells; ++i) {
    putU32(packet, offset, 0x3F000000u + static_cast<uint32_t>(i));
    offset += sizeof(float);
  }
  for (size_t i = 0; i < kMagBytes; ++i) packet[offset++] = static_cast<uint8_t>(0xA0 + i);
  packet[offset++] = 1;  // battery status
  packet[offset++] = 2;  // battery fault
  putU16(packet, offset, 4175);
  offset += 2;
  putU16(packet, offset, 7350);
  offset += 2;
  assert(offset == kPacketHeaderLen + kMatrixBytes + kMagBytes + kV5BatteryBytes);

  uint8_t type = 0x21;
  while (offset < packet.size()) {
    const size_t valueLen =
        (packet.size() - offset > 257) ? 255 : packet.size() - offset - 2;
    packet[offset++] = type++;
    packet[offset++] = static_cast<uint8_t>(valueLen);
    for (size_t i = 0; i < valueLen; ++i) {
      packet[offset++] = static_cast<uint8_t>(i ^ type);
    }
  }
  assert(offset == packet.size());
  return packet;
}

std::vector<uint8_t> makeV4Sample() {
  const size_t payloadLen = kMatrixBytes + kMagBytes + 4;
  std::vector<uint8_t> packet(kPacketHeaderLen + payloadLen, 0);
  putU16(packet, 0, 0xA55A);
  packet[2] = 4;
  packet[3] = kPacketFlagMag | kPacketFlagBattery;
  const uint8_t uid[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  std::memcpy(packet.data() + kUidOffset, uid, sizeof(uid));
  putU32(packet, 10, 9);
  putU64(packet, 14, 1700000000456ULL);
  putU16(packet, 22, static_cast<uint16_t>(payloadLen));
  for (size_t i = kPacketHeaderLen; i < packet.size(); ++i) {
    packet[i] = static_cast<uint8_t>((0x5A + i) & 0xff);
  }
  return packet;
}

void expectOpaqueDataRoundTrip(const std::vector<uint8_t>& packet, uint16_t frameId) {
  nhos::EspNowFragment fragments[nhos::kEspNowDataFragCount];
  const uint8_t count = nhos::EspNowFragmenter::fragment(
      packet.data(), packet.size(), frameId, nhos::kEspNowFragTypeData,
      fragments, nhos::kEspNowDataFragCount);
  CHECK(count > 0);
  for (uint8_t i = 0; i < count; ++i) {
    CHECK(fragments[i].len <= nhos::kEspNowMaxPacketBytes);
  }

  uint8_t scratch[nhos::kEspNowDataFrameBytes] = {};
  nhos::EspNowReassembler reassembler(scratch, sizeof(scratch));
  nhos::ReassembledFrame output;
  bool complete = false;
  for (uint8_t i = 0; i < count; ++i) {
    complete = reassembler.onFragment(fragments[i].bytes, fragments[i].len, &output);
    if (i + 1 < count) CHECK(!complete);
  }
  CHECK(complete);
  CHECK(output.frameType == nhos::kEspNowFragTypeData);
  CHECK(output.frameId == frameId);
  CHECK(output.len == packet.size());
  CHECK(std::memcmp(output.data, packet.data(), packet.size()) == 0);
}

void testV5MaximumFrameRelayIsByteExact() {
  const auto packet = makeV5MaximumDataFrame();
  CHECK(packet.size() == nhos::kEspNowDataFrameBytes);
  CHECK(packet[2] == 5);
  CHECK(std::memcmp(packet.data() + kUidOffset,
                    "\x3C\xDC\x75\x45\xCC\xD0", 6) == 0);
  expectOpaqueDataRoundTrip(packet, 0x5015);
}

void testV4FrameRelayRemainsByteExact() {
  const auto packet = makeV4Sample();
  CHECK(packet[2] == 4);
  expectOpaqueDataRoundTrip(packet, 0x4004);
}

}  // namespace

int main() {
  testV5MaximumFrameRelayIsByteExact();
  testV4FrameRelayRemainsByteExact();
  if (gFailures == 0) {
    std::puts("OK: Hub v5/v4 opaque relay contract passed");
    return 0;
  }
  std::fprintf(stderr, "%d relay contract test(s) failed\\n", gFailures);
  return 1;
}
