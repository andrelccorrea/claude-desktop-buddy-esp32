// ============================================================
// net_bridge.cpp — see net_bridge.h for the protocol.
// ============================================================
#include "net_bridge.h"
#include "wifi_creds.h"
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_random.h>
#include <mbedtls/md.h>

static WiFiServer s_server(NET_PORT);
static WiFiClient s_client;
static uint8_t  s_secret[32];
static uint8_t  s_key[32];
static char     s_nh[33], s_nb[33];
static uint32_t s_rxSeq, s_txSeq;
static uint32_t s_connectedMs, s_lastRxMs;
static bool     s_started = false;
// Handshake stages: waiting for hello, waiting for the host's mac, authenticated.
static enum { ST_NONE, ST_HELLO, ST_MAC, ST_AUTH } s_state = ST_NONE;
static char     s_line[1100];
static uint16_t s_len = 0;

static const uint32_t HANDSHAKE_MS = 3000;
// The host sends a heartbeat every 10 s; a silent client gives way to a new one.
static const uint32_t IDLE_MS = 30000;

static void hmac(const uint8_t* key, const char* msg, uint8_t out[32]) {
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, 32,
                  (const uint8_t*)msg, strlen(msg), out);
}

static void toHex(const uint8_t* b, size_t n, char* out) {
  static const char* d = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { out[2 * i] = d[b[i] >> 4]; out[2 * i + 1] = d[b[i] & 15]; }
  out[2 * n] = 0;
}

static bool fromHex(const char* s, uint8_t* out, size_t n) {
  if (strlen(s) != 2 * n) return false;
  for (size_t i = 0; i < n; i++) {
    char buf[3] = {s[2 * i], s[2 * i + 1], 0};
    char* end;
    out[i] = (uint8_t)strtoul(buf, &end, 16);
    if (*end) return false;
  }
  return true;
}

static bool sameHex(const char* a, const char* b, size_t n) {
  if (strlen(a) < n || strlen(b) < n) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < n; i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

// Pulls the string value of "key" out of a one-line JSON object.
static bool jsonStr(const char* line, const char* key, char* out, size_t cap) {
  char pat[16];
  snprintf(pat, sizeof(pat), "\"%s\":\"", key);
  const char* p = strstr(line, pat);
  if (!p) return false;
  p += strlen(pat);
  const char* e = strchr(p, '"');
  if (!e || (size_t)(e - p) >= cap) return false;
  memcpy(out, p, e - p);
  out[e - p] = 0;
  return true;
}

static void dropClient(const char* why) {
  if (s_client) s_client.stop();
  s_state = ST_NONE;
  s_len = 0;
  Serial.printf("[net] client dropped: %s\n", why);
}

static void lineMac(char dir, uint32_t seq, const char* json, char out[33]) {
  static char msg[1100];
  uint8_t mac[32];
  snprintf(msg, sizeof(msg), "%c|%lu|%s", dir, (unsigned long)seq, json);
  hmac(s_key, msg, mac);
  toHex(mac, 16, out);
}

bool netConfigured() {
  Preferences p;
  p.begin("net", true);
  bool ok = p.getString("ssid", "") != "" && p.getString("secret", "").length() == NET_SECRET_HEX_LEN;
  p.end();
  return ok;
}

void netProvision(const char* ssid, const char* pass, const char* secretHex) {
  Preferences p;
  p.begin("net", false);
  p.putString("ssid", ssid ? ssid : "");
  p.putString("pass", pass ? pass : "");
  p.putString("secret", secretHex ? secretHex : "");
  p.end();
}

void netInit() {
  char ssid[WIFI_SSID_LEN] = {0}, pass[WIFI_PASS_LEN] = {0}, hex[NET_SECRET_HEX_LEN + 1] = {0};
  wifiCredsLoad(ssid, pass);
  Preferences p;
  p.begin("net", true);
  p.getString("secret", hex, sizeof(hex));
  p.end();
  if (!fromHex(hex, s_secret, sizeof(s_secret))) return;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("claude-buddy");
  WiFi.setSleep(false);  // a sleeping radio adds ~100 ms to every tap
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, pass);
  s_started = true;
  Serial.printf("[net] joining '%s'\n", ssid);
}

static void handleLine(void (*onLine)(const char*)) {
  if (s_state == ST_HELLO) {
    if (!jsonStr(s_line, "hello", s_nh, sizeof(s_nh)) || strlen(s_nh) != 32) return dropClient("bad hello");
    uint8_t nb[16], mac[32];
    for (int i = 0; i < 16; i += 4) { uint32_t r = esp_random(); memcpy(nb + i, &r, 4); }
    toHex(nb, 16, s_nb);
    char msg[80], macHex[65];
    snprintf(msg, sizeof(msg), "board|%s|%s", s_nh, s_nb);
    hmac(s_secret, msg, mac);
    toHex(mac, 32, macHex);
    s_client.printf("{\"hello\":\"%s\",\"mac\":\"%s\"}\n", s_nb, macHex);
    s_state = ST_MAC;
    return;
  }
  if (s_state == ST_MAC) {
    char got[65], want[65], msg[80];
    uint8_t mac[32];
    snprintf(msg, sizeof(msg), "host|%s|%s", s_nh, s_nb);
    hmac(s_secret, msg, mac);
    toHex(mac, 32, want);
    if (!jsonStr(s_line, "mac", got, sizeof(got)) || !sameHex(got, want, 64)) return dropClient("bad mac");
    snprintf(msg, sizeof(msg), "key|%s|%s", s_nh, s_nb);
    hmac(s_secret, msg, s_key);
    s_rxSeq = s_txSeq = 0;
    s_state = ST_AUTH;
    Serial.printf("[net] host %s authenticated\n", s_client.remoteIP().toString().c_str());
    return;
  }
  // ST_AUTH: "<32 hex> <json>"
  if (s_len < 34 || s_line[32] != ' ') return dropClient("unframed line");
  char want[33];
  const char* json = s_line + 33;
  lineMac('h', s_rxSeq, json, want);
  if (!sameHex(s_line, want, 32)) return dropClient("bad line mac");
  s_rxSeq++;
  if (json[0] == '{') onLine(json);
}

void netPoll(void (*onLine)(const char*)) {
  if (!s_started) return;
  static bool wasUp = false;
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !wasUp) {
    s_server.begin();
    s_server.setNoDelay(true);
    if (MDNS.begin("claude-buddy")) MDNS.addService("claude-buddy", "tcp", NET_PORT);
    Serial.printf("[net] ip %s port %d\n", WiFi.localIP().toString().c_str(), NET_PORT);
  }
  wasUp = up;
  if (!up) return;

  uint32_t now = millis();
  WiFiClient incoming = s_server.available();
  if (incoming) {
    bool busy = s_client && s_client.connected() && s_state == ST_AUTH && now - s_lastRxMs < IDLE_MS;
    if (busy) {
      incoming.stop();
    } else {
      if (s_client) s_client.stop();
      s_client = incoming;
      s_client.setNoDelay(true);
      s_state = ST_HELLO;
      s_len = 0;
      s_connectedMs = s_lastRxMs = now;
    }
  }
  if (s_state == ST_NONE) return;
  if (!s_client.connected()) return dropClient("closed");
  if (s_state != ST_AUTH && now - s_connectedMs > HANDSHAKE_MS) return dropClient("handshake timeout");

  while (s_client.available()) {
    char c = s_client.read();
    s_lastRxMs = now;
    if (c == '\n' || c == '\r') {
      if (s_len) { s_line[s_len] = 0; handleLine(onLine); s_len = 0; }
      if (s_state == ST_NONE) return;
    } else if (s_len < sizeof(s_line) - 1) {
      s_line[s_len++] = c;
    } else {
      return dropClient("line too long");
    }
  }
}

void netWrite(const char* json) {
  if (s_state != ST_AUTH || !s_client.connected()) return;
  char mac[33];
  lineMac('b', s_txSeq++, json, mac);
  s_client.printf("%s %s\n", mac, json);
}

bool netActive() { return s_state == ST_AUTH && s_client.connected(); }
