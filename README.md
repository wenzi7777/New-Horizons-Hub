# New Horizons Hub firmware

Arduino sketch for the "New Horizons Direct" Hub: a repurposed ESP32-S3
board (GCU V2.3.D, 4MB flash, no PSRAM) that relays ESP-NOW-paired New
Horizons wearable devices to a New Horizons Desktop Backend over WiFi,
without needing a New Horizons Gateway software instance on the LAN.

This repo used to live inside `NewHorizonsOS-OTA/firmware/newhorizons_hub/`
but was split out on its own once the Hub's feature set diverged enough to
be a genuinely separate product (own OTA release line, own version
numbering, own control protocol) rather than a build variant of the device
firmware. The Hub doesn't need `MatrixScanner`/`Calibration`/`ImuManager`/
`DisplayManager` from the device firmware, and pulling that code (and its
RAM use) into a no-PSRAM board is unnecessary risk.

## Shared files (manually synced copies, not symlinks)

`Config.h`, `BoardConfig.h`, `Storage.h/.cpp`, `WifiManager.h/.cpp`,
`LedController.h/.cpp`, `BoardPins.h/.cpp`, `EspNowFrame.h/.cpp`,
`JsonUtils.h/.cpp`, `OtaManager.h/.cpp` started as copies of the same files
in `NewHorizonsOS-OTA/firmware/newhorizons_os/`. Since this Hub firmware now
lives in a separate repo from the device firmware, there is no
build-system or git mechanism keeping them in sync at all — **if you change
the shared protocol/board logic in `NewHorizonsOS-OTA`, re-copy the
relevant file here too** (or vice versa), by hand. `Config.h`'s
`kHardwareModel` is the one deliberate exception: this repo's copy hardcodes
a `" (Hub)"` suffix so `OtaManager::parseManifest()`'s `model_mismatch`
check can't be fooled by a manifest URL pointed at the wrong repo/product,
even though the Hub and a device can be the exact same physical board
(`NHOS_BOARD_NAME`). (`LedController.h`'s `LedSignal` enum also carries a
Hub-only `UplinkDegraded` value that the device firmware's copy never uses
— kept in sync anyway rather than forking the enum.)

## Hub-specific modules

- `EspNowHubManager.h/.cpp` — ESP-NOW device roster + event-driven polling
  (HELLO/PAIRED/POLL protocol; Hub advances to the next device on
  frame-completion-or-timeout, not a fixed schedule). Also extracts each
  device's device_uid from its data frames' header, and separately routes
  `kEspNowFragTypeControl` frames (command responses) to a dedicated
  callback so they're never mistaken for sensor data.
- `HubUplinkClient.h/.cpp` — WiFi STA + `WebSocketsClient` (TLS), ports
  `New-Horizons-Gateway/newhorizons_gateway/upstream_wss.py`'s protocol
  behavior to embedded C++, including inbound `{"type":"command",...}`
  parsing (device-targeted) and `{"type":"gateway_command",...}` parsing
  (Hub-itself-targeted, `set_config`/`factory_reset` only — see "Hub
  control model" below) plus `sendDeviceMessage()`/
  `sendGatewayCommandResult()` for the respective replies. `gateway_status`
  heartbeat payload also carries `paired_devices` (device_uid/mac/status,
  30s staleness-filtered) and `target_mode`/`server_url` (so the Desktop
  UI's Manage Hub panel can show/pre-fill this Hub's actual current
  settings, not just whatever it was last told) for the Desktop UI.
- `EspNowCommandDispatcher.h/.cpp` — Hub-side half of the reverse command
  channel: fragments/resends a backend command to a device over ESP-NOW
  until a response arrives or the retry budget is exhausted, ports
  `New-Horizons-Gateway/newhorizons_gateway/udp_control.py`'s resend model.
  Only one command may be in flight per device (see the header comment for
  why).
- `HubConfig.h/.cpp` — NVS-backed `gateway_id`/`target_mode`/`manual_url`/
  `auth_token`.
- `HubConfigPortal.h/.cpp` — SoftAP + captive-portal setup form. Collects
  Wi-Fi credentials plus the connectivity fields needed to reach *some*
  Backend (`target_mode`/`manual_url`/`auth_token`) — `gateway_id` is
  auto-generated (`nh-hub-XXXXXX`), not typed, since renaming is a Desktop
  concern once the Hub is online (see "Hub control model" below). This is
  the **only** WebServer the Hub ever runs — it does not persist into
  normal operation.
- `newhorizons_hub.ino` — wiring. Boots straight into `HubConfigPortal` if
  Wi-Fi credentials aren't set yet (`HubConfigPortal`'s single form writes
  both Wi-Fi and Hub settings together, so this one check gates both);
  otherwise normal operation with no WebServer at all. Also owns
  `handleGatewayCommand()` (`set_config`/`factory_reset` handlers) and
  `buildPairedDevicesDetailJson()` (the heartbeat's display-only paired
  list, distinct from `buildPairedDevicesJson()`'s Backend-routing list).

### Hub control model

**There is no persistent Hub WebUI.** An earlier `DirectWebUI.h/.cpp` +
`LanDeviceScanner.h/.cpp` (an always-on management page + Backend-relayed
LAN scanner) were tried and then removed: a no-PSRAM board doesn't need a
second full `WebServer`/route table running for the Hub's entire uptime
alongside `HubUplinkClient`'s WS/TLS connection, just to duplicate what that
connection can already carry. All ongoing Hub management happens from the
Desktop app's Relays page → a Hub card's "Manage" button, which drives:

- **Status / paired-devices display** — read straight off the existing
  `gateway_status` heartbeat (`channel`/`mac`/`ip`/`target_mode`/
  `server_url`/`paired_devices`), no round trip needed.
- **LAN device scan / migrate-to-Hub** — Desktop calls the Backend's
  `GET /api/gateways/<id>/lan-devices` / `POST /api/gateways/<id>/migrate-device`
  directly (`NewHorizonsService.list_hub_lan_devices()`/`migrate_device_to_hub()`).
  Neither needs to reach the physical Hub at all: the device list is
  already the Backend's own, and migration is just two ordinary
  `publish_command()` calls (`set_transport`+`reboot`) to the *device*,
  the same path `/api/device-command` uses.
- **"取消" (ESP-NOW → back to Gateway)** — also just `/api/device-command`
  with `set_transport(mode=wifi_udp)`+`reboot`, same as any other device
  command. Nothing Hub-specific.
- **`set_config` / `factory_reset`** — the only two operations that
  genuinely must reach the Hub process itself (only it can rewrite its own
  NVS and reboot). These go over a parallel message pair —
  `gateway_command` (Backend→Hub) / `gateway_command_result` (Hub→Backend)
  — deliberately **not** the same `command`/`device_result` pair devices
  use, because that pair (and every routing table behind
  `publish_command()`) is hard-wired to a `device_uid` target; a Hub has no
  such id to hand it. `service.py::publish_gateway_command()` sends
  through `_gateway_session_senders[gateway_id]` (the same map already
  used for `gateway_claim_update`); `HubUplinkClient::onGatewayCommand()`
  routes the inbound side straight to `newhorizons_hub.ino`'s
  `handleGatewayCommand()`.

**Known, documented limitation**: anyone who can reach the Desktop
Backend's Manage Hub panel can reconfigure or factory-reset any Hub — there
is no per-Hub access control today, matching the existing Gateway WebUI's
posture. First-party accepted tradeoff, not an oversight.

## Real-hardware validation

Every module above (event-driven ESP-NOW polling, WS uplink, reverse
command channel, config portal, `gateway_command` protocol, OTA) has been
validated end to end on real hardware (GCU V2.3.D Hub + a real wearable
device board), including the WebUI-removal round's `set_config`/
`factory_reset`/LAN-scan/paired-devices-heartbeat verification. See
`~/.claude/plans/linked-strolling-puppy.md` (development history document,
not part of this repo) for the full dated account, including every real bug
found and fixed along the way — nothing here was validated only in
simulation.

**Not yet validated**: 2–4 devices connected to one Hub simultaneously
(needs device boards the original developer didn't have on hand yet), and
multi-Hub ESP-NOW channel collision handling in practice (the Hub reports
its actual WiFi channel so the Backend can *detect* a same-environment
collision, but cannot assign a different one — `esp_wifi_set_channel()` is
a no-op once a Hub's WiFi STA is associated to a real access point, since
channel is dictated by the AP).

## Recovery: falling back to wifi_udp/Gateway

In rough order of "try this first":

1. **Move one device back to `wifi_udp` (Gateway) via Desktop.** Relays
   page → the device's own settings, or `POST /api/device-command` with
   `{"command":"set_transport","mode":"wifi_udp"}` then `{"command":"reboot"}`.
   Works over whatever transport the device currently has (ESP-NOW-via-Hub
   or direct), no Hub-specific plumbing involved.
2. **Change the Hub's own settings (rename, switch target_mode/manual_url,
   or factory-reset) via Desktop.** Relays page → the Hub's card → "Manage"
   → Hub Settings form (`set_config`) or the Factory Reset button. Both
   require the Hub's WS uplink to be connected — if the Hub's uplink is
   down, use option 3 instead.
3. **Hub forgot its Wi-Fi/Hub config, or won't come up correctly, or its
   uplink can't be reached from Desktop at all**: it should fall back into
   `HubConfigPortal` on its own (see `newhorizons_hub.ino` -- boots
   straight into the portal whenever `wifi_ssid` isn't set). If it doesn't
   and you need to force it, use the portal's own Factory Reset button
   once you're connected to its SoftAP -- this clears `wifi_ssid`/
   `wifi_pass` and the four `HubConfig` NVS fields, then reboots straight
   back into setup. This needs zero Backend reachability, but only helps
   if the Hub is already showing the portal (i.e. `wifi_ssid` really is
   unset) -- if the stored Wi-Fi password is simply *wrong*, the Hub keeps
   retrying that network and never reaches the portal on its own; use
   option 4 in that case.
4. **Wi-Fi credentials are wrong (or you just need setup access and can't
   reach the Hub any other way) and there's no physical reset button** —
   the current GCU V2.3.D board has none; a future board revision adds
   one. Power the Hub off and back on 5 times in a row, each cycle within
   about 5 seconds of the previous power-on (real hardware, not scripted
   -- just flip the power quickly). This performs the exact same factory
   reset as the portal's own button above, then boots into
   `HubConfigPortal`. Tracked via a boot counter in NVS (survives real
   power loss, unlike RTC memory) -- see `newhorizons_hub.ino`'s
   `consumeQuickBootFactoryResetTrigger()`/`serviceQuickBootCounterClear()`.
   Real-hardware validated 2026-08-07.
5. **Last resort — full NVS wipe.**
   ```bash
   esptool.py --chip esp32s3 --port <port> erase_flash
   ```
   This clears everything (Wi-Fi credentials, `HubConfig`, the ESP-NOW peer
   table) -- the Hub will boot straight into `HubConfigPortal` as if brand
   new after reflashing. Only reach for this when the portal itself is
   unreachable or misbehaving; it's strictly more disruptive than option 3.

**Known limitation**: "Nearby LAN devices" scanning, "轉為本地配對" (LAN →
ESP-NOW migration), and `set_config`/`factory_reset` all depend on the
Backend being reachable — none of them depend on any direct Hub→device
WiFi/UDP path. If the Backend is down or the Hub's uplink is disconnected,
the only recovery path that still works is option 3 above (the Hub's own
SoftAP, which needs no Backend at all).

## Build

```bash
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=min_spiffs" \
  --build-property "build.extra_flags=-DNHOS_BOARD_GCU_V23D_LTS -DESP32=ESP32" \
  newhorizons_hub
```

Or via the release scripts (also builds the release `.bin` + OTA manifest):

```bash
VERSION=v0.1.0 scripts/build_release.sh
scripts/flash_firmware.sh /dev/cu.usbserial-10
```

Requires the `WebSockets` library (Links2004/arduinoWebSockets):
`arduino-cli lib install WebSockets`.

**Note on `-DESP32=ESP32`**: the `--build-property "build.extra_flags=..."`
pattern *replaces* the esp32 platform's own default `build.extra_flags`
rather than appending to it — which normally includes `-DESP32=ESP32` among
other defines. Nothing else in this sketch checks the bare `ESP32` macro
(only `ARDUINO_ARCH_ESP32`, which is unaffected), but the `WebSockets`
library's network-type auto-detection does check `defined(ESP32)`
specifically, so it's added back explicitly here. Worth knowing if you add
another library with the same expectation.
