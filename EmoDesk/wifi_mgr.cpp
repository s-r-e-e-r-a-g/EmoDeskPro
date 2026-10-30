#include "wifi_mgr.h"
#include "sys.h"
#include "settings.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <esp_wifi.h>

static DNSServer dns;
static volatile WifiState st = WF_NO_CREDS;
static volatile bool gotIp = false;
static volatile bool lostIp = false;
static volatile uint8_t lastReason = 0;
static bool ap = false;
static int netIdx = -1;           // index of the network being tried / used
static uint32_t attemptAt = 0, waitUntil = 0, offlineSince = 0, connectedAt = 0, apClosePending = 0;
static uint8_t fails = 0;
static char curSsid[33] = "";
static char reasonText[40] = "";

static const wifi_power_t TXP[] = {WIFI_POWER_8_5dBm, WIFI_POWER_11dBm, WIFI_POWER_15dBm, WIFI_POWER_19_5dBm};

static bool haveCreds() {
  for (int i = 0; i < MAX_WIFI; i++) if (S.wifi[i].ssid[0]) return true;
  return false;
}

static int nextNet(int from) {
  for (int k = 1; k <= MAX_WIFI; k++) {
    int i = (from + k + MAX_WIFI) % MAX_WIFI;
    if (S.wifi[i].ssid[0]) return i;
  }
  return -1;
}

static const char* reasonName(uint8_t r) {
  switch (r) {
    case WIFI_REASON_NO_AP_FOUND: return "network not found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "wrong password?";
    case WIFI_REASON_BEACON_TIMEOUT: return "signal lost";
    case WIFI_REASON_ASSOC_LEAVE: return "left";
    case WIFI_REASON_AUTH_EXPIRE: return "auth timeout";
    default: return "disconnected";
  }
}

static void onEvent(arduino_event_id_t ev, arduino_event_info_t info) {
  switch (ev) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP: gotIp = true; break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      lastReason = info.wifi_sta_disconnected.reason;
      lostIp = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP: lostIp = true; break;
    default: break;
  }
}

static void startAp() {
  if (ap) return;
  WiFi.mode(haveCreds() ? WIFI_AP_STA : WIFI_AP);
  WiFi.setTxPower(TXP[S.txPower & 3]);
  IPAddress apIP(192, 168, 4, 1);
  IPAddress netMsk(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  bool res = WiFi.softAP(SETUP_AP_SSID, nullptr, 1, 0, 4);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());
  ap = true;
  logf("[WIFI] setup hotspot '%s' at %s (success=%s, mode=%s, ch=%d)",
       SETUP_AP_SSID, WiFi.softAPIP().toString().c_str(),
       res ? "yes" : "NO", haveCreds() ? "AP+STA" : "AP", WiFi.channel());
}

static void stopAp() {
  if (!ap) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  ap = false;
  logf("[WIFI] setup hotspot closed");
}

static void attempt() {
  netIdx = nextNet(netIdx);
  if (netIdx < 0) { st = WF_NO_CREDS; return; }
  safeCopy(curSsid, S.wifi[netIdx].ssid, sizeof(curSsid));
  WiFi.disconnect(false, false);
  WiFi.setSleep(S.wifiSleep);
  WiFi.begin(S.wifi[netIdx].ssid, S.wifi[netIdx].pass);
  WiFi.setTxPower(TXP[S.txPower & 3]);
  attemptAt = millis();
  lastReason = 0;
  st = WF_CONNECTING;
  static const char* const TXN[] = {"8.5", "11", "15", "19.5"};
  logf("[WIFI] connecting to '%s' (attempt %u, tx %s dBm)", curSsid, fails + 1, TXN[S.txPower & 3]);
}

void wifiBegin() {
  WiFi.persistent(false);
  WiFi.onEvent(onEvent);
  WiFi.setHostname(S.host);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  offlineSince = millis();
  if (!haveCreds()) { st = WF_NO_CREDS; startAp(); return; }
  attempt();
}

void wifiReconnect() {
  fails = 0;
  netIdx = -1;
  WiFi.setHostname(S.host);
  if (!haveCreds()) { WiFi.disconnect(); st = WF_NO_CREDS; startAp(); return; }
  attempt();
}

static void onOnline() {
  st = WF_CONNECTED;
  fails = 0;
  connectedAt = millis();
  reasonText[0] = 0;
  MDNS.end();
  if (MDNS.begin(S.host)) MDNS.addService("http", "tcp", 80);
  configTzTime(S.tz, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  logf("[WIFI] online '%s' %s rssi %d", curSsid, WiFi.localIP().toString().c_str(), WiFi.RSSI());
  if (ap) apClosePending = millis();
}

void wifiLoop() {
  uint32_t now = millis();
  if (ap) dns.processNextRequest();

  if (gotIp) { gotIp = false; lostIp = false; onOnline(); }
  if (lostIp) {
    lostIp = false;
    if (st == WF_CONNECTED) {
      safeCopy(reasonText, reasonName(lastReason), sizeof(reasonText));
      logf("[WIFI] lost connection (%s, reason %u)", reasonText, lastReason);
      offlineSince = now;
      st = WF_WAITING;
      waitUntil = now + 1500;   // quick first retry, the AP may just have rebooted
    } else if (st == WF_CONNECTING && lastReason && lastReason != WIFI_REASON_ASSOC_LEAVE && now - attemptAt > 500) {
      // a definite failure: no need to wait for the timeout
      safeCopy(reasonText, reasonName(lastReason), sizeof(reasonText));
      attemptAt = now - 15000;
    }
  }

  switch (st) {
    case WF_CONNECTING:
      if (now - attemptAt > 15000) {
        fails++;
        logf("[WIFI] '%s' failed: %s", curSsid, reasonText[0] ? reasonText : "timeout");
        WiFi.disconnect(false, false);
        uint32_t back = min<uint32_t>(60000, 5000UL << min<uint8_t>(fails / 2, 4));
        st = WF_WAITING;
        waitUntil = now + back;
      }
      break;
    case WF_WAITING:
      // A phone joined the setup hotspot: hold still so the page stays reachable.
      if (ap && WiFi.softAPgetStationNum() > 0) { waitUntil = now + 5000; break; }
      if ((int32_t)(now - waitUntil) >= 0) attempt();
      break;
    case WF_CONNECTED:
      if (WiFi.status() != WL_CONNECTED && now - connectedAt > 3000) { lostIp = true; lastReason = 0; }
      break;
    default: break;
  }

  if (st != WF_CONNECTED && !ap && haveCreds() && now - offlineSince > 45000) startAp();
  // Close the hotspot once we've been online a while and nobody is using it.
  if (ap && st == WF_CONNECTED && apClosePending && now - apClosePending > 30000) {
    if (WiFi.softAPgetStationNum() == 0) { stopAp(); apClosePending = 0; }
    else apClosePending = now;
  }
}

bool wifiOnline() { return st == WF_CONNECTED; }
bool wifiApActive() { return ap; }
WifiState wifiState() { return st; }
const char* wifiSsid() { return curSsid; }
String wifiIp() { return st == WF_CONNECTED ? WiFi.localIP().toString() : (ap ? WiFi.softAPIP().toString() : String("")); }
int wifiRssi() { return st == WF_CONNECTED ? WiFi.RSSI() : 0; }
uint32_t wifiConnectedForS() { return st == WF_CONNECTED ? (millis() - connectedAt) / 1000 : 0; }
const char* wifiLastReason() { return reasonText; }

static bool scanning = false;
void wifiScanStart() {
  if (scanning) return;
  // Scanning while a connection attempt is running breaks the attempt.
  if (st == WF_CONNECTING) { st = WF_WAITING; waitUntil = millis() + 8000; WiFi.disconnect(false, false); }
  WiFi.scanDelete();
  scanning = WiFi.scanNetworks(true, false) == WIFI_SCAN_RUNNING;
}

int wifiScanResults(String& json) {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return -1;
  if (n < 0) return scanning ? -1 : -2;
  scanning = false;
  json = "[";
  int k = 0;
  for (int i = 0; i < n && k < 20; i++) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    bool dup = false;
    for (int j = 0; j < i; j++) if (WiFi.SSID(j) == ssid) { dup = true; break; }
    if (dup) continue;
    ssid.replace("\\", "\\\\");
    ssid.replace("\"", "\\\"");
    if (k++) json += ',';
    json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"open\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  json += "]";
  return k;
}
