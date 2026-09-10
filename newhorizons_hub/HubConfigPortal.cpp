#include "HubConfigPortal.h"

#include <WiFi.h>
#include <esp_mac.h>
#include <esp_random.h>

namespace nhos {

namespace {
// Distinct from firmware/newhorizons_os/Config.h's kDefaultApSsidPrefix/
// kSetupPortalDomain (NHOS / nhos.os) on purpose -- a Hub's setup AP should
// be visibly different from a device's, since a site could have both
// broadcasting at once. The prefixes differ from their first character
// (NHHub- vs NHOS-) so the two are told apart in a Wi-Fi picker at a glance.
constexpr char kHubApSsidPrefix[] = "NHHub";
constexpr char kHubSetupPortalDomain[] = "nhos.hub";
}  // namespace

void HubConfigPortal::begin(Storage* storage, HubConfig* hubConfig) {
  storage_ = storage;
  hubConfig_ = hubConfig;

  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char ssid[48];
  snprintf(ssid, sizeof(ssid), "%s-%02X%02X%02X%02X%02X%02X", kHubApSsidPrefix, mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);

  Serial.println(F("[hub_config_portal] starting"));
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(ssid);
  WiFi.AP.enableDhcpCaptivePortal();
  configureRoutes();
  dnsServer_.start(DNS_DEFAULT_PORT, "*", WiFi.softAPIP());
  portalServer_.begin();
  active_ = true;
  Serial.print(F("[hub_config_portal] started ssid="));
  Serial.print(ssid);
  Serial.print(F(" portal=http://"));
  Serial.println(kHubSetupPortalDomain);
}

void HubConfigPortal::service() {
  if (!active_) return;
  dnsServer_.processNextRequest();
  portalServer_.handleClient();
}

void HubConfigPortal::configureRoutes() {
  if (routesConfigured_) return;
  portalServer_.on("/", HTTP_GET, [this]() { handleRoot(); });
  portalServer_.on("/portal", HTTP_GET, [this]() { handleRoot(); });
  portalServer_.on("/save", HTTP_POST, [this]() { handleSave(); });
  portalServer_.on("/factory_reset", HTTP_POST, [this]() { handleFactoryReset(); });
  portalServer_.on("/generate_204", HTTP_GET, [this]() { handleRedirect(); });
  portalServer_.on("/gen_204", HTTP_GET, [this]() { handleRedirect(); });
  portalServer_.on("/hotspot-detect.html", HTTP_GET, [this]() { handleRoot(); });
  portalServer_.on("/connecttest.txt", HTTP_GET, [this]() { handleRedirect(); });
  portalServer_.on("/ncsi.txt", HTTP_GET, [this]() { handleRedirect(); });
  portalServer_.onNotFound([this]() { handleRedirect(); });
  routesConfigured_ = true;
}

void HubConfigPortal::handleRoot() {
  portalServer_.sendHeader("Cache-Control", "no-store");
  portalServer_.send(200, "text/html", page("", false));
}

void HubConfigPortal::handleSave() {
  String ssid = portalServer_.arg("ssid");
  String password = portalServer_.arg("password");
  ssid.trim();

  if (ssid.isEmpty()) {
    portalServer_.sendHeader("Cache-Control", "no-store");
    portalServer_.send(400, "text/html", page("Wi-Fi SSID is required.", false));
    return;
  }

  const String targetMode = portalServer_.arg("target_mode");
  const String manualUrl = portalServer_.arg("manual_url");
  const String authToken = portalServer_.arg("auth_token");
  UplinkTargetMode parsedMode;
  if (!HubConfig::parseTargetMode(targetMode, parsedMode)) {
    portalServer_.sendHeader("Cache-Control", "no-store");
    portalServer_.send(400, "text/html", page("Invalid backend target mode.", false));
    return;
  }
  if (parsedMode == UplinkTargetMode::Manual && manualUrl.isEmpty()) {
    portalServer_.sendHeader("Cache-Control", "no-store");
    portalServer_.send(400, "text/html", page("Manual URL is required for manual target mode.", false));
    return;
  }
  if (hubConfig_ == nullptr) {
    portalServer_.sendHeader("Cache-Control", "no-store");
    portalServer_.send(400, "text/html", page("Hub config unavailable.", false));
    return;
  }
  if (!hubConfig_->apply(*storage_, generateGatewayId(), targetMode, manualUrl, authToken)) {
    portalServer_.sendHeader("Cache-Control", "no-store");
    portalServer_.send(400, "text/html", page("Could not save Hub settings.", false));
    return;
  }

  // Written directly under the same keys WifiManager::applyCredentials()
  // would use -- newhorizons_hub.ino already established this convention,
  // and it means this portal doesn't need a WifiManager instance of its
  // own just to persist two strings.
  storage_->putString("wifi_ssid", ssid);
  storage_->putString("wifi_pass", password);

  portalServer_.sendHeader("Cache-Control", "no-store");
  portalServer_.send(200, "text/html", page("Saved. Rebooting...", true));
  delay(200);
  ESP.restart();
}

String HubConfigPortal::generateGatewayId() {
  // Mirrors the Desktop frontend's own Gateway-ID suggestion pattern
  // ('nh-hub-'+Math.random().toString(16).slice(2,8)) but generated here
  // in firmware since this form has no JS "Auto-generate" button to click
  // -- there's no field to click it into. `nh-hub-` (not Gateway
  // software's `nh-gateway-`) keeps the two client types visually
  // distinguishable in the Desktop UI's gateway_id column.
  char buf[16];
  snprintf(buf, sizeof(buf), "nh-hub-%06x", static_cast<unsigned>(esp_random() & 0xFFFFFFu));
  return String(buf);
}

void HubConfigPortal::handleFactoryReset() {
  storage_->putString("wifi_ssid", "");
  storage_->putString("wifi_pass", "");
  storage_->putString("hub_gateway_id", "");
  storage_->putString("hub_target_mode", "");
  storage_->putString("hub_manual_url", "");
  storage_->putString("hub_auth_token", "");
  portalServer_.sendHeader("Cache-Control", "no-store");
  portalServer_.send(200, "text/html", page("Factory reset done. Rebooting...", true));
  delay(200);
  ESP.restart();
}

void HubConfigPortal::handleRedirect() {
  portalServer_.sendHeader("Location", String("http://") + kHubSetupPortalDomain + "/portal", true);
  portalServer_.send(302, "text/plain", "redirect to New Horizons Hub setup");
}

String HubConfigPortal::page(const String& message, bool success) const {
  const String savedSsid = storage_ ? storage_->getString("wifi_ssid", "") : "";
  const String savedTargetMode = storage_ ? storage_->getString("hub_target_mode", "production") : "production";
  const String savedManualUrl = storage_ ? storage_->getString("hub_manual_url", "") : "";
  const String savedAuthToken = storage_ ? storage_->getString("hub_auth_token", "") : "";
  String out;
  out.reserve(2600);
  out += F("<!doctype html><html><head><meta charset=\"utf-8\">");
  out += F("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  out += F("<title>NHOS Hub Setup</title>");
  out += F("<style>body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;margin:0;background:#101417;color:#eef2f5}");
  out += F("main{max-width:440px;margin:0 auto;padding:28px 20px}h1{font-size:24px;margin:0 0 8px}");
  out += F("p{color:#b9c2ca;line-height:1.45}label{display:block;margin:16px 0 6px;color:#dce3e8}");
  out += F("input,select{box-sizing:border-box;width:100%;font-size:16px;padding:12px;border-radius:6px;border:1px solid #5f6b74;background:#151b20;color:#fff}");
  out += F("button{width:100%;margin-top:20px;padding:12px;font-size:16px;border:0;border-radius:6px;background:#2dd4bf;color:#041011;font-weight:700}");
  out += F(".reset{background:#3a2323;color:#f5b1b1;margin-top:10px}");
  out += F(".msg{padding:10px 12px;border-radius:6px;background:#1f2930}.ok{background:#12382f}</style></head><body><main>");
  out += F("<h1>NHOS Hub Setup</h1>");
  out += F("<p>Connect this Hub to your Wi-Fi and choose which Backend it reports to. "
           "A Gateway ID is generated automatically -- rename it or change these settings "
           "later from the Desktop app's Manage Hub panel, once this Hub is online.</p>");
  if (!message.isEmpty()) {
    out += success ? F("<p class=\"msg ok\">") : F("<p class=\"msg\">");
    out += htmlEscape(message);
    out += F("</p>");
  }
  out += F("<form method=\"post\" action=\"/save\">");
  out += F("<label for=\"ssid_select\">Nearby Wi-Fi</label>");
  out += F("<select id=\"ssid_select\" onchange=\"document.getElementById('ssid').value=this.value\">");
  out += F("<option value=\"\">Select a network or type manually</option>");
  out += wifiNetworkOptionsHtml();
  out += F("</select>");
  out += F("<label for=\"ssid\">Wi-Fi SSID</label>");
  out += F("<input id=\"ssid\" name=\"ssid\" autocomplete=\"off\" value=\"");
  out += htmlEscape(savedSsid);
  out += F("\" required>");
  out += F("<label for=\"password\">Wi-Fi Password</label>");
  out += F("<input id=\"password\" name=\"password\" type=\"password\" autocomplete=\"current-password\">");
  out += F("<label for=\"target_mode\">Backend</label>");
  out += F("<select id=\"target_mode\" name=\"target_mode\">");
  out += F("<option value=\"production\"");
  if (savedTargetMode == "production") out += F(" selected");
  out += F(">Production</option>");
  out += F("<option value=\"local\"");
  if (savedTargetMode == "local") out += F(" selected");
  out += F(">Local</option>");
  out += F("<option value=\"manual\"");
  if (savedTargetMode == "manual") out += F(" selected");
  out += F(">Manual</option>");
  out += F("</select>");
  out += F("<label for=\"manual_url\">Manual URL (only for Manual)</label>");
  out += F("<input id=\"manual_url\" name=\"manual_url\" autocomplete=\"off\" value=\"");
  out += htmlEscape(savedManualUrl);
  out += F("\">");
  out += F("<label for=\"auth_token\">Auth Token (optional)</label>");
  out += F("<input id=\"auth_token\" name=\"auth_token\" autocomplete=\"off\" value=\"");
  out += htmlEscape(savedAuthToken);
  out += F("\">");
  out += F("<button type=\"submit\">Save &amp; Reboot</button>");
  out += F("</form>");
  out += F("<form method=\"post\" action=\"/factory_reset\" onsubmit=\"return confirm('Erase all Hub settings and Wi-Fi credentials?');\">");
  out += F("<button type=\"submit\" class=\"reset\">Factory Reset</button>");
  out += F("</form>");
  out += F("<p>Setup AP: ");
  out += htmlEscape(WiFi.softAPSSID());
  out += F("<br>Manual URL: http://");
  out += kHubSetupPortalDomain;
  out += F("<br>Fallback: http://");
  out += WiFi.softAPIP().toString();
  out += F("</p></main></body></html>");
  return out;
}

String HubConfigPortal::wifiNetworkOptionsHtml() const {
  String out;
  int16_t count = WiFi.scanNetworks(false, true, false, 160);
  if (count <= 0) {
    out += F("<option value=\"\">No networks found</option>");
    WiFi.scanDelete();
    return out;
  }
  for (int16_t i = 0; i < count && i < 16; ++i) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;
    out += F("<option value=\"");
    out += htmlEscape(ssid);
    out += F("\">");
    out += htmlEscape(ssid);
    out += F(" (");
    out += WiFi.RSSI(i);
    out += F(" dBm, ");
    out += WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? F("open") : F("secure");
    out += F(")</option>");
  }
  WiFi.scanDelete();
  return out;
}

String HubConfigPortal::htmlEscape(const String& value) const {
  String out;
  out.reserve(value.length());
  for (size_t i = 0; i < value.length(); ++i) {
    char c = value.charAt(i);
    if (c == '&') {
      out += F("&amp;");
    } else if (c == '<') {
      out += F("&lt;");
    } else if (c == '>') {
      out += F("&gt;");
    } else if (c == '"') {
      out += F("&quot;");
    } else {
      out += c;
    }
  }
  return out;
}

}  // namespace nhos
