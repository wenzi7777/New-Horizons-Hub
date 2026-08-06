#include "EspNowOtaRelay.h"

#include <cstring>

#include <esp_now.h>

#include "JsonUtils.h"

namespace nhos {

void EspNowOtaRelay::begin(EspNowHubManager* hubManager) {
  hubManager_ = hubManager;
}

void EspNowOtaRelay::sendHubRequestReply(const uint8_t mac[6], const String& json) {
  EspNowFragment frags[kEspNowMaxFragCount];
  const uint8_t count = EspNowFragmenter::fragment(
      reinterpret_cast<const uint8_t*>(json.c_str()), json.length(), 0,
      kEspNowFragTypeHubRequest, frags, kEspNowMaxFragCount);
  for (uint8_t i = 0; i < count; ++i) {
    esp_now_send(mac, frags[i].bytes, frags[i].len);
  }
}

void EspNowOtaRelay::handleFetchManifestRequest(const uint8_t mac[6], const String& manifestUrl) {
  // Small, one-shot GET -- same bounded-blocking precedent as
  // OtaManager::fetchManifest(), safe here since this runs from
  // EspNowHubManager::service()'s deferred hub-request dispatch (main
  // loop context), not the raw recv callback.
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  http.setConnectTimeout(6000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  String result = "{";
  bool first = true;
  jsonStringField(result, "hub_req", "fetch_manifest", first);
  if (!http.begin(client, manifestUrl)) {
    jsonBoolField(result, "ok", false, first);
    jsonStringField(result, "error", "manifest_http_begin_failed", first);
    result += "}";
    sendHubRequestReply(mac, result);
    return;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    jsonBoolField(result, "ok", false, first);
    jsonStringField(result, "error", "manifest_http_" + String(code), first);
    result += "}";
    http.end();
    sendHubRequestReply(mac, result);
    return;
  }
  const String payload = http.getString();
  http.end();
  jsonBoolField(result, "ok", true, first);
  jsonRawField(result, "manifest", payload, first);
  result += "}";
  Serial.printf("[ota_relay] fetch_manifest ok url=%s len=%u\n", manifestUrl.c_str(),
                static_cast<unsigned>(payload.length()));
  sendHubRequestReply(mac, result);
}

void EspNowOtaRelay::handleRelayStartRequest(const uint8_t mac[6], const String& url,
                                              const String& sha256, size_t size) {
  String result = "{";
  bool first = true;
  jsonStringField(result, "hub_req", "ota_relay_start", first);

  if (active_) {
    jsonBoolField(result, "ok", false, first);
    jsonStringField(result, "error", "relay_already_active", first);
    result += "}";
    sendHubRequestReply(mac, result);
    return;
  }
  if (size == 0) {
    jsonBoolField(result, "ok", false, first);
    jsonStringField(result, "error", "size_required", first);
    result += "}";
    sendHubRequestReply(mac, result);
    return;
  }

  client_.stop();
  client_.setInsecure();
  http_.setTimeout(12000);
  http_.setConnectTimeout(12000);
  http_.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http_.begin(client_, url)) {
    jsonBoolField(result, "ok", false, first);
    jsonStringField(result, "error", "firmware_http_begin_failed", first);
    result += "}";
    sendHubRequestReply(mac, result);
    return;
  }
  const int code = http_.GET();
  if (code != HTTP_CODE_OK) {
    jsonBoolField(result, "ok", false, first);
    jsonStringField(result, "error", "firmware_http_" + String(code), first);
    result += "}";
    http_.end();
    sendHubRequestReply(mac, result);
    return;
  }

  active_ = true;
  memcpy(targetMac_, mac, 6);
  sourceUrl_ = url;
  httpOpen_ = true;
  httpContentLength_ = http_.getSize();
  httpBytesRead_ = 0;
  currentChunkIndex_ = 0;
  totalChunks_ = static_cast<uint16_t>((size + kOtaChunkPayloadBytes - 1) / kOtaChunkPayloadBytes);
  chunkAwaitingAck_ = false;
  chunkAttempts_ = 0;

  jsonBoolField(result, "ok", true, first);
  jsonUnsignedField(result, "total_chunks", totalChunks_, first);
  jsonUnsignedField(result, "chunk_size", static_cast<unsigned long>(kOtaChunkPayloadBytes), first);
  result += "}";
  Serial.printf("[ota_relay] relay_start mac=%02X:%02X:%02X:%02X:%02X:%02X url=%s size=%u "
                "total_chunks=%u\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], url.c_str(),
                static_cast<unsigned>(size), totalChunks_);
  sendHubRequestReply(mac, result);

  // Prime the first chunk so service() has something to send on its next
  // tick -- this initial read is part of the same one-time, bounded
  // blocking window as the http_.begin()/GET() calls above (data should
  // already be arriving on a connection that just returned HTTP_CODE_OK).
  if (readNextChunkFromHttp()) {
    sendCurrentChunk();
  } else {
    finishSession("firmware_first_read_failed");
  }
}

bool EspNowOtaRelay::readNextChunkFromHttp() {
  NetworkClient* stream = http_.getStreamPtr();
  size_t got = 0;
  const uint32_t startMs = millis();
  // Bounded wait for at least one byte -- matches OtaManager's own
  // idle-timeout philosophy, just scoped to a single chunk instead of the
  // whole file, since this runs once per service() tick, not in a loop
  // spanning the entire transfer.
  while (got < kOtaChunkPayloadBytes) {
    if (!http_.connected() && stream->available() == 0) {
      break;  // connection closed, whatever we've read is the final partial chunk
    }
    size_t available = stream->available();
    if (available == 0) {
      if (millis() - startMs > 5000) break;  // this chunk's read timed out
      delay(1);
      continue;
    }
    if (available > kOtaChunkPayloadBytes - got) {
      available = kOtaChunkPayloadBytes - got;
    }
    const int readBytes = stream->readBytes(chunkRecord_ + kOtaChunkSubHeaderLen + got, available);
    if (readBytes <= 0) break;
    got += static_cast<size_t>(readBytes);
    httpBytesRead_ += static_cast<size_t>(readBytes);
  }
  if (got == 0) {
    return false;
  }
  chunkPayloadLen_ = got;
  chunkRecord_[0] = static_cast<uint8_t>(currentChunkIndex_ & 0xFF);
  chunkRecord_[1] = static_cast<uint8_t>((currentChunkIndex_ >> 8) & 0xFF);
  chunkRecord_[2] = static_cast<uint8_t>(got & 0xFF);
  chunkRecord_[3] = static_cast<uint8_t>((got >> 8) & 0xFF);
  return true;
}

void EspNowOtaRelay::sendCurrentChunk() {
  // Only fragments and arms the paced burst -- see chunkFrags_'s comment
  // in the header for why the actual esp_now_send() calls are spread
  // across service() ticks instead of firing here in one go.
  chunkFragCount_ = EspNowFragmenter::fragment(
      chunkRecord_, kOtaChunkSubHeaderLen + chunkPayloadLen_, currentChunkIndex_,
      kEspNowFragTypeOta, chunkFrags_, kEspNowMaxFragCount);
  chunkFragsSent_ = 0;
  chunkFragIntervalUs_ = chunkFragCount_ > 0 ? kOtaChunkSendWindowUs / chunkFragCount_ : 0;
  chunkNextFragDueUs_ = micros();
  chunkAwaitingAck_ = true;
  chunkSentMs_ = millis();
  ++chunkAttempts_;
}

void EspNowOtaRelay::finishSession(const char* reason) {
  Serial.printf("[ota_relay] session_end reason=%s chunk=%u/%u\n", reason,
                currentChunkIndex_, totalChunks_);
  if (httpOpen_) {
    http_.end();
    httpOpen_ = false;
  }
  active_ = false;
  chunkAwaitingAck_ = false;
  chunkFragsSent_ = 0;
  chunkFragCount_ = 0;
}

void EspNowOtaRelay::service() {
  if (!active_) return;

  if (chunkFragsSent_ < chunkFragCount_) {
    const uint32_t nowUs = micros();
    if (static_cast<int32_t>(nowUs - chunkNextFragDueUs_) >= 0) {
      esp_now_send(targetMac_, chunkFrags_[chunkFragsSent_].bytes, chunkFrags_[chunkFragsSent_].len);
      ++chunkFragsSent_;
      chunkNextFragDueUs_ += chunkFragIntervalUs_;
    }
    return;  // don't advance/resend-check until this chunk's burst is fully sent
  }

  if (ackPendingAdvance_) {
    // Deferred out of handleChunkAck() (raw recv-callback context) -- the
    // actual HTTP read + esp_now_send() for the next chunk happens here,
    // in main-loop context, same as OtaManager's own bounded-blocking
    // precedent.
    ackPendingAdvance_ = false;
    ++currentChunkIndex_;
    chunkAttempts_ = 0;
    if (currentChunkIndex_ >= totalChunks_) {
      finishSession("complete");
      return;
    }
    if (readNextChunkFromHttp()) {
      sendCurrentChunk();
    } else {
      finishSession("firmware_read_failed");
    }
    return;
  }

  if (chunkAwaitingAck_) {
    if (millis() - chunkSentMs_ >= kOtaChunkResendIntervalMs) {
      if (chunkAttempts_ >= kOtaChunkMaxAttempts) {
        finishSession("chunk_delivery_timeout");
        return;
      }
      sendCurrentChunk();  // resend the already-buffered chunk, no HTTP re-read
    }
  }
}

void EspNowOtaRelay::handleChunkAck(const uint8_t mac[6], uint16_t chunkIndex) {
  // Deliberately minimal: this fires from the raw ESP-NOW recv callback
  // (small stack, no I/O allowed -- see EspNowPairing.cpp's
  // handleControlFragment() comment for the stack-overflow bug this
  // exact pattern avoids). Only cheap state is touched here; the actual
  // HTTP read + esp_now_send() for the next chunk is deferred to
  // service() via ackPendingAdvance_.
  if (!active_ || memcmp(mac, targetMac_, 6) != 0 || chunkIndex != currentChunkIndex_) {
    return;  // stale/mismatched ack -- ignore
  }
  chunkAwaitingAck_ = false;
  ackPendingAdvance_ = true;
}

}  // namespace nhos
