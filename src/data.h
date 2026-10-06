#pragma once
#include "serial_ota.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include "ble_bridge.h"
#include "net_bridge.h"
#include "xfer.h"

struct TamaState {
  uint8_t  sessionsTotal;
  uint8_t  sessionsRunning;
  uint8_t  sessionsWaiting;
  bool     recentlyCompleted;
  uint32_t tokensToday;
  uint32_t lastUpdated;
  char     msg[24];
  bool     connected;
  char     lines[8][92];
  uint8_t  nLines;
  uint16_t lineGen;          // bumps when lines change — lets UI reset scroll
  char     promptId[40];     // pending permission request ID; empty = no prompt
  char     promptTool[20];
  char     promptHint[44];
  // Forward-compatible multi-choice question support. The desktop bridge
  // doesn't currently send `prompt.choices[]` over the wire, but the
  // parser below picks them up if it ever does — and demo mode injects
  // a fake one so the UI can be exercised today. promptChoiceN > 0
  // tells the UI to render the multi-choice modal instead of the
  // approve/deny screen.
  static const uint8_t MAX_CHOICES = 4;
  static const uint8_t CHOICE_LABEL_LEN = 24;
  uint8_t  promptChoiceN;
  char     promptChoiceLabels[MAX_CHOICES][CHOICE_LABEL_LEN];
  char     promptChoiceIds[MAX_CHOICES][16];
  // Most-recent assistant turn text — concatenation of `text` content blocks
  // from the latest `{"evt":"turn","role":"assistant",...}` event. Capped
  // and lossy by design; the screen can't display long replies anyway.
  char     lastTurnText[320];
  uint32_t lastTurnMs;       // millis() of the last assistant turn event
  uint16_t lastTurnGen;      // bumps each new assistant turn → UI scroll reset
  // Per-session detail from a host bridge's {"evt":"status"} line (model,
  // context use, branch...). Rate limits are account-wide; -1 = unknown.
  static const uint8_t MAX_SESS = 5;
  struct Sess {
    char    name[17];
    char    branch[13];
    char    model[15];
    char    effort[7];
    char    tokens[12];      // "84k/200k"
    uint8_t ctx;             // context window used, percent
    uint16_t mins;           // session wall-clock minutes
    char    state;           // 'r' running, 'w' waiting, 'i' idle
  } sess[MAX_SESS];
  uint8_t  nSess;
  int8_t   rl5h = -1, rl7d = -1;
};

// ---------------------------------------------------------------------------
// Three modes, checked in priority order:
//   demo   → auto-cycle fake scenarios every 8s, ignore live data
//   live   → JSON arrived in the last 10s over USB or BT
//   asleep → no data, all zeros, "No Claude connected"
// ---------------------------------------------------------------------------

static uint32_t _lastLiveMs = 0;
static uint32_t _lastBtByteMs = 0;   // hasClient() lies; track actual BT traffic
static bool     _demoMode   = false;
static uint8_t  _demoIdx    = 0;
static uint32_t _demoNext   = 0;

struct _Fake { const char* n; uint8_t t,r,w; bool c; uint32_t tok; };
static const _Fake _FAKES[] = {
  {"asleep",0,0,0,false,0}, {"one idle",1,0,0,false,12000},
  {"busy",4,3,0,false,89000}, {"attention",2,1,1,false,45000},
  {"completed",1,0,0,true,142000},
};

inline void dataSetDemo(bool on) {
  _demoMode = on;
  if (on) { _demoIdx = 0; _demoNext = millis(); }
}
inline bool dataDemo() { return _demoMode; }

inline bool dataConnected() {
  return _lastLiveMs != 0 && (millis() - _lastLiveMs) <= 30000;
}

inline bool dataBtActive() {
  // Desktop's idle keepalive is ~10s; give it 1.5x headroom.
  return _lastBtByteMs != 0 && (millis() - _lastBtByteMs) <= 15000;
}

inline const char* dataScenarioName() {
  if (_demoMode) return _FAKES[_demoIdx].n;
  if (dataConnected()) return dataBtActive() ? "bt" : "usb";
  return "none";
}

// Set true once the bridge sends a time sync — until then the RTC may
// hold whatever was on the coin cell (or 2000-01-01 if it lost power).
static bool _rtcValid = false;
inline bool dataRtcValid() { return _rtcValid; }

// Tooling hooks — defined in main.cpp / hal_m5.cpp. Forward-declared so
// the JSON parser below can route them without dragging the full
// headers into data.h.
void cmdScreenshot();
void cmdSplash();
void cmdClearPrompt();
void cmdOpenMenu();
void cmdOpenSettings();
void cmdOpenReset();
void cmdOpenAsk();
void cmdOpenBuddies();
void cmdOpenInfo(uint8_t page);
void cmdCloseAll();
void cmdSetRotation(uint8_t r);
void halInjectTap  (int sx, int sy, uint32_t durMs);
void halInjectSwipe(int sx0, int sy0, int sx1, int sy1, uint32_t durMs);

// True while lines from the USB serial port are applied. Wi-Fi provisioning
// is accepted only from there: the BLE link is open to anyone in range.
static bool _fromUsb = false;

static void _applyJson(const char* line, TamaState* out) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;
  // Tooling commands: dump sprite / inject synthetic touch. Handled here
  // before xferCommand() so they short-circuit cleanly.
  const char* tcmd = doc["cmd"];
  // {"cmd":"wifi","ssid":"...","pass":"...","secret":"<64 hex>"}; an empty
  // ssid forgets the network and brings BLE back. Restarts to apply.
  if (tcmd && strcmp(tcmd, "wifi") == 0) {
    if (!_fromUsb) return;
    netProvision(doc["ssid"] | "", doc["pass"] | "", doc["secret"] | "");
    Serial.println("{\"ack\":\"wifi\",\"ok\":true,\"n\":0}");
    Serial.flush();
    delay(100);
    ESP.restart();
    return;
  }
  if (tcmd && strcmp(tcmd, "screenshot") == 0) {
    cmdScreenshot();
    return;
  }
  if (tcmd && strcmp(tcmd, "splash") == 0) {
    cmdSplash();
    Serial.println("{\"ack\":\"splash\",\"ok\":true,\"n\":0}");
    return;
  }
  if (tcmd && strcmp(tcmd, "clearprompt") == 0) {
    cmdClearPrompt();
    Serial.println("{\"ack\":\"clearprompt\",\"ok\":true,\"n\":0}");
    return;
  }
  // Direct overlay opens — for tools/capture_readme.py to skip
  // tap-dancing through menus.
  if (tcmd && strcmp(tcmd, "openmenu") == 0)     { cmdOpenMenu();     Serial.println("{\"ack\":\"openmenu\",\"ok\":true,\"n\":0}");     return; }
  if (tcmd && strcmp(tcmd, "opensettings") == 0) { cmdOpenSettings(); Serial.println("{\"ack\":\"opensettings\",\"ok\":true,\"n\":0}"); return; }
  if (tcmd && strcmp(tcmd, "openreset") == 0)    { cmdOpenReset();    Serial.println("{\"ack\":\"openreset\",\"ok\":true,\"n\":0}");    return; }
  if (tcmd && strcmp(tcmd, "openask") == 0)      { cmdOpenAsk();      Serial.println("{\"ack\":\"openask\",\"ok\":true,\"n\":0}");      return; }
  if (tcmd && strcmp(tcmd, "openbuddies") == 0)  { cmdOpenBuddies();  Serial.println("{\"ack\":\"openbuddies\",\"ok\":true,\"n\":0}");  return; }
  if (tcmd && strcmp(tcmd, "openinfo") == 0) {
    uint8_t page = (uint8_t)(doc["page"] | 0);
    cmdOpenInfo(page);
    Serial.printf("{\"ack\":\"openinfo\",\"ok\":true,\"n\":0,\"page\":%u}\n", page);
    return;
  }
  if (tcmd && strcmp(tcmd, "rotation") == 0) {   // saves and reboots
    Serial.println("{\"ack\":\"rotation\",\"ok\":true,\"n\":0}");
    cmdSetRotation((uint8_t)(doc["value"] | 0));
    return;
  }
  if (tcmd && strcmp(tcmd, "closeall") == 0)     { cmdCloseAll();     Serial.println("{\"ack\":\"closeall\",\"ok\":true,\"n\":0}");     return; }
  if (tcmd && strcmp(tcmd, "tap") == 0) {
    int x = doc["x"] | 0;
    int y = doc["y"] | 0;
    uint32_t dur = doc["duration"] | 80;
    halInjectTap(x, y, dur);
    Serial.println("{\"ack\":\"tap\",\"ok\":true,\"n\":0}");
    return;
  }
  if (tcmd && strcmp(tcmd, "swipe") == 0) {
    int x0 = doc["x0"] | 0;
    int y0 = doc["y0"] | 0;
    int x1 = doc["x1"] | 0;
    int y1 = doc["y1"] | 0;
    uint32_t dur = doc["duration"] | 200;
    halInjectSwipe(x0, y0, x1, y1, dur);
    Serial.println("{\"ack\":\"swipe\",\"ok\":true,\"n\":0}");
    return;
  }
  if (xferCommand(doc)) { _lastLiveMs = millis(); return; }

  // Bridge sends {"time":[epoch_sec, tz_offset_sec]}; gmtime_r on the
  // adjusted epoch yields local components including weekday.
  JsonArray t = doc["time"];
  if (!t.isNull() && t.size() == 2) {
    time_t local = (time_t)t[0].as<uint32_t>() + (int32_t)t[1];
    struct tm lt; gmtime_r(&local, &lt);
    RTC_TimeTypeDef tm = { (uint8_t)lt.tm_hour, (uint8_t)lt.tm_min, (uint8_t)lt.tm_sec };
    RTC_DateTypeDef dt = { (uint8_t)lt.tm_wday, (uint8_t)(lt.tm_mon + 1),
                           (uint8_t)lt.tm_mday, (uint16_t)(lt.tm_year + 1900) };
    M5.Rtc.SetTime(&tm);
    M5.Rtc.SetDate(&dt);
    extern uint32_t _clkLastRead;
    _clkLastRead = 0;   // force re-read so _clkDt and _rtcValid agree
    _rtcValid = true;
    _lastLiveMs = millis();
    return;
  }

  // Per-turn event: {"evt":"turn","role":"assistant","content":[{"type":"text","text":"..."}, ...]}
  // We only stash assistant text; tool-use blocks and user turns are ignored.
  const char* evt = doc["evt"];
  if (evt && strcmp(evt, "status") == 0) {
    JsonArray ss = doc["sessions"];
    uint8_t n = 0;
    for (JsonObject o : ss) {
      if (n >= TamaState::MAX_SESS) break;
      TamaState::Sess& d = out->sess[n++];
      strlcpy(d.name,   o["n"] | "", sizeof(d.name));
      strlcpy(d.branch, o["b"] | "", sizeof(d.branch));
      strlcpy(d.model,  o["m"] | "", sizeof(d.model));
      strlcpy(d.effort, o["e"] | "", sizeof(d.effort));
      strlcpy(d.tokens, o["k"] | "", sizeof(d.tokens));
      d.ctx   = o["c"] | 0;
      d.mins  = o["d"] | 0;
      d.state = (o["s"] | "i")[0];
    }
    out->nSess = n;
    out->rl5h = doc["rl"][0] | -1;
    out->rl7d = doc["rl"][1] | -1;
    _lastLiveMs = millis();
    return;
  }
  if (evt && strcmp(evt, "turn") == 0) {
    const char* role = doc["role"];
    if (role && strcmp(role, "assistant") == 0) {
      JsonArray content = doc["content"];
      if (!content.isNull()) {
        size_t cap = sizeof(out->lastTurnText) - 1;
        size_t pos = 0;
        out->lastTurnText[0] = 0;
        for (JsonObject blk : content) {
          const char* t = blk["type"];
          if (!t || strcmp(t, "text") != 0) continue;
          const char* txt = blk["text"];
          if (!txt) continue;
          if (pos && pos + 2 < cap) { out->lastTurnText[pos++] = '\n'; out->lastTurnText[pos++] = '\n'; }
          while (*txt && pos < cap) out->lastTurnText[pos++] = *txt++;
          if (pos >= cap) break;
        }
        out->lastTurnText[pos] = 0;
        out->lastTurnMs = millis();
        out->lastTurnGen++;
      }
    }
    _lastLiveMs = millis();
    return;
  }

  out->sessionsTotal     = doc["total"]     | out->sessionsTotal;
  out->sessionsRunning   = doc["running"]   | out->sessionsRunning;
  out->sessionsWaiting   = doc["waiting"]   | out->sessionsWaiting;
  out->recentlyCompleted = doc["completed"] | false;
  uint32_t bridgeTokens = doc["tokens"] | 0;
  if (doc["tokens"].is<uint32_t>()) statsOnBridgeTokens(bridgeTokens);
  out->tokensToday = doc["tokens_today"] | out->tokensToday;
  const char* m = doc["msg"];
  if (m) { strncpy(out->msg, m, sizeof(out->msg)-1); out->msg[sizeof(out->msg)-1]=0; }
  JsonArray la = doc["entries"];
  if (!la.isNull()) {
    uint8_t n = 0;
    for (JsonVariant v : la) {
      if (n >= 8) break;
      const char* s = v.as<const char*>();
      strncpy(out->lines[n], s ? s : "", 91); out->lines[n][91]=0;
      n++;
    }
    if (n != out->nLines || (n > 0 && strcmp(out->lines[n-1], out->msg) != 0)) {
      out->lineGen++;
    }
    out->nLines = n;
  }
  JsonObject pr = doc["prompt"];
  if (!pr.isNull()) {
    const char* pid = pr["id"]; const char* pt = pr["tool"]; const char* ph = pr["hint"];
    strncpy(out->promptId,   pid ? pid : "", sizeof(out->promptId)-1);   out->promptId[sizeof(out->promptId)-1]=0;
    strncpy(out->promptTool, pt  ? pt  : "", sizeof(out->promptTool)-1); out->promptTool[sizeof(out->promptTool)-1]=0;
    strncpy(out->promptHint, ph  ? ph  : "", sizeof(out->promptHint)-1); out->promptHint[sizeof(out->promptHint)-1]=0;
    // Forward-compatible: pick up choices[] if the bridge ever forwards
    // multi-choice questions. Each entry is either a string label or an
    // object {id, label} — accept both.
    out->promptChoiceN = 0;
    JsonArray ch = pr["choices"];
    if (!ch.isNull()) {
      for (JsonVariant v : ch) {
        if (out->promptChoiceN >= TamaState::MAX_CHOICES) break;
        const char* lbl = nullptr; const char* cid = nullptr;
        if (v.is<const char*>())     { lbl = v.as<const char*>(); }
        else if (v.is<JsonObject>()) { JsonObject o = v.as<JsonObject>(); lbl = o["label"]; cid = o["id"]; }
        if (!lbl) continue;
        uint8_t i = out->promptChoiceN++;
        strncpy(out->promptChoiceLabels[i], lbl, TamaState::CHOICE_LABEL_LEN - 1);
        out->promptChoiceLabels[i][TamaState::CHOICE_LABEL_LEN - 1] = 0;
        // Use the provided id, otherwise just the index as a stringified fallback.
        if (cid) {
          strncpy(out->promptChoiceIds[i], cid, sizeof(out->promptChoiceIds[i]) - 1);
          out->promptChoiceIds[i][sizeof(out->promptChoiceIds[i]) - 1] = 0;
        } else {
          snprintf(out->promptChoiceIds[i], sizeof(out->promptChoiceIds[i]), "%u", i);
        }
      }
    }
  } else {
    // Locally-injected test prompts (id begins with "test-") are
    // preserved across bridge heartbeats so the modal stays up until
    // the user taps a choice. Main.cpp clears them itself after the
    // "sent..." confirmation has been displayed.
    if (strncmp(out->promptId, "test-", 5) != 0) {
      out->promptId[0] = 0; out->promptTool[0] = 0; out->promptHint[0] = 0;
      out->promptChoiceN = 0;
    }
  }
  out->lastUpdated = millis();
  _lastLiveMs = millis();
}

template<size_t N>
struct _LineBuf {
  char buf[N];
  uint16_t len = 0;
  void feed(Stream& s, TamaState* out) {
    while (s.available()) {
      char c = s.read();
      if (c == '\n' || c == '\r') {
        if (len > 0) { buf[len]=0; if (buf[0]=='{') _applyJson(buf, out); len=0; }
      } else if (len < N-1) {
        buf[len++] = c;
        // Binary firmware upload over USB (see serial_ota.h).
        if (len >= 4 && &s == &Serial && !memcmp(buf + len - 4, "FWB1", 4)) {
          len = 0;
          serial_ota::run(s);
        }
      }
    }
  }
};

static _LineBuf<1024> _usbLine, _btLine;

inline void dataPoll(TamaState* out) {
  uint32_t now = millis();

  if (_demoMode) {
    if (now >= _demoNext) { _demoIdx = (_demoIdx + 1) % 5; _demoNext = now + 8000; }
    const _Fake& s = _FAKES[_demoIdx];
    out->sessionsTotal=s.t; out->sessionsRunning=s.r; out->sessionsWaiting=s.w;
    out->recentlyCompleted=s.c; out->tokensToday=s.tok; out->lastUpdated=now;
    out->connected = true;
    snprintf(out->msg, sizeof(out->msg), "demo: %s", s.n);
    return;
  }

  _fromUsb = true;
  _usbLine.feed(Serial, out);
  _fromUsb = false;
  static TamaState* _netOut;
  _netOut = out;
  netPoll([](const char* json) { _applyJson(json, _netOut); });
  // BLE ring buffer is drained manually since it's not a Stream.
  while (bleAvailable()) {
    int c = bleRead();
    if (c < 0) break;
    _lastBtByteMs = millis();
    if (c == '\n' || c == '\r') {
      if (_btLine.len > 0) {
        _btLine.buf[_btLine.len] = 0;
        if (_btLine.buf[0] == '{') _applyJson(_btLine.buf, out);
        _btLine.len = 0;
      }
    } else if (_btLine.len < sizeof(_btLine.buf) - 1) {
      _btLine.buf[_btLine.len++] = (char)c;
    }
  }

  out->connected = dataConnected();
  if (!out->connected) {
    out->sessionsTotal=0; out->sessionsRunning=0; out->sessionsWaiting=0;
    out->recentlyCompleted=false; out->lastUpdated=now;
    strncpy(out->msg, "No Claude connected", sizeof(out->msg)-1);
    out->msg[sizeof(out->msg)-1]=0;
  }
}
