#include "hal_m5.h"
#include "canvas.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <stdarg.h>
#include "ble_bridge.h"
#include "net_bridge.h"
#include "data.h"
#include "buddy.h"
#include "touch_keyboard.h"
#include "version.h"
#if BUDDY_ASK_CLAUDE
#include "wifi_creds.h"
#include "ask_claude.h"
#include <WiFi.h>             // closeAsk() calls WiFi.disconnect() to free the radio
#endif

// Splash-hold deadline (set by cmd:splash). While in the future, the
// main loop's draw block is skipped and the existing sprite contents
// (the held splash) get re-pushed each frame, giving snap.py time to
// capture it without race-to-redraw. Definitions live further down the
// file (after W / CLAUDE_CORAL / drawSparkle / character APIs are all
// in scope); only the latch flag lives up here.
static uint32_t splashHoldUntilMs = 0;

// Advertise as "Claude-XXXX" (last two BT MAC bytes) so multiple sticks
// in one room are distinguishable in the desktop picker. Name persists in
// btName for the BLUETOOTH info page.
static char btName[16] = "Claude";
// The BT MAC, or the chip's base MAC on boards whose BLE radio is a
// co-processor (ESP32-P4: no BT MAC in eFuse, esp_read_mac returns zeros).
static void btMac(uint8_t mac[6]) {
  if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK || !(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]))
    esp_efuse_mac_get_default(mac);
}
static void startBt() {
  // A board provisioned for Wi-Fi talks to its host over the authenticated
  // TCP bridge and leaves the (unencrypted) BLE radio off.
  if (netConfigured()) { netInit(); return; }
  uint8_t mac[6] = {0};
  btMac(mac);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

#include "character.h"
#include "stats.h"
// Logical canvas size (see canvas.h) — 240x320 on the CYD, 320x480 on the
// 3.5" boards, 400x240 on the 800x480 panels at 2x, and so on. Set once in
// setup() from the Canvas, before anything draws.
int W = 240, H = 320;
int CX = 120;

// ─── Layout ─────────────────────────────────────────────────────────────
// Two arrangements, picked from the canvas shape:
//   Portrait  (W <= H) — upstream's CYD layout: status strip, pet on top,
//             activity line + transcript HUD below; Info/Pet pages shrink
//             the pet into a 70 px "peek" header. Taller canvases give the
//             pet a 3x scale and the HUD more rows.
//   Landscape (W >  H) — the pet lives in a left pane at full size all the
//             time, the shortcut bubbles sit in a row under it, and the
//             right pane holds the activity line + HUD, the approval card,
//             the clock and the Info/Pet pages.
// Everything below reads these instead of hardcoded 240x320 coordinates.
struct Layout {
  bool    land;
  uint8_t petScale;        // ASCII buddy home scale (2 or 3)
  int     petX, petW;      // pet region (portrait: full width)
  int     petY, petH;      // portrait: 0..petH is the pet's band
  int     paneX, paneW;    // content pane (portrait: full width)
  int     actY;            // activity line
  int     hudY;            // transcript HUD top (runs to H)
  int     approvalY;       // approval card top (runs to H)
  int     infoX, infoY, infoW;   // Info / Pet pages
  int     bubbleX, bubbleY;      // first home bubble
  bool    bubbleRow;             // bubbles laid out in a row (landscape)
};
static Layout L;
static const int STATUS_H_ = 14;   // status strip height (STATUS_H below is the same value)

static void computeLayout() {
  L.land = W > H;
  if (!L.land) {
    L.petScale  = (W >= 300 && H >= 440) ? 3 : 2;
    L.petX = 0; L.petW = W;
    L.petY = 0;
    L.petH = (L.petScale == 3) ? 200 : 150;
    L.paneX = 0; L.paneW = W;
    L.actY  = L.petH + 24;
    L.hudY  = L.actY + 14;
    L.approvalY = (H - L.petH - 20 > 150) ? L.petH + 20 : H - 150;
    L.infoX = 0; L.infoY = 70; L.infoW = W;
    L.bubbleX = 4; L.bubbleY = 22; L.bubbleRow = false;
  } else {
    L.petScale  = 2;
    L.petX = 0; L.petW = (W * 45) / 100;
    L.petY = STATUS_H_ + 1;
    L.petH = H - L.petY - 32;          // leaves room for the bubble row
    L.paneX = L.petW + 1; L.paneW = W - L.paneX;
    L.actY  = STATUS_H_ + 1;
    L.hudY  = L.actY + 14;
    L.approvalY = STATUS_H_ + 1;
    L.infoX = L.paneX; L.infoY = 0; L.infoW = L.paneW;
    L.bubbleX = 6; L.bubbleY = H - 27; L.bubbleRow = true;
  }
}

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
// Claude brand colors — coral on white for badges, accent lines, the
// status-strip sparkle and the splash wordmark. Stay constant regardless
// of which theme is active.
static const uint16_t CLAUDE_CORAL = 0xDBAA;     // #D97757 → RGB565
static const uint16_t CLAUDE_INK   = 0xFFFF;     // white glyphs on coral
// The play-sparkle ("logo the pet plays with") gets a brighter, more
// saturated orange — closer to claude.ai's vivid asterisk after the 8bpp
// sprite's RGB332 quantization mutes CLAUDE_CORAL into a pinkier hue.
// 0xFB46 ≈ #F86830, which sits at a clean RGB332 grid point so it
// quantizes cleanly to recognisable Claude orange instead of drifting.
static const uint16_t CLAUDE_SPARK = 0xFB46;
const uint16_t PANEL = 0x2104;   // overlay panel background

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 4;           // 0..4 → ScreenBreath 20..100
bool    btnALong    = false;

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
bool     swallowBtnA = false;
bool     swallowBtnB = false;
bool     buddyMode = false;
bool     gifAvailable = false;
const uint8_t SPECIES_GIF = 0xFF;   // species NVS sentinel: use the installed GIF

static void applyTheme();                                                  // defined below; called from nextPet()
static const int WRAP_COLS = 80;   // max chars per wrapped row (+NUL)
static uint8_t wrapInto(const char* in, char out[][WRAP_COLS], uint8_t maxRows, uint8_t width);   // ditto; used by the RESPONSE Info page before its own definition
// Sparkle / branding helpers — declared up front so drawClock() and the
// other early users can call them before their definitions appear.
static void drawSparkle(int cx, int cy, int r, uint16_t col);
static void drawClaudeBadge(int x, int y);
static void drawClaudeBadgeCentered(int y);
static void sparklePlaySpawn();
static void drawSparklePlay();

// Cycle GIF (if installed) → ASCII species 0..N-1 → GIF. Persisted to the
// existing "species" NVS key; 0xFF means GIF mode.
static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → species 0
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {  // last species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {                                   // species i → species i+1
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
  applyTheme();   // re-pick theme vs manifest palette for the new mode
}
uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS = 30000;

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;

// Face-down = Z-axis dominant and negative. Debounced so a toss doesn't count.
static bool isFaceDown() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

static void applyBrightness() { M5.Axp.ScreenBreath(20 + brightLevel * 20); }

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    M5.Axp.SetLDO2(true);
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}
bool     responseSent = false;

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) M5.Beep.tone(freq, dur);
}

// Event-specific sound patterns. Each is a short note sequence (freq Hz,
// dur ms; freq=0 = rest). Picked so the device is usable across the room
// without looking — approve/deny/level-up are deliberately distinctive.
static const BeepNote SFX_APPROVE_ARRIVE[] = { {900,  60}, {1300, 80} };
static const BeepNote SFX_APPROVED[]       = { {1000, 50}, {1500, 50}, {2000, 90} };
static const BeepNote SFX_DENIED[]         = { {800,  80}, {500, 150} };
static const BeepNote SFX_LEVEL_UP[]       = { {1200, 60}, {1500, 60}, {1800, 60}, {2200, 120} };
static const BeepNote SFX_MENU[]           = { {800,  60} };
static const BeepNote SFX_PASSKEY[]        = { {1400, 60}, {0, 60}, {1800, 100} };
static const BeepNote SFX_HEART[]          = { {1500, 50}, {2000, 80} };
static const BeepNote SFX_NAV[]            = { {1800, 30} };

static void sfx(const BeepNote* seq, uint8_t n) {
  if (settings().sound) M5.Beep.play(seq, n);
}
#define SFX(name) sfx(name, sizeof(name) / sizeof(name[0]))

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
  netWrite(json);
}
const uint8_t INFO_PAGES = 8;
const uint8_t INFO_PG_BUTTONS  = 1;
const uint8_t INFO_PG_RESPONSE = 3;   // "What Claude just said" — turn events
const uint8_t INFO_PG_SESSIONS = 4;   // visual breakdown of active sessions
const uint8_t INFO_PG_CREDITS  = 7;

void applyDisplayMode() {
  // Portrait shrinks the pet into the 70 px header on Info/Pet pages;
  // landscape keeps it full size in its own pane.
  bool peek = displayMode != DISP_NORMAL && !L.land;
  characterSetPeek(peek);
  buddySetPeek(peek);
  // Clear the whole sprite on mode switch. drawInfo/drawPet clear their
  // own regions when they run, but when you switch FROM info/pet TO normal,
  // those functions stop running and their stale pixels stay behind. Full
  // clear is cheap and guarantees no leftovers between modes.
  spr.fillSprite(0x0000);
  characterInvalidate();  // redraws character on next tick (text mode path)
}

// Item text indexed by a stable ID; menuIds[] lists the IDs this build
// shows, in order. Boards built without Ask Claude drop its entries.
const char* menuItems[] = { "ask claude", "buddies", "settings", "turn off", "help", "about", "demo" };
#if BUDDY_ASK_CLAUDE
static const uint8_t menuIds[] = { 0, 1, 2, 3, 4, 5, 6 };
#else
static const uint8_t menuIds[] = { 1, 2, 3, 4, 5, 6 };
#endif
const uint8_t MENU_N = sizeof(menuIds);

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
const char* settingsItems[] = {
  "brightness", "sound", "bluetooth", "wifi", "led", "transcript",
  "rotation", "ascii pet", "theme", "wifi setup", "api key",
  "test choice", "calibrate", "reset"
};
#if BUDDY_ASK_CLAUDE
static const uint8_t settingsIds[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
#else
static const uint8_t settingsIds[] = { 0, 1, 2, 4, 5, 6, 7, 8, 11, 12, 13 };
#endif
const uint8_t SETTINGS_N = sizeof(settingsIds);

// Screen rotation (LovyanGFX 0..3), persisted separately from Settings so
// the struct's NVS layout stays upstream-compatible. Upstream's "clock rot"
// setting (an IMU feature these boards can't use) became this one. Changing
// it reboots: the canvas, layout and touch calibration all depend on it.
static uint8_t rotationLoad() {
  Preferences p; p.begin("disp", true);
  uint8_t r = p.getUChar("rot", BUDDY_ROTATION);
  p.end();
  return r & 3;
}
static void rotationSave(uint8_t r) {
  Preferences p; p.begin("disp", false);
  p.putUChar("rot", r & 3);
  p.end();
}

// Built-in themes. Index 0 = upstream default (matches character.cpp's pal
// initialiser). Indices 1-3 are CYD additions. RGB565.
//                                 body    bg      text    textDim  ink
static const Palette THEMES[] = {
  /* default */    { 0xC2A6, 0x0000, 0xFFFF, 0x8410, 0x0000 },   // dim olive on black
  /* claude light*/{ 0xDBAA, 0xF79D, 0x39C5, 0x8BEC, 0xF79D },   // coral on cream, brown text
  /* claude dark */{ 0xDBAA, 0x18E3, 0xEF19, 0x8C0E, 0x18E3 },   // coral on near-black, cream text
  /* terminal    */{ 0x07E0, 0x0000, 0xFFFF, 0x8410, 0x0000 },   // pure green on black
};
static const char* THEME_NAMES[] = { "default", "light", "dark", "term" };
static const uint8_t THEME_N = sizeof(THEMES) / sizeof(THEMES[0]);

// Push the right palette to character.cpp based on which renderer is live.
// ASCII mode (buddyMode) honours the user's theme choice; GIF mode honours
// the loaded pack's manifest palette. Call on boot, on theme change, and
// on every ASCII<->GIF mode flip.
static void applyTheme() {
  if (buddyMode || !characterLoaded()) {
    characterSetPalette(THEMES[settings().theme % THEME_N]);
  } else {
    characterRestoreManifestPalette();
  }
}

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "delete char", "factory reset" };
const uint8_t RESET_N = 2;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      return;
    case 1: s.sound = !s.sound; break;
    case 2:
      // BT toggle is a stored preference only — BLE stays live. Turning
      // BLE off cleanly would require tearing down the BLE stack which
      // the Arduino BLE library doesn't do reliably. If we need a
      // hard-off someday, stop advertising via BLEDevice::getAdvertising().
      s.bt = !s.bt;
      break;
    case 3: s.wifi = !s.wifi; break;   // stored only
    case 4: s.led = !s.led; break;
    case 5: s.hud = !s.hud; break;
    case 6:
      rotationSave(M5.Lcd.getRotation() + 1);
      delay(100);
      ESP.restart();
      return;
    case 7: nextPet(); return;
    case 8: s.theme = (s.theme + 1) % THEME_N; applyTheme(); break;
#if BUDDY_ASK_CLAUDE
    case 9: {                                   // wifi setup → keyboard ×2
      char ssid[WIFI_SSID_LEN] = {0};
      char pass[WIFI_PASS_LEN] = {0};
      wifiCredsLoad(ssid, pass);
      if (!kbdShow("WiFi SSID",     ssid, sizeof(ssid), false)) return;
      if (!kbdShow("WiFi password", pass, sizeof(pass), true))  return;
      wifiCredsSave(ssid, pass);
      s.wifi = true;
      return;
    }
    case 10: {                                  // api key → masked keyboard
      char key[ANTHROPIC_KEY_LEN] = {0};
      apiKeyLoad(key);
      if (!kbdShow("Anthropic API key", key, sizeof(key), true)) return;
      apiKeySave(key);
      return;
    }
#endif
    case 11: {                                  // inject a fake multi-choice prompt
      // Quick demo of the multi-choice modal that lights up automatically
      // if/when the desktop bridge ever forwards prompt.choices[]. While
      // a real bridge heartbeat will eventually overwrite this (~few s
      // when connected), an offline device holds it indefinitely.
      strncpy(tama.promptId,   "test-mcq-1", sizeof(tama.promptId) - 1);
      tama.promptId[sizeof(tama.promptId) - 1] = 0;
      strncpy(tama.promptTool, "Bash", sizeof(tama.promptTool) - 1);
      tama.promptTool[sizeof(tama.promptTool) - 1] = 0;
      strncpy(tama.promptHint, "Choose how to handle ~/Downloads/old/",
              sizeof(tama.promptHint) - 1);
      tama.promptHint[sizeof(tama.promptHint) - 1] = 0;
      tama.promptChoiceN = 3;
      const char* lbls[3] = { "delete it", "move to backup", "do nothing" };
      const char* ids [3] = { "del",       "move",            "skip"       };
      for (int i = 0; i < 3; i++) {
        strncpy(tama.promptChoiceLabels[i], lbls[i], TamaState::CHOICE_LABEL_LEN - 1);
        tama.promptChoiceLabels[i][TamaState::CHOICE_LABEL_LEN - 1] = 0;
        strncpy(tama.promptChoiceIds[i],    ids[i],  sizeof(tama.promptChoiceIds[i]) - 1);
        tama.promptChoiceIds[i][sizeof(tama.promptChoiceIds[i]) - 1] = 0;
      }
      responseSent     = false;
      promptArrivedMs  = millis();
      lastPromptId[0]  = 0;                 // re-arm the new-prompt detector
      settingsOpen     = false;             // close settings so the modal is visible
      characterInvalidate();
      SFX(SFX_APPROVE_ARRIVE);
      return;
    }
    case 12:                                    // calibrate touch
      touchCalibrate();
      spr.invalidate();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
      return;
    case 13: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
  }
  settingsSave();
}

// Tap-twice confirm: first tap arms (label flips to "really?"), second
// within 3s executes. Scrolling away clears the arm.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  if (idx == 0) {
    // delete char: wipe /characters/, reboot into ASCII mode
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            LittleFS.remove(fp);
          }
          e.close();
          LittleFS.rmdir(path);
        } else {
          e.close();
          LittleFS.remove(path);
        }
      }
      d.close();
    }
  } else {
    // factory reset: NVS namespace wipe + filesystem format + BLE bonds.
    // Clears stats, owner, petname, species, settings, GIF characters,
    // and any stored LTKs so the next desktop has to re-pair.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

// Footer hint row inside a menu panel — now that rows are directly
// tappable, the hint is just a reminder of the close affordance.
const int MENU_HINT_H = 14;
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* /*downLbl*/ = "", const char* /*rightLbl*/ = "") {
  spr.drawFastHLine(mx + 6, hy - 4, mw - 12, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  spr.setCursor(mx + 8, hy);
  spr.print("tap a row, X to close");
}

// Top-right X close button drawn on every overlay panel. Tap region is
// generous so a resistive finger lands easily.
static void drawPanelX(const Palette& p, int mx, int my, int mw, uint16_t edgeCol) {
  // small rounded box around the X for visual hit-target
  spr.fillRoundRect(mx + mw - 22, my + 2, 18, 14, 3, PANEL);
  spr.drawRoundRect(mx + mw - 22, my + 2, 18, 14, 3, edgeCol);
  spr.setTextSize(1);
  spr.setTextColor(edgeCol, PANEL);
  spr.setCursor(mx + mw - 16, my + 6);
  spr.print("X");
}

static void drawSettings() {
  const Palette& p = characterPalette();
  int mw = 210, mh = 16 + SETTINGS_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  Settings& s = settings();
  bool vals[] = { s.sound, s.bt, s.wifi, s.led, s.hud };
  for (int row = 0; row < SETTINGS_N; row++) {
    const uint8_t i = settingsIds[row];
    bool sel = (row == settingsSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + row * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    // Shift the value column inward enough that the top-right X close
    // badge can't overlap the first row's value text.
    spr.setCursor(mx + mw - 58, my + 8 + row * 14);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      spr.printf("%u/4", brightLevel);
    } else if (i >= 1 && i <= 5) {
      spr.setTextColor(vals[i-1] ? GREEN : p.textDim, PANEL);
      spr.print(vals[i-1] ? " on" : "off");
    } else if (i == 6) {
      spr.printf("%u", (unsigned)M5.Lcd.getRotation() * 90);
    } else if (i == 7) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.printf("%u/%u", pos, total);
    } else if (i == 8) {
      // Show theme name, tinted with the theme's own body color so the
      // user previews the accent without committing to the switch.
      spr.setTextColor(THEMES[s.theme % THEME_N].body, PANEL);
      spr.print(THEME_NAMES[s.theme % THEME_N]);
#if BUDDY_ASK_CLAUDE
    } else if (i == 9) {
      bool ok = wifiCredsPresent();
      spr.setTextColor(ok ? GREEN : p.textDim, PANEL);
      spr.print(ok ? "set" : "tap");
    } else if (i == 10) {
      bool ok = apiKeyPresent();
      spr.setTextColor(ok ? GREEN : p.textDim, PANEL);
      spr.print(ok ? "set" : "tap");
#endif
    }
  }
  drawPanelX(p, mx, my, mw, p.textDim);
  drawMenuHints(p, mx, mw, my + mh - 12);
}

static void drawReset() {
  const Palette& p = characterPalette();
  int mw = 210, mh = 16 + RESET_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, HOT);
  spr.setTextSize(1);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawPanelX(p, mx, my, mw, HOT);
  drawMenuHints(p, mx, mw, my + mh - 12);
}

#if BUDDY_ASK_CLAUDE
static void openAsk();              // defined below; standalone Claude client modal
#endif
static void openBuddySwitcher();    // defined below; full-screen species preview

void menuConfirm() {
  switch (menuIds[menuSel]) {
#if BUDDY_ASK_CLAUDE
    case 0: menuOpen = false; openAsk(); break;
#endif
    case 1: menuOpen = false; openBuddySwitcher(); break;
    case 2: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 3:
      // Deep sleep where a touch IRQ can wake us; otherwise just blank.
      menuOpen = false;
      if (!M5.Axp.PowerOff()) { M5.Axp.SetLDO2(false); screenOff = true; }
      break;
    case 4:
    case 5:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuIds[menuSel] == 4) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 6: dataSetDemo(!dataDemo()); break;
  }
}

void drawMenu() {
  const Palette& p = characterPalette();
  int mw = 210, mh = 16 + MENU_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[menuIds[i]]);
    if (menuIds[i] == 6) spr.print(dataDemo() ? "  on" : "  off");   // demo toggle suffix
  }
  drawPanelX(p, mx, my, mw, p.textDim);
  drawMenuHints(p, mx, mw, my + mh - 12);
}

// Clock orientation: gravity along the in-plane X axis means the stick is
// on its side. Signed counter for hysteresis on both transitions — same
// pattern as face-down nap.
//   0 = portrait (sprite path, pet sleeps underneath)
//   1 = landscape, BtnA-side down (M5.Lcd rotation 1)
//   3 = landscape, USB-side down (M5.Lcd rotation 3)
static uint8_t clockOrient   = 0;
static int8_t  orientFrames  = 0;
static uint8_t paintedOrient = 0;
// RTC and IMU share an I2C bus. Reading the RTC at 60fps starves the IMU
// reads in clockUpdateOrient — orientation detection gets noisy. Cache the
// time once per second; mood logic and drawClock both read from here.
static RTC_TimeTypeDef _clkTm;
static RTC_DateTypeDef _clkDt;
uint32_t               _clkLastRead = 0;   // zeroed by data.h on time-sync
static bool            _onUsb       = false;
static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = M5.Axp.GetVBusVoltage() > 4.0f;
  M5.Rtc.GetTime(&_clkTm);
  M5.Rtc.GetDate(&_clkDt);
}

static void clockUpdateOrient() {
  // CYD has no accelerometer, so there's no gravity vector to derive a
  // landscape orientation from. Pin the clock to portrait; the "clock
  // rot" setting becomes a no-op (kept so NVS/settings layout is stable).
  clockOrient = 0;
  return;
#if 0
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  uint8_t lock = settings().clockRot;
  if (lock == 1) { clockOrient = 0; return; }
  if (lock == 2) {
    // Locked landscape: never drop to 0, but still pick 1 vs 3 from
    // gravity so the cradle works either way up. Need a strong tilt
    // for the 1↔3 swap so handling jitter doesn't flip it; otherwise
    // hold whatever we last had (or 1 from boot).
    if (clockOrient == 0) clockOrient = (ax >= 0) ? 1 : 3;
    if      (ax >  0.5f && clockOrient != 1) clockOrient = 1;
    else if (ax < -0.5f && clockOrient != 3) clockOrient = 3;
    return;
  }
  // Dual threshold: strict to enter (must be clearly sideways), loose to
  // stay (tolerate ~65° of tilt). With one shared threshold a slight lean
  // while sitting on the long edge puts ax right at the boundary and the
  // counter ratchets down in ~half a second.
  bool side = (clockOrient == 0)
    ? fabsf(ax) > 0.7f && fabsf(ay) < 0.5f && fabsf(az) < 0.5f
    : fabsf(ax) > 0.4f;
  if (side) { if (orientFrames < 20) orientFrames++; }
  else      { if (orientFrames > -10) orientFrames--; }
  if (clockOrient == 0 && orientFrames >= 15) {
    clockOrient = (ax > 0) ? 1 : 3;
  } else if (clockOrient != 0 && orientFrames <= -8) {
    clockOrient = 0;
  } else if (clockOrient != 0 && side) {
    // Direct 1↔3: a fast flip keeps |ax|>0.7 (just changes sign), so
    // `side` never drops and the exit-via-0 path can't fire. Watch for
    // ax sign disagreeing with the stored orientation.
    static int8_t swapFrames = 0;
    uint8_t want = (ax > 0) ? 1 : 3;
    if (want != clockOrient) { if (++swapFrames >= 8) { clockOrient = want; swapFrames = 0; } }
    else swapFrames = 0;
  }
#endif
}

// Clock face: shown when charging on USB with nothing else going on.
// Portrait paints the upper ~110px to the sprite; pet renders below.
// Landscape draws direct to LCD with rotation — sprite stays untouched.
static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static uint8_t clockDow() { return _clkDt.WeekDay % 7; }
static void drawClock() {
  const Palette& p = characterPalette();
  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", _clkTm.Hours, _clkTm.Minutes);
  char ss[4]; snprintf(ss, sizeof(ss), ":%02u", _clkTm.Seconds);
  uint8_t mi = (_clkDt.Month >= 1 && _clkDt.Month <= 12) ? _clkDt.Month - 1 : 0;
  char dl[12]; snprintf(dl, sizeof(dl), "%s %s %02u", DOW[clockDow()], MON[mi], _clkDt.Date);
  paintedOrient = 0;

  // Portrait: the pet keeps its band at the top and the clock takes the
  // rest (upstream: 150..320 on the CYD). Landscape: the pet keeps the left
  // pane and the clock fills the right one.
  int x0, y0, w, h;
  if (L.land) { x0 = L.paneX; y0 = STATUS_H_ + 1; w = L.paneW; h = H - y0; }
  else        { x0 = 0; y0 = L.petH; w = W; h = H - L.petH; }
  int cx = x0 + w / 2, mid = y0 + h / 2;
  spr.fillRect(x0, y0, w, h, p.bg);
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(5); spr.setTextColor(p.text, p.bg);    spr.drawString(hm, cx, mid - 25);
  spr.setTextSize(2); spr.setTextColor(p.textDim, p.bg); spr.drawString(ss, cx, mid + 17);
  spr.setTextSize(2);                                     spr.drawString(dl, cx, mid + 47);
  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(1);
  // Claude branding strip at top of the clock face, above the pet area.
  if (!L.land) drawClaudeBadgeCentered(2);
  // Sparkle "pet plays with the logo" particle is drawn here too so it
  // animates on the clock face — the pet peek is right above the time
  // text, so a sparkle drifting through that area reads as the pet
  // playing with it.
  drawSparklePlay();
}

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 3)  return P_BUSY;
  return P_IDLE;   // connected, 0+ sessions, nothing urgent — hang out
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

// ─── Easter-egg idle speech bubbles ─────────────────────────────────────
// Pet shows a small "thought bubble" with a short word every few minutes
// while idle. Some entries are time-/day-gated so the buddy feels alive
// without ever spamming you.
struct EggMsg { const char* text; uint8_t weekdayMask; int8_t hour; };
//                                       0=any, bits 1<<0..1<<6 = Sun..Sat
//                                       hour: -1 = any, else exact hour
static const EggMsg EGGS[] = {
  { "hi!",       0,        -1 },
  { "hmm...",    0,        -1 },
  { "tea?",      0,        -1 },
  { "yawn",      0,        -1 },
  { "code?",     0,        -1 },
  { "claude!",   0,        -1 },
  { "ready",     0,        -1 },
  { "...",       0,        -1 },
  { "morning!",  0,         9 },
  { "lunch?",    0,        12 },
  { "tea time",  0,        15 },
  { "TGIF",      0x20,     -1 },    // Friday
  { "weekend!",  0x41,     -1 },    // Sun | Sat
  { "Monday :(", 0x02,      9 },
  { "almost!",   0,        17 },
  { "night...",  0,        22 },
};
static const uint8_t N_EGGS = sizeof(EGGS) / sizeof(EGGS[0]);

static uint32_t    eggUntilMs = 0;        // 0 = no bubble showing
static uint32_t    eggNextDueMs = 0;
static const char* eggText = nullptr;

// Returns true when activeState looks "ambient" — pet is calm and on-screen
// and a passing thought wouldn't interrupt anything important.
static bool _eggAmbient() {
  // askOpen/bswOpen modals short-circuit the main loop's draw block, so
  // drawEggBubble() doesn't paint while they're up — no need to gate on
  // them here (they're declared later in the file anyway).
  return (activeState == P_IDLE || activeState == P_SLEEP)
      && !menuOpen && !settingsOpen && !resetOpen
      && !screenOff && !napping
      && displayMode == DISP_NORMAL
      && tama.promptId[0] == 0;
}

// Pick a random egg that matches the current weekday/hour gates.
static const EggMsg* _eggPick() {
  // Read current hour/day from the software RTC if synced; default to "any"
  // so eggs still fire pre-sync.
  uint8_t hr = 12, dow = 1;
  if (dataRtcValid()) {
    RTC_TimeTypeDef t; M5.Rtc.GetTime(&t);
    RTC_DateTypeDef d; M5.Rtc.GetDate(&d);
    hr  = t.Hours;
    dow = d.WeekDay % 7;
  }
  // Tally eligible eggs, then pick uniformly.
  uint8_t eligible[N_EGGS]; uint8_t n = 0;
  for (uint8_t i = 0; i < N_EGGS; i++) {
    if (EGGS[i].weekdayMask && !(EGGS[i].weekdayMask & (1 << dow))) continue;
    if (EGGS[i].hour >= 0 && EGGS[i].hour != hr) continue;
    eligible[n++] = i;
  }
  if (n == 0) return nullptr;
  return &EGGS[eligible[esp_random() % n]];
}

static void eggTick() {
  uint32_t now = millis();
  if (eggUntilMs && (int32_t)(now - eggUntilMs) >= 0) {
    eggUntilMs = 0;
    eggText    = nullptr;
  }
  if (!_eggAmbient()) { eggNextDueMs = 0; return; }
  if (eggNextDueMs == 0) {
    eggNextDueMs = now + 60000UL + (esp_random() % 120000UL);    // 1–3 min
    return;
  }
  if ((int32_t)(now - eggNextDueMs) < 0) return;
  // ~1-in-2 idle fires becomes a "pet plays with the logo" sparkle drift
  // instead of a text bubble — so on average a sparkle every ~2–6 min.
  if ((esp_random() & 1) == 0) {
    sparklePlaySpawn();
  } else {
    const EggMsg* e = _eggPick();
    if (e) {
      eggText    = e->text;
      eggUntilMs = now + 1800;
      SFX(SFX_NAV);
    }
  }
  eggNextDueMs = now + 60000UL + (esp_random() % 120000UL);
}

// ─── Sparkle-play particle ──────────────────────────────────────────────
// Occasional drifting Claude sparkle that floats near the pet during long
// idle stretches — the buddy "playing" with the logo. Spawned by the egg
// system as a visual alternative to a speech bubble.
static struct {
  bool     active = false;
  float    x = 0, y = 0;
  float    vx = 0, vy = 0;
  uint32_t until = 0;
} sparklePlay;

static void sparklePlaySpawn() {
  sparklePlay.active = true;
  // Drop in from the upper-right and float down/left toward the pet.
  sparklePlay.x  = L.petX + L.petW - 28;
  sparklePlay.y  = L.petY + 30;
  sparklePlay.vx = -0.6f;
  sparklePlay.vy = 0.4f;
  sparklePlay.until = millis() + 4500;
}

static void sparklePlayTick() {
  if (!sparklePlay.active) return;
  uint32_t now = millis();
  if ((int32_t)(now - sparklePlay.until) >= 0) { sparklePlay.active = false; return; }
  sparklePlay.x += sparklePlay.vx;
  sparklePlay.y += sparklePlay.vy;
  // Soft "wall" bounces — sparkle ambles around the pet area without
  // wandering off into the HUD.
  const float xMin = L.petX + 40, xMax = L.petX + L.petW - 40;
  const float yMin = L.petY + 30,  yMax = L.petY + L.petH - 10;
  if (sparklePlay.x < xMin) { sparklePlay.vx = -sparklePlay.vx; sparklePlay.x = xMin; }
  if (sparklePlay.x > xMax) { sparklePlay.vx = -sparklePlay.vx; sparklePlay.x = xMax; }
  if (sparklePlay.y < yMin) { sparklePlay.vy = -sparklePlay.vy; sparklePlay.y = yMin; }
  if (sparklePlay.y > yMax) { sparklePlay.vy = -sparklePlay.vy; sparklePlay.y = yMax; }
}

static void drawSparklePlay() {
  if (!sparklePlay.active) return;
  drawSparkle((int)sparklePlay.x, (int)sparklePlay.y, 4, CLAUDE_SPARK);
  drawSparkle((int)sparklePlay.x, (int)sparklePlay.y, 2, CLAUDE_SPARK);
}

// ─── Reusable Claude branding badge ─────────────────────────────────────
// Small "✦ Claude" inline mark — used on the Pet stats page and the clock
// face. Total footprint ~50×8 px (sparkle + 1 px gap + size-1 wordmark).
static void drawClaudeBadge(int x, int y) {
  drawSparkle(x + 3, y + 3, 3, CLAUDE_CORAL);
  spr.setTextSize(1);
  spr.setTextColor(CLAUDE_CORAL, characterPalette().bg);
  spr.setCursor(x + 12, y);
  spr.print("Claude");
}
static void drawClaudeBadgeCentered(int y) {
  drawClaudeBadge(L.petX + (L.petW - 48) / 2, y);
}

// Draw the speech bubble above the pet's head. Called from the home draw
// path AFTER buddyTick so the bubble sits on top of the species body.
static void drawEggBubble() {
  if (!eggText || !eggUntilMs) return;
  const Palette& p = characterPalette();
  int len = (int)strlen(eggText);
  int tw  = len * 12;                                   // size 2 glyph width
  int bw  = tw + 14;
  int bh  = 24;
  int bx  = L.petX + (L.petW - bw) / 2;
  int by  = L.petY + 18;                                // upper portion above pet
  spr.fillRoundRect(bx, by, bw, bh, 6, p.bg);
  spr.drawRoundRect(bx, by, bw, bh, 6, CLAUDE_CORAL);
  // tail of the bubble
  spr.fillTriangle(bx + bw/2 - 4, by + bh,
                   bx + bw/2 + 4, by + bh,
                   bx + bw/2,     by + bh + 5, p.bg);
  spr.drawLine(bx + bw/2 - 4, by + bh,
               bx + bw/2,     by + bh + 5, CLAUDE_CORAL);
  spr.drawLine(bx + bw/2 + 4, by + bh,
               bx + bw/2,     by + bh + 5, CLAUDE_CORAL);
  spr.setTextSize(2);
  spr.setTextColor(CLAUDE_CORAL, p.bg);
  spr.setCursor(bx + 7, by + 4);
  spr.print(eggText);
  spr.setTextSize(1);
}

bool checkShake() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}




// Info / Pet pages are laid out for the full portrait screen below a 70 px
// peek header. In landscape they render in the right pane instead: the
// caller translates the canvas (beginPage) and these locals shadow the
// global W/H with the pane's size in the page's own coordinates.
#define PANE_GEOMETRY \
  const int W = L.infoW; const int H = ::H - (L.infoY - 70); (void)W; (void)H;

static void beginPage() {
  if (!L.land) return;
  spr.setOrigin(L.infoX, L.infoY - 70);
  spr.setClipRect(0, 70, L.infoW, H - L.infoY);
}
static void endPage() {
  if (!L.land) return;
  spr.setOrigin(0, 0);
  spr.clearClipRect();
}

// Persistent screen-level title row ("INFO  n/3") matching the PET header,
// then a per-page section label below it. The fixed title is the cue that
// B cycles pages here just like it does on PET.
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  PANE_GEOMETRY
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y); spr.print("Info");
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 12;
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(4, y); spr.print(section);
  y += 12;
}

// Maps a y coordinate from upstream's 320-px-tall full-screen layouts
// (splash, passkey) onto the current canvas: centred, and compressed on
// canvases shorter than 320 so nothing falls off the bottom.
static int vy(int y320) {
  float f = (H - 20) / 300.0f;
  if (f > 1.0f) f = 1.0f;
  return H / 2 + (int)((y320 - 160) * f);
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(2);
  spr.setTextColor(p.textDim, p.bg);
  spr.drawString("BLUETOOTH PAIRING", CX, vy(96));
  spr.setTextSize(4);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.drawString(b, CX, vy(160));
  spr.setTextSize(2);
  spr.setTextColor(p.textDim, p.bg);
  spr.drawString("enter on desktop", CX, vy(240));
  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(1);
}

void drawInfo() {
  PANE_GEOMETRY
  const Palette& p = characterPalette();
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](const char* fmt, ...) {
    char b[64]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setCursor(4, y); spr.print(b); y += 8;
  };

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("DeskPet watches your");
    ln("Claude desktop sessions.");
    y += 6;
    ln("It sleeps when nothing's");
    ln("happening, wakes when");
    ln("you start working,");
    ln("gets impatient when");
    ln("approvals pile up.");
    y += 6;
    spr.setTextColor(p.text, p.bg);
    ln("Tap the left side of a");
    ln("prompt to approve, the");
    ln("right to deny.");
    y += 6;
    spr.setTextColor(p.textDim, p.bg);
    ln("18 species. Open the");
    ln("face bubble to browse.");

  } else if (infoPage == 1) {
    _infoHeader(p, y, "CONTROLS", infoPage);
    // Touchscreen-only — keep the discoverable inventory of zones,
    // bubbles, and gestures up to date with the current build.
    spr.setTextColor(p.text, p.bg);    ln("Bubbles (upper-left)");
    spr.setTextColor(p.textDim, p.bg); ln("  heart = pet stats");
    ln("  face  = buddy switcher");
    ln("  gear  = settings");
    ln("  i     = info pages"); y += 4;

    spr.setTextColor(p.text, p.bg);    ln("Tap left side");
    spr.setTextColor(p.textDim, p.bg); ln("  home <-> pet stats");
    ln("  approve a prompt"); y += 4;

    spr.setTextColor(p.text, p.bg);    ln("Tap right side");
    spr.setTextColor(p.textDim, p.bg); ln("  page through info");
    ln("  deny a prompt"); y += 4;

    spr.setTextColor(p.text, p.bg);    ln("Hold left ~0.6s");
    spr.setTextColor(p.textDim, p.bg); ln("  open the main menu"); y += 4;

    spr.setTextColor(p.text, p.bg);    ln("Tap the pet");
    spr.setTextColor(p.textDim, p.bg); ln("  floating hearts"); y += 4;

    spr.setTextColor(p.text, p.bg);    ln("Swipe HUD vertically");
    spr.setTextColor(p.textDim, p.bg); ln("  scroll transcript"); y += 4;

    spr.setTextColor(p.text, p.bg);    ln("Top-right corner");
    spr.setTextColor(p.textDim, p.bg); ln("  tap = screen off");
    ln("  any touch wakes");

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CLAUDE", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("  sessions  %u", tama.sessionsTotal);
    ln("  running   %u", tama.sessionsRunning);
    ln("  waiting   %u", tama.sessionsWaiting);
    y += 8;
    spr.setTextColor(p.text, p.bg);
    ln("LINK");
    spr.setTextColor(p.textDim, p.bg);
    ln("  via       %s", dataScenarioName());
    ln("  ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("  last msg  %lus", (unsigned long)age);
    ln("  state     %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "RESPONSE", infoPage);
    if (tama.lastTurnText[0] == 0) {
      spr.setTextColor(p.textDim, p.bg);
      ln("nothing yet.");
      ln("");
      ln("the latest assistant");
      ln("reply will show here");
      ln("once a turn completes.");
    } else {
      uint32_t age = (millis() - tama.lastTurnMs) / 1000;
      spr.setTextColor(p.textDim, p.bg);
      if      (age < 60)   ln("%lus ago", (unsigned long)age);
      else if (age < 3600) ln("%lum ago", (unsigned long)(age/60));
      else                 ln("%luh ago", (unsigned long)(age/3600));
      y += 4;

      // Word-wrap into the same ~38-char column the HUD uses.
      spr.setTextColor(p.text, p.bg);
      const int HW = W / 6 - 1;
      static char wrapBuf[24][WRAP_COLS];
      uint8_t rows = wrapInto(tama.lastTurnText, wrapBuf, 24, HW);
      // Drop into available vertical space below the header.
      int maxRows = (H - y - 4) / 9;
      uint8_t shown = (rows < maxRows) ? rows : (uint8_t)maxRows;
      for (uint8_t i = 0; i < shown; i++) {
        spr.setCursor(4, y); spr.print(wrapBuf[i]); y += 9;
      }
      if (shown < rows) {
        spr.setTextColor(p.textDim, p.bg);
        spr.setCursor(W - 30, H - 10);
        spr.print("...");
      }
    }

  } else if (infoPage == 5) {
    _infoHeader(p, y, "DEVICE", infoPage);

    // The CYD has a TP4056-style charge IC on the USB-C input and a 2:1
    // divider on GPIO34. With no battery attached the divider is floating
    // and the ADC reads garbage anywhere in 0–4 V; only a reading inside
    // a realistic LiPo window means a battery is actually connected.
    float vBat   = M5.Axp.GetBatVoltage();           // volts
    bool  onUsb  = M5.Axp.GetVBusVoltage() > 4.0f;   // always true on CYD (hardcoded)
    bool  battOk = (vBat > 3.0f && vBat < 4.5f);     // realistic LiPo range
    int   pct    = (int)((vBat - 3.2f) * 100.0f);    // 3.2 V = 0 %, 4.2 V = 100 %
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    bool  full     = battOk && vBat > 4.15f;
    bool  charging = battOk && onUsb && !full;

    // Headline state
    spr.setTextSize(2);
    if (battOk) {
      spr.setTextColor(p.text, p.bg);
      spr.setCursor(4, y);
      spr.printf("%d%%", pct);
      spr.setTextSize(1);
      spr.setTextColor(full ? GREEN : (charging ? HOT : p.textDim), p.bg);
      spr.setCursor(60, y + 4);
      spr.print(full ? "full" : (charging ? "charging" : "battery"));
    } else {
      // No battery detected — CYD running entirely off USB. Don't show a
      // misleading percentage from a floating ADC reading.
      spr.setTextColor(GREEN, p.bg);
      spr.setCursor(4, y);
      spr.print("USB");
      spr.setTextSize(1);
      spr.setTextColor(p.textDim, p.bg);
      spr.setCursor(60, y + 4);
      spr.print("powered");
    }
    y += 20;
    spr.setTextSize(1);

    spr.setTextColor(p.textDim, p.bg);
    if (battOk) {
      int vBat_mV = (int)(vBat * 1000);
      ln("  battery  %d.%02dV", vBat_mV / 1000, (vBat_mV % 1000) / 10);
      ln("  state    %s", full ? "full" : (charging ? "charging" : "discharging"));
      ln("  charge IC TP4056-style");
    } else {
      ln("  battery  none attached");
      ln("  charger  ready (TP4056)");
      ln("  connect  LiPo to JST PH2");
    }
    if (onUsb) ln("  usb in   5.0V (host)");
    y += 8;

    spr.setTextColor(p.text, p.bg);
    ln("SYSTEM");
    spr.setTextColor(p.textDim, p.bg);
    if (ownerName()[0]) ln("  owner    %s", ownerName());
    uint32_t up = millis() / 1000;
    ln("  uptime   %luh %02lum", up / 3600, (up / 60) % 60);
    ln("  heap     %uKB", ESP.getFreeHeap() / 1024);
    ln("  bright   %u/4", brightLevel);
    ln("  bt       %s", settings().bt ? (dataBtActive() ? "linked" : "on") : "off");
    ln("  temp     %dC", (int)M5.Axp.GetTempInAXP192());

  } else if (infoPage == 4) {
    _infoHeader(p, y, "SESSIONS", infoPage);
    uint8_t total   = tama.sessionsTotal;
    uint8_t running = tama.sessionsRunning;
    uint8_t waiting = tama.sessionsWaiting;
    uint8_t idle    = (total >= running + waiting) ? total - running - waiting : 0;

    // Big total count
    spr.setTextSize(4);
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(8, y);
    spr.printf("%u", total);
    spr.setTextSize(1);
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(56, y + 8);
    spr.print(total == 1 ? "session" : "sessions");
    y += 40;

    // Visual block of colored dots — one per session, wrapped on 12 cols.
    // Limit to 36 visible (3 rows of 12) — anything beyond gets a "+N" tail.
    const int dotR = 5, dotPitch = 14;
    int dotsX0 = 8, dotsY = y + 4;
    uint8_t shown = total > 36 ? 36 : total;
    for (uint8_t i = 0; i < shown; i++) {
      uint16_t col = (i < running) ? GREEN
                   : (i < running + waiting) ? HOT
                   : p.textDim;
      int dx = dotsX0 + (i % 12) * dotPitch;
      int dy = dotsY   + (i / 12) * dotPitch;
      spr.fillCircle(dx, dy, dotR, col);
    }
    if (total > shown) {
      spr.setTextColor(p.textDim, p.bg);
      spr.setCursor(dotsX0 + 12 * dotPitch + 4, dotsY - 4);
      spr.printf("+%u", total - shown);
    }
    y += 14 * ((shown + 11) / 12) + 14;

    // Legend
    spr.setTextSize(1);
    spr.fillCircle(8,  y + 4, 4, GREEN);    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(18, y + 1); spr.printf("running  %u", running);
    spr.fillCircle(8,  y + 18, 4, HOT);
    spr.setCursor(18, y + 15); spr.printf("waiting  %u", waiting);
    spr.fillCircle(8,  y + 32, 4, p.textDim);
    spr.setCursor(18, y + 29); spr.printf("idle     %u", idle);
    y += 50;

    // Quick recent activity — first three entries from the heartbeat.
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(8, y); spr.print("RECENT");
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    if (tama.nLines == 0) {
      spr.setCursor(8, y); spr.print("(no entries yet)");
    } else {
      uint8_t n = tama.nLines < 3 ? tama.nLines : 3;
      for (uint8_t i = 0; i < n; i++) {
        // newest is at index nLines-1 by convention
        const char* line = tama.lines[tama.nLines - 1 - i];
        spr.setCursor(8, y);
        spr.printf("%.*s", W / 6 - 2, line);
        y += 9;
      }
    }

  } else if (infoPage == 6) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = settings().bt && dataBtActive();

    spr.setTextColor(linked ? GREEN : (settings().bt || !bleReady() ? HOT : p.textDim), p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.print(!bleReady() ? "error" : linked ? "linked" : (settings().bt ? "discover" : "off"));
    spr.setTextSize(1);
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    spr.setTextColor(p.text, p.bg);
    ln("  %s", btName);
    spr.setTextColor(p.textDim, p.bg);
    uint8_t mac[6] = {0};
    btMac(mac);
    ln("  %02X:%02X:%02X:%02X:%02X:%02X",
       mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    y += 8;

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln("  last msg  %lus", (unsigned long)age);
    } else if (settings().bt) {
      spr.setTextColor(p.text, p.bg);
      ln("TO PAIR");
      spr.setTextColor(p.textDim, p.bg);
      ln(" Open Claude desktop");
      ln(" > Developer");
      ln(" > Hardware Buddy");
      y += 4;
      ln(" auto-connects via BLE");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("original by");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("Felix Rieseberg");
    spr.setTextColor(p.textDim, p.bg);
    ln("github.com/anthropics");
    ln("/claude-desktop-buddy");
    y += 8;

    spr.setTextColor(p.textDim, p.bg);
    ln("CYD port by");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("J. Perich");
    spr.setTextColor(p.textDim, p.bg);
    ln("github.com/jdperich/");
    ln("claude-desktop-buddy-cyd");
    y += 8;

    spr.setTextColor(p.textDim, p.bg);
    ln("multi-board fork");
    ln("github.com/alinke/");
    ln("claude-desktop-buddy-esp32");
    y += 8;
    spr.setTextColor(p.text, p.bg);
    ln("%s", BUDDY_BOARD_NAME);
    spr.setTextColor(p.textDim, p.bg);
    ln("v%s  MIT licensed", BUDDY_VERSION);
  }
}


// Greedy word-wrap into fixed-width rows. Continuation rows get a leading
// space. Returns number of rows written.
static uint8_t wrapInto(const char* in, char out[][WRAP_COLS], uint8_t maxRows, uint8_t width) {
  if (width > WRAP_COLS - 1) width = WRAP_COLS - 1;
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;                     // skip leading spaces
    // measure next word
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;              // continuation indent
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    else if (col == 1 && row > 0) {}           // already have the indent space
    // hard-break words that still don't fit
    while (wlen > width - col) {
      uint8_t take = width - col;
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

// ─── Token activity sparkline ────────────────────────────────────────────
// 60-bucket ring; each bucket is 10 s wide so the chart fills out within
// a few minutes of activity instead of taking an hour. The head bucket
// is updated live each frame as tokens stream in.
static const uint8_t  SPARK_N         = 60;
static const uint32_t SPARK_BUCKET_MS = 10000UL;
// Floor for chart scaling so a lone bar in an otherwise-empty chart
// doesn't shoot to the top — small activity should look small.
static const uint16_t SPARK_SCALE_FLOOR = 200;
static uint16_t      sparkBuf[SPARK_N] = {0};
static uint8_t       sparkHead = 0;
static uint32_t      sparkLastTok = 0;
static uint32_t      sparkLastMin = 0;

// sparkHead names the *current* bucket — the rightmost bar in the chart,
// continuously updated as tokens stream in. On a minute roll the head
// advances and the new slot zeroes out. Bridge restarts (cur < lastTok)
// re-baseline without producing a phantom drop.
static void sparkTick() {
  static bool sparkInited = false;
  uint32_t nowBucket = millis() / SPARK_BUCKET_MS;
  uint32_t cur       = stats().tokens;
  // Baseline lazily so the very first frame doesn't push the entire NVS-
  // restored cumulative count into bucket #0.
  if (!sparkInited) {
    sparkLastTok = cur;
    sparkLastMin = nowBucket;
    sparkInited  = true;
    return;
  }
  if (cur < sparkLastTok) sparkLastTok = cur;

  if (nowBucket != sparkLastMin) {
    uint32_t advance = nowBucket - sparkLastMin;
    if (advance > SPARK_N) advance = SPARK_N;
    for (uint32_t i = 0; i < advance; i++) {
      sparkHead = (sparkHead + 1) % SPARK_N;
      sparkBuf[sparkHead] = 0;
    }
    sparkLastTok = cur;
    sparkLastMin = nowBucket;
  }
  uint32_t delta = cur - sparkLastTok;
  if (delta > 65535) delta = 65535;
  sparkBuf[sparkHead] = (uint16_t)delta;            // live-update visible bar
}

static void drawSparkline(int x, int y, int w, int h) {
  const Palette& p = characterPalette();
  uint16_t maxV = 0;
  for (uint8_t i = 0; i < SPARK_N; i++) if (sparkBuf[i] > maxV) maxV = sparkBuf[i];
  if (maxV == 0) return;                              // nothing to plot — leave the area clean
  uint16_t scaleMax = maxV < SPARK_SCALE_FLOOR ? SPARK_SCALE_FLOOR : maxV;

  spr.fillRect(x, y, w, h, p.bg);
  spr.drawFastHLine(x, y + h - 1, w, p.textDim);

  // Tiny title above the chart, plus the current max in the corner
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(x, y - 9);
  spr.print("tok/min");

  for (uint8_t i = 0; i < SPARK_N; i++) {
    // sparkHead is the newest (rightmost) bucket; iterate from oldest on
    // the left (sparkHead+1, the slot about to be overwritten next minute)
    // through to sparkHead itself on the right.
    uint8_t  idx = (sparkHead + 1 + i) % SPARK_N;
    uint16_t v   = sparkBuf[idx];
    if (v == 0) continue;
    int bh = (int)((uint32_t)v * (h - 2) / scaleMax);
    if (bh < 1) bh = 1;
    int bx = x + i * (w / SPARK_N);
    spr.drawFastVLine(bx, y + h - 1 - bh, bh, p.body);
  }
}

// ─── Persistent status strip (top 14 px of the home screen) ─────────────
// A tiny BLE link dot, current session counts, and tokens-today. Drawn
// AFTER the pet tick so the pet's clear rect doesn't wipe it. The pet's
// own overlay row (Zzz / hearts at y≈12) is partially under the strip;
// since overlays animate and the strip is informational, the strip
// painting on top is the intended z-order.
static const int STATUS_H = 14;

// Claude sparkle mark — 4 main rays + 4 short diagonals at radius r,
// drawn around (cx, cy). Used in the status strip (r=3) and the boot
// splash (r=20 with extra-thick rays via drawSparkleBig).
static void drawSparkle(int cx, int cy, int r, uint16_t col) {
  spr.drawFastVLine(cx, cy - r, 2 * r + 1, col);
  spr.drawFastHLine(cx - r, cy, 2 * r + 1, col);
  int d = (r * 2) / 3;
  spr.drawLine(cx - d, cy - d, cx + d, cy + d, col);
  spr.drawLine(cx + d, cy - d, cx - d, cy + d, col);
}

// ─── Tooling helpers (deferred to here so they see all the constants) ───
// Draws the boot splash to the sprite — called from setup() for the real
// boot and from cmdSplash() for tools/capture_readme.py.
static void renderBootSplash() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextDatum(MC_DATUM);

  // Concentric coral sparkles — Claude brand mark.
  drawSparkle(W / 2, vy(100), 22, CLAUDE_CORAL);
  drawSparkle(W / 2, vy(100), 16, CLAUDE_CORAL);
  drawSparkle(W / 2, vy(100),  8, CLAUDE_CORAL);

  spr.setTextSize(4);
  spr.setTextColor(CLAUDE_CORAL, p.bg);
  spr.drawString("Claude", W / 2, vy(162));

  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.drawString("Hardware Buddy", W / 2, vy(192));
  spr.drawString(BUDDY_BOARD_NAME,  W / 2, vy(204));

  spr.setTextSize(2);
  if (ownerName()[0]) {
    char greet[40];
    snprintf(greet, sizeof(greet), "Hi %s!", ownerName());
    spr.setTextColor(p.text, p.bg);
    spr.drawString(greet, W / 2, vy(250));
    char pet[40];
    snprintf(pet, sizeof(pet), "Pet: %s", petName());
    spr.setTextColor(p.body, p.bg);
    spr.drawString(pet, W / 2, vy(274));
  } else {
    spr.setTextColor(p.body, p.bg);
    spr.drawString("Hello!", W / 2, vy(262));
  }
  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(1);
}

void cmdSplash() {
  spr.render(renderBootSplash);
  splashHoldUntilMs = millis() + 8000;
}

void cmdClearPrompt() {
  tama.promptId[0]   = 0;
  tama.promptTool[0] = 0;
  tama.promptHint[0] = 0;
  tama.promptChoiceN = 0;
  responseSent       = false;
  lastPromptId[0]    = 0;
}

// Streams the current frame over USB Serial as a series of base64-encoded
// rows wrapped in "SCR-BEGIN W H BPP" / "SCR <b64>" / "SCR-END" markers.
// tools/snap.py on the host reads the stream and saves a PNG. Triggered by
// {"cmd":"screenshot"} on Serial. Rows are physical panel rows: 8 bpp
// (RGB332) on boards without PSRAM, 16 bpp (RGB565, byte-swapped as
// LovyanGFX stores it) on the rest.
#include <mbedtls/base64.h>
static bool s_scrFail = false;
static void _scrRow(const uint8_t* row, size_t len) {
  if (s_scrFail) return;
  static char b64[2200];               // 800 px * 2 bytes -> 2136 chars
  size_t outLen = 0;
  if (mbedtls_base64_encode((unsigned char*)b64, sizeof(b64) - 1, &outLen, row, len) != 0) {
    Serial.println("SCR-ERR enc-fail");
    s_scrFail = true;
    return;
  }
  b64[outLen] = 0;
  Serial.print("SCR ");
  Serial.println(b64);
}
void cmdScreenshot() {
  if (!spr.bands()) { Serial.println("SCR-ERR no-sprite"); return; }
  Serial.printf("SCR-BEGIN %d %d %d\n", spr.physWidth(), spr.physHeight(), spr.colorDepth());
  s_scrFail = false;
  spr.forEachRow(_scrRow);
  if (!s_scrFail) Serial.println("SCR-END");
}

static void drawSparkline(int x, int y, int w, int h);   // fwd decl — defined above
static void drawStatusStrip() {
  const Palette& p = characterPalette();
  spr.fillRect(0, 0, W, STATUS_H, p.bg);
  spr.drawFastHLine(0, STATUS_H, W, CLAUDE_CORAL);
  spr.setTextSize(1);

  // Claude sparkle, color-coded by BLE link state:
  //   coral = secure (encrypted)  · amber = BLE open · green = USB only
  //   · dim = offline
  // Over USB there's no BLE link at all, but a desktop bridge feeding
  // heartbeats over serial is just as connected.
  uint16_t markCol = !dataConnected() ? p.textDim
                    : !bleConnected() ? GREEN
                    : bleSecure()    ? CLAUDE_CORAL
                                     : 0xFD20;   // amber
  drawSparkle(6, STATUS_H / 2, 3, markCol);

  // Session counts: "run N wait M". Numbers tinted by state (green/HOT/dim).
  int x = 14;
  spr.setCursor(x, 3); spr.setTextColor(p.textDim, p.bg); spr.print("run ");
  x += 24;
  spr.setCursor(x, 3);
  spr.setTextColor(tama.sessionsRunning ? p.text : p.textDim, p.bg);
  spr.printf("%u", tama.sessionsRunning);
  x += 6;
  spr.setCursor(x, 3); spr.setTextColor(p.textDim, p.bg); spr.print(" wait ");
  x += 36;
  spr.setCursor(x, 3);
  spr.setTextColor(tama.sessionsWaiting ? HOT : p.textDim, p.bg);
  spr.printf("%u", tama.sessionsWaiting);

  // Inline activity sparkline between the labels and tokens — 70x12 on the
  // CYD, wider when the canvas is.
  int sparkW = W - 170; if (sparkW < 70) sparkW = 70;
  drawSparkline(86, 1, sparkW, 12);

  // Tokens today, right-aligned
  uint32_t t = tama.tokensToday;
  char buf[16];
  if      (t >= 1000000) snprintf(buf, sizeof(buf), "%lu.%luM", (unsigned long)(t/1000000), (unsigned long)((t/100000)%10));
  else if (t >= 1000)    snprintf(buf, sizeof(buf), "%lu.%luK", (unsigned long)(t/1000),    (unsigned long)((t/100)%10));
  else                   snprintf(buf, sizeof(buf), "%lu",      (unsigned long)t);
  int bw = (int)strlen(buf) * 6;
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(W - bw - 4, 3);
  spr.print(buf);
}

// ─── Tool icons (36×36 vector glyphs drawn with TFT primitives) ──────────
// Programmatic rather than raster bitmaps so they scale crisply and don't
// bloat flash — each one is ~10–15 line/circle calls.
static void iconBash(int x, int y, uint16_t fg) {
  spr.drawRoundRect(x + 1, y + 4, 34, 28, 4, fg);
  // ">" chevron (double-pixel for weight)
  for (int o = 0; o < 2; o++) {
    spr.drawLine(x + 9 + o, y + 12, x + 16 + o, y + 18, fg);
    spr.drawLine(x + 9 + o, y + 24, x + 16 + o, y + 18, fg);
  }
  // "_" underscore
  spr.drawFastHLine(x + 18, y + 24, 9, fg);
  spr.drawFastHLine(x + 18, y + 25, 9, fg);
}
static void iconRead(int x, int y, uint16_t fg) {
  int cx = x + 18, cy = y + 18;
  spr.drawEllipse(cx, cy, 14, 8, fg);
  spr.drawCircle(cx, cy, 5, fg);
  spr.fillCircle(cx, cy, 2, fg);
}
static void iconWrite(int x, int y, uint16_t fg) {
  // File outline with folded corner + a "+"
  spr.drawRect(x + 5, y + 3, 22, 28, fg);
  spr.drawLine(x + 22, y + 3,  x + 27, y + 8, fg);
  spr.drawFastVLine(x + 22, y + 3, 6, fg);
  spr.drawFastHLine(x + 22, y + 8, 6, fg);
  spr.drawFastVLine(x + 16, y + 14, 12, fg);
  spr.drawFastHLine(x + 10, y + 20, 12, fg);
}
static void iconEdit(int x, int y, uint16_t fg) {
  // Diagonal pencil from top-right to bottom-left
  for (int o = 0; o < 2; o++) {
    spr.drawLine(x + 28 - o, y + 5,  x + 8 - o,  y + 25, fg);
    spr.drawLine(x + 29 - o, y + 6,  x + 9 - o,  y + 26, fg);
  }
  // pencil tip + eraser stub
  spr.drawLine(x + 6, y + 28, x + 12, y + 26, fg);
  spr.drawLine(x + 6, y + 28, x + 10, y + 22, fg);
  spr.drawFastHLine(x + 26, y + 4, 5, fg);
  spr.drawFastHLine(x + 27, y + 3, 4, fg);
}
static void iconWeb(int x, int y, uint16_t fg) {
  int cx = x + 18, cy = y + 18, r = 13;
  spr.drawCircle(cx, cy, r, fg);
  spr.drawFastHLine(cx - r, cy, 2 * r + 1, fg);
  spr.drawFastHLine(cx - 12, cy - 6, 25, fg);
  spr.drawFastHLine(cx - 12, cy + 6, 25, fg);
  spr.drawFastVLine(cx, cy - r, 2 * r + 1, fg);
  spr.drawEllipse(cx, cy, 6, r, fg);
}
static void iconSearch(int x, int y, uint16_t fg) {
  int cx = x + 13, cy = y + 13, r = 9;
  spr.drawCircle(cx, cy, r, fg);
  spr.drawCircle(cx, cy, r - 1, fg);
  // handle
  for (int o = 0; o < 3; o++) {
    spr.drawLine(cx + 6, cy + 6 + o, x + 30, y + 30 + o, fg);
  }
}
static void iconDefault(int x, int y, uint16_t fg) {
  int cx = x + 18, cy = y + 18;
  spr.drawCircle(cx, cy, 14, fg);
  spr.drawCircle(cx, cy, 13, fg);
  spr.setTextSize(3);
  spr.setTextColor(fg, characterPalette().bg);
  spr.setCursor(cx - 8, cy - 10);
  spr.print("?");
  spr.setTextSize(1);
}

// Case-insensitive prefix match — keeps the dispatch readable.
static bool _toolIs(const char* tool, const char* prefix) {
  for (int i = 0; prefix[i]; i++) {
    char a = tool[i];        if (a == 0) return false;
    char b = prefix[i];
    if (a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
    if (b >= 'A' && b <= 'Z') b = b - 'A' + 'a';
    if (a != b) return false;
  }
  return true;
}
static void drawToolIcon(int x, int y, const char* tool, uint16_t fg) {
  if (!tool || !tool[0])        { iconDefault(x, y, fg); return; }
  if (_toolIs(tool, "bash"))    { iconBash(x, y, fg); return; }
  if (_toolIs(tool, "read"))    { iconRead(x, y, fg); return; }
  if (_toolIs(tool, "write"))   { iconWrite(x, y, fg); return; }
  if (_toolIs(tool, "edit") ||
      _toolIs(tool, "multiedit") ||
      _toolIs(tool, "notebookedit")) { iconEdit(x, y, fg); return; }
  if (_toolIs(tool, "websearch")||
      _toolIs(tool, "grep") ||
      _toolIs(tool, "glob"))    { iconSearch(x, y, fg); return; }
  if (_toolIs(tool, "webfetch")){ iconWeb(x, y, fg); return; }
  if (_toolIs(tool, "ls"))      { iconRead(x, y, fg); return; }
  iconDefault(x, y, fg);
}

// ─── Multi-choice question (forward-compatible) ─────────────────────────
// When tama.promptChoiceN > 0, the prompt is a multi-choice question
// rather than a binary approve/deny. The protocol extension this listens
// for (prompt.choices[]) isn't sent by the current desktop bridge, but
// the moment it is, this UI lights up automatically.
static bool isMultiChoicePrompt() {
  return tama.promptId[0] && tama.promptChoiceN > 0;
}

static const int MCQ_TOP   = 84;
static const int MCQ_GAP   = 6;
// 44 px cards on the CYD; shorter when N cards wouldn't fit the canvas.
static int mcqRowH() {
  int n = tama.promptChoiceN ? tama.promptChoiceN : 1;
  int h = (H - MCQ_TOP - 20) / n - MCQ_GAP;
  return h < 44 ? h : 44;
}

static void drawMultiChoice() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);

  // Header — same icon + tool name layout as the approval screen.
  drawToolIcon(4, 6, tama.promptTool, p.body);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  spr.setTextSize(2);
  spr.setTextColor(waited >= 10 ? HOT : p.textDim, p.bg);
  spr.setCursor(48, 10);
  spr.printf("choose? %lus", (unsigned long)waited);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(48, 32);
  spr.print(tama.promptTool);

  // Hint wraps to two lines below the icon.
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  const int HW = W / 6 - 1;
  int hlen = strlen(tama.promptHint);
  int y = 56;
  for (int i = 0; i < 2 && i * HW < hlen; i++) {
    spr.setCursor(4, y);
    spr.printf("%.*s", HW, tama.promptHint + i * HW);
    y += 9;
  }

  // Choice cards — full-width tappable rows with a coral number badge.
  const int MCQ_ROW_H = mcqRowH();
  for (uint8_t i = 0; i < tama.promptChoiceN; i++) {
    int y0 = MCQ_TOP + i * (MCQ_ROW_H + MCQ_GAP);
    spr.fillRoundRect(8, y0, W - 16, MCQ_ROW_H, 6, p.bg);
    spr.drawRoundRect(8, y0, W - 16, MCQ_ROW_H, 6, CLAUDE_CORAL);
    // Number badge
    spr.fillCircle(28, y0 + MCQ_ROW_H / 2, 14, CLAUDE_CORAL);
    spr.setTextSize(2);
    spr.setTextColor(CLAUDE_INK, CLAUDE_CORAL);
    spr.setCursor(23, y0 + MCQ_ROW_H / 2 - 7);
    spr.printf("%u", i + 1);
    // Label
    spr.setTextSize(1);
    spr.setTextColor(p.text, p.bg);
    int maxChars = (W - 64) / 6;
    spr.setCursor(50, y0 + MCQ_ROW_H / 2 - 4);
    spr.printf("%.*s", maxChars, tama.promptChoiceLabels[i]);
  }

  // Footer hint + "sent" state
  spr.setTextSize(1);
  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(8, H - 14);
    spr.print("sent...");
  } else {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(8, H - 14);
    spr.print("tap a choice to answer");
  }
}

// Set when the user answers a locally-injected (id starting with "test-")
// multi-choice prompt. After the delay we clear the fake prompt ourselves
// since no bridge is going to do it for us.
static uint32_t testPromptClearAt = 0;

// Touch handler for the multi-choice modal — called from the gesture
// pass. Returns true (event consumed) if the tap hit a choice row.
static bool handleMultiChoiceTap(const HalTouchEvent& evt) {
  if (!isMultiChoicePrompt() || responseSent) return false;
  const int MCQ_ROW_H = mcqRowH();
  for (uint8_t i = 0; i < tama.promptChoiceN; i++) {
    int y0 = MCQ_TOP + i * (MCQ_ROW_H + MCQ_GAP);
    int y1 = y0 + MCQ_ROW_H;
    if (evt.ey < y0 || evt.ey > y1) continue;
    if (evt.ex < 8 || evt.ex > W - 8) continue;
    // Match — send choice back. The cmd name `choice` is a forward
    // extension; desktop bridges that don't understand it will drop the
    // ack but the user still gets local feedback.
    char cmd[160];
    snprintf(cmd, sizeof(cmd),
             "{\"cmd\":\"choice\",\"id\":\"%s\",\"choice\":\"%s\"}",
             tama.promptId, tama.promptChoiceIds[i]);
    sendCmd(cmd);
    responseSent = true;
    uint32_t tookS = (millis() - promptArrivedMs) / 1000;
    statsOnApproval(tookS);
    SFX(SFX_APPROVED);
    if (tookS < 5) { triggerOneShot(P_HEART, 2000); SFX(SFX_HEART); }
    // Test prompts have no real bridge to clean them up — schedule a
    // local dismiss so the "sent..." footer is visible briefly and then
    // the user returns to the home screen.
    if (strncmp(tama.promptId, "test-", 5) == 0) {
      testPromptClearAt = millis() + 1500;
    }
    return true;
  }
  return false;
}

// Where the approve / deny split sits — also the HAL's A/B zone boundary,
// so the visible split and the tap zones always agree.
static int approvalDivX() { return L.paneX + (int)(L.paneW * 0.62f); }

static void drawApproval() {
  const Palette& p = characterPalette();
  // Portrait: a band across the bottom (150 px on the CYD, more on taller
  // canvases). Landscape: the whole right pane.
  const int x0  = L.paneX, w = L.paneW;
  const int top = L.approvalY;
  const int AREA = H - top;
  spr.fillRect(x0, top, w, AREA, p.bg);
  spr.drawFastHLine(x0, top, w, p.textDim);

  // Tool icon at top-left, tinted with the body color so it inherits the
  // theme; "approve? Ns" + tool name sit to its right.
  drawToolIcon(x0 + 4, top + 6, tama.promptTool, p.body);

  int textX = x0 + 48;
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  spr.setTextSize(2);
  spr.setTextColor(waited >= 10 ? HOT : p.textDim, p.bg);
  spr.setCursor(textX, top + 8);
  spr.printf("approve? %lus", (unsigned long)waited);

  // Tool name big, on a second line to the right of the icon.
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(2);
  spr.setCursor(textX, top + 28);
  spr.print(tama.promptTool);

  // Action row: the touch layer maps everything left of divX to approve,
  // the right strip to deny. Make that split visible.
  int by = H - 30;
  int divX = approvalDivX();

  // Hint: size-1, full pane width below the icon, as many lines as fit
  // above the action row (4 on the CYD).
  int y = top + 52;
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  const int HW = (w - 8) / 6;
  const int maxLines = (by - 8 - y) / 9;
  int hlen = strlen(tama.promptHint);
  for (int i = 0; i < maxLines && i * HW < hlen; i++) {
    spr.setCursor(x0 + 4, y);
    spr.printf("%.*s", HW, tama.promptHint + i * HW);
    y += 9;
  }

  spr.drawFastHLine(x0, by - 8, w, p.textDim);
  spr.drawFastVLine(divX, by - 8, 38, p.textDim);
  spr.setTextSize(2);
  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(x0 + 4, by);
    spr.print("sent...");
  } else {
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(x0 + 8, by);
    spr.print("approve");
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(divX + 8, by);
    spr.print("deny");
    spr.setTextSize(1);
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(x0 + 8, by + 18);
    spr.print(L.land ? "tap this side or pet" : "tap this side");
    spr.setCursor(divX + 8, by + 18);
    spr.print("tap >");
  }
  spr.setTextSize(1);
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 2, y, 2, col);
    spr.fillCircle(x + 2, y, 2, col);
    spr.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  } else {
    spr.drawCircle(x - 2, y, 2, col);
    spr.drawCircle(x + 2, y, 2, col);
    spr.drawLine(x - 4, y + 1, x, y + 5, col);
    spr.drawLine(x + 4, y + 1, x, y + 5, col);
  }
}

static void drawPetStats(const Palette& p) {
  PANE_GEOMETRY
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 16;

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y - 2); spr.print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(54 + i * 16, y + 2, i < mood, moodCol);

  y += 20;
  spr.setCursor(6, y - 2); spr.print("fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = 38 + i * 9;
    if (i < fed) spr.fillCircle(px, y + 1, 2, p.body);
    else spr.drawCircle(px, y + 1, 2, p.textDim);
  }

  y += 20;
  spr.setCursor(6, y - 2); spr.print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = 54 + i * 13;
    if (i < en) spr.fillRect(px, y - 2, 9, 6, enCol);
    else spr.drawRect(px, y - 2, 9, 6, p.textDim);
  }

  y += 24;
  spr.fillRoundRect(6, y - 2, 42, 14, 3, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(11, y + 1); spr.printf("Lv %u", stats().level);

  y += 20;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y);
  spr.printf("approved %u", stats().approvals);
  spr.setCursor(6, y + 10);
  spr.printf("denied   %u", stats().denials);
  uint32_t nap = stats().napSeconds;
  spr.setCursor(6, y + 20);
  spr.printf("napped   %luh%02lum", nap/3600, (nap/60)%60);
  auto tokFmt = [&](const char* label, uint32_t v, int yPx) {
    spr.setCursor(6, yPx);
    if (v >= 1000000)   spr.printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr.printf("%s%lu.%luK", label, v/1000, (v/100)%10);
    else                spr.printf("%s%lu", label, v);
  };
  tokFmt("tokens   ", stats().tokens, y + 30);
  tokFmt("today    ", tama.tokensToday, y + 40);
}

static void drawPetHowTo(const Palette& p) {
  PANE_GEOMETRY
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(6, y); spr.print(s); y += 9;
  };
  auto gap = [&]() { y += 4; };

  y += 12;  // room for the PET header drawn by drawPet()

  ln(p.body,    "MOOD");
  ln(p.textDim, " approve fast = up");
  ln(p.textDim, " deny lots = down"); gap();

  ln(p.body,    "FED");
  ln(p.textDim, " 50K tokens =");
  ln(p.textDim, " level up + confetti"); gap();

  ln(p.body,    "ENERGY");
  ln(p.textDim, " idle = refills");
  ln(p.textDim, " (no motion sensor)"); gap();

  ln(p.textDim, "idle 30s = off");
  ln(p.textDim, "any touch = wake"); gap();

  ln(p.textDim, "tap L/R = next/page");
  ln(p.textDim, "hold L = menu");
}

void drawPet() {
  PANE_GEOMETRY
  const Palette& p = characterPalette();
  int y = 70;

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  // Claude branding strip at top-centre — painted after the buddy peek
  // tick so it survives the buddy's clear rect. (Landscape keeps the pet
  // full size in its own pane, so there's no peek strip to brand.)
  if (!L.land) drawClaudeBadgeCentered(2);

  // Header on top of whichever page drew — title left, counter right.
  // No possessive — splash already uses "Hi <owner>! / Pet: <pet>", so
  // matching that voice keeps the branding consistent.
  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y + 2);
  if (ownerName()[0]) {
    spr.printf("%s (%s)", petName(), ownerName());
  } else {
    spr.print(petName());
  }
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y + 2);
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
}

// One-line "what Claude is doing" indicator, painted in Claude coral
// immediately above the HUD. Uses the heartbeat's `msg` field — a
// human-readable summary like "approve: Bash" or "generating reply".
// Skipped when there's an active approval (the approval panel owns the
// whole bottom of the screen).
static const int ACTIVITY_AREA = 14;
static void drawActivityLine() {
  const Palette& p = characterPalette();
  const int x0 = L.paneX, w = L.paneW;
  int y = L.actY;
  spr.fillRect(x0, y, w, ACTIVITY_AREA, p.bg);
  spr.drawFastHLine(x0, y, w, p.textDim);
  if (tama.promptId[0]) return;          // approval overrides
  spr.setTextSize(1);
  spr.setTextColor(CLAUDE_CORAL, p.bg);
  spr.setCursor(x0 + 4, y + 3);
  if (tama.msg[0]) {
    spr.printf("* %.*s", w / 6 - 3, tama.msg);
  } else if (!dataConnected()) {
    spr.setTextColor(p.textDim, p.bg);
    spr.print("* waiting for Claude...");
  } else {
    spr.setTextColor(p.textDim, p.bg);
    spr.print("* idle");
  }
}

void drawHUD() {
  if (tama.promptId[0]) {
    if (isMultiChoicePrompt()) drawMultiChoice();
    else                       drawApproval();
    return;
  }
  const Palette& p = characterPalette();
  // Upstream's CYD HUD: 240px wide fits ~38 size-1 chars, 15 visible rows
  // with ~10 px of bottom padding so the lowest glyph clears the panel's
  // overscan band. Here the row count and width come from the pane.
  const int x0 = L.paneX, w = L.paneW;
  const int LH = 8;
  const int top = L.hudY;
  const int AREA = H - top;
  int SHOW = (AREA - 10) / LH;
  if (SHOW > 48) SHOW = 48;
  int WIDTH = (w - 8) / 6;
  if (WIDTH > WRAP_COLS - 1) WIDTH = WRAP_COLS - 1;
  spr.fillRect(x0, top, w, AREA, p.bg);
  spr.setTextSize(1);

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  if (tama.nLines == 0) {
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(x0 + 4, H - LH - 2);
    spr.print(tama.msg);
    return;
  }

  // Wrap all transcript lines into a flat display buffer. Track which
  // transcript index each display row came from, so we can dim older ones.
  static const int MAX_DISP = 64;
  static char disp[MAX_DISP][WRAP_COLS];
  static uint8_t srcOf[MAX_DISP];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < MAX_DISP; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], MAX_DISP - nDisp, WIDTH);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }

  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int end = (int)nDisp - msgScroll;
  int start = end - SHOW; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    spr.setTextColor(fresh ? p.text : p.textDim, p.bg);
    spr.setCursor(x0 + 4, top + 2 + i * LH);
    spr.print(disp[row]);
  }
  if (msgScroll > 0) {
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(x0 + w - 18, H - LH - 2);
    spr.printf("-%u", msgScroll);
  }
}

#if BUDDY_ASK_CLAUDE
// ─── Ask Claude — standalone over WiFi ──────────────────────────────────
// One-tap preset prompts that POST to the Anthropic Messages API and
// stream the reply back. Opens from the main menu's "ask claude" entry;
// closes via the X corner or after a finished response (tap anywhere).
struct AskPreset { const char* label; const char* prompt; };
static const AskPreset PRESETS[] = {
  { "pep talk",    "Give me a brief, encouraging two-sentence pep talk for someone debugging an ESP32 project. Be specific and warm." },
  { "mindfulness", "Give me a 30-second mindfulness exercise as three short numbered steps. No preamble." },
  { "fun fact",    "Tell me one surprising fun fact about engineering, in two sentences." },
  { "desk tip",    "Suggest one specific small thing I could tidy or improve at my desk right now, in one sentence." },
};
static const uint8_t PRESET_N = sizeof(PRESETS) / sizeof(PRESETS[0]);

static bool     askOpen        = false;
static bool     askPickerOpen  = false;
static uint8_t  askPickerSel   = 0;
static uint16_t lastAskGen     = 0;
static uint8_t  askLinesScroll = 0;   // future use for response paging

// Preset rows: 56 px on the CYD, shorter on canvases under ~300 px tall.
static int askRowH() {
  int h = (H - 50 - 24) / 4;
  return h < 56 ? h : 56;
}

static void openAsk() {
  askOpen        = true;
  askPickerOpen  = true;
  askPickerSel   = 0;
  askLinesScroll = 0;
  lastAskGen     = 0;
  askReset();
  characterInvalidate();
}

static void closeAsk() {
  askOpen        = false;
  askPickerOpen  = false;
  askReset();
  WiFi.disconnect(true, false);   // free the radio for BLE smoothness
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
  applyDisplayMode();
}

static void drawAskHeader(const Palette& p, const char* subtitle) {
  spr.fillRect(0, 0, W, 22, p.body);
  spr.setTextSize(2);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(6, 4);
  spr.print("Ask Claude");
  spr.setTextSize(1);
  spr.setCursor(W - 16, 8);
  spr.print("X");                                  // close hint in corner
  if (subtitle && *subtitle) {
    spr.setTextColor(p.bg, p.body);
    spr.setCursor(W - 6 * (int)strlen(subtitle) - 22, 8);
    spr.print(subtitle);
  }
}

static void drawAskPicker() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  drawAskHeader(p, "");
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(8, 30);
  spr.print("Pick a prompt — tap a row");

  // Show preset rows: each is a tappable card.
  const int rowY0 = 50;
  const int rowH  = askRowH();
  for (uint8_t i = 0; i < PRESET_N; i++) {
    int y = rowY0 + i * rowH;
    spr.fillRoundRect(8, y, W - 16, rowH - 8, 6, p.bg);
    spr.drawRoundRect(8, y, W - 16, rowH - 8, 6, p.textDim);
    spr.setTextSize(2);
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(16, y + (rowH >= 50 ? 6 : 3));
    spr.print(PRESETS[i].label);
    // Truncated prompt preview underneath
    spr.setTextSize(1);
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(16, y + (rowH - 20 < 28 ? rowH - 20 : 28));
    int maxC = (W - 32) / 6;
    char buf[96];
    snprintf(buf, sizeof(buf), "%s", PRESETS[i].prompt);
    int blen = (int)strlen(buf);
    if (blen > maxC) { buf[maxC - 3] = '.'; buf[maxC - 2] = '.'; buf[maxC - 1] = '.'; buf[maxC] = 0; }
    spr.print(buf);
  }

  // Warning band at the bottom if creds are missing
  if (!wifiCredsPresent() || !apiKeyPresent()) {
    int y = H - 22;
    spr.fillRect(0, y, W, 22, HOT);
    spr.setTextColor(0xFFFF, HOT);
    spr.setTextSize(1);
    spr.setCursor(6, y + 6);
    spr.print(!wifiCredsPresent() ? "Set WiFi in Settings first" : "Set API key in Settings first");
  }
}

static void drawAskResponse() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);

  // Header with state-aware subtitle
  const char* st = "";
  AskStateE state = askState();
  if      (state == ASK_WIFI_CONNECTING) st = "wifi...";
  else if (state == ASK_POSTING)         st = "asking...";
  else if (state == ASK_STREAMING)       st = "streaming";
  else if (state == ASK_DONE)            st = "done";
  else if (state == ASK_ERROR)           st = "error";
  drawAskHeader(p, st);

  int y = 28;
  if (state == ASK_ERROR) {
    spr.setTextSize(2);
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(8, y);
    spr.print("error");
    y += 22;
    spr.setTextSize(1);
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(8, y);
    spr.print(askError());
    spr.setCursor(8, H - 20);
    spr.print("tap anywhere to close");
    return;
  }

  // Elapsed timer
  uint32_t ms = askElapsedMs();
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(8, y);
  spr.printf("%lu.%lus elapsed", (unsigned long)(ms/1000), (unsigned long)((ms%1000)/100));
  y += 12;

  // Word-wrap the growing response into the rest of the screen.
  spr.setTextColor(p.text, p.bg);
  const int HW = W / 6 - 1;
  static char wrap[40][WRAP_COLS];
  uint8_t rows = wrapInto(askResponse(), wrap, 40, HW);
  int avail = (H - y - 22) / 9;             // leave 22 px footer
  int start = (rows > avail) ? rows - avail : 0;
  for (uint8_t i = 0; start + i < rows && (int)i < avail; i++) {
    spr.setCursor(8, y); spr.print(wrap[start + i]); y += 9;
  }

  // Footer hint
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(8, H - 18);
  if (state == ASK_DONE) {
    spr.print("tap to close, or pick new prompt");
  } else if (state == ASK_STREAMING) {
    spr.print("streaming...");
  } else {
    spr.print("connecting...");
  }
}

// Returns true if it consumed the touch — main loop then suppresses BtnA/B.
static bool askHandleTouch(const HalTouchEvent& evt) {
  // X close in top-right corner of ANY ask sub-screen
  if (evt.ex >= W - 30 && evt.ey < 22) { closeAsk(); return true; }

  if (askPickerOpen) {
    const int rowY0 = 50;
    const int rowH  = askRowH();
    for (uint8_t i = 0; i < PRESET_N; i++) {
      int y0 = rowY0 + i * rowH;
      int y1 = y0 + rowH - 8;
      if (evt.ey >= y0 && evt.ey <= y1 && evt.ex >= 8 && evt.ex <= W - 8) {
        if (!wifiCredsPresent() || !apiKeyPresent()) return true;   // ignore tap, banner already shown
        askPickerSel = i;
        askPickerOpen = false;
        askStart(PRESETS[i].prompt);
        return true;
      }
    }
    return true;
  }

  // Response shown — tap anywhere closes when done or errored
  AskStateE st = askState();
  if (st == ASK_DONE || st == ASK_ERROR) {
    closeAsk();
    return true;
  }
  return true;
}

#endif  // BUDDY_ASK_CLAUDE

// ─── Buddy switcher (full-screen preview) ────────────────────────────────
// The settings panel covers the buddy while you're cycling species, which
// makes the "ascii pet" stepper useless for actually picking one. This
// modal gives the buddy the whole screen with arrows to step left/right
// and an X to close. Selection is committed on close — abandon by leaving
// the device idle if you change your mind (we don't persist until close).
static bool    bswOpen   = false;
static uint8_t bswIdx    = 0;     // 0..N-1 species, N = GIF (if available)
static uint8_t bswEntryIdx = 0;   // remembered for cancel-restore on close

// Where the pet renders. Home: the pet region from the layout (top band in
// portrait, vertically centred in the left pane in landscape). Switcher:
// centred on the whole screen.
static int bswPetYOff() { return L.land ? 30 : 0; }
static void applyPetGeometry(bool switcher) {
  if (switcher) {
    buddySetGeometry(CX, 0, W, bswPetYOff(), L.petScale);
    int nameY = L.land ? H - 44 : 200;
    characterSetArea(0, 22, W, nameY - 40);
    return;
  }
  int yOff = 0;
  if (L.land) {
    yOff = L.petY + (L.petH - buddyHeight(L.petScale)) / 2;
    if (yOff < L.petY) yOff = L.petY;
  }
  buddySetGeometry(L.petX + L.petW / 2, L.petX, L.petW, yOff, L.petScale);
  characterSetArea(L.petX, L.petY, L.petW, L.land ? L.petH : 140);
}

static void openBuddySwitcher() {
  applyPetGeometry(true);
  uint8_t n = buddySpeciesCount();
  bswEntryIdx = buddyMode ? buddySpeciesIdx() : n;
  bswIdx = bswEntryIdx;
  bswOpen = true;
  characterInvalidate();
  buddyInvalidate();
}

// ─── Direct-open command hooks for tools/capture_readme.py ──────────────
// Skip the tap-dance through menus / long-presses when scripting
// screenshots. Each just sets the open-flag (or calls the existing
// open function) so the next frame renders the panel.
void cmdOpenMenu()     { menuOpen = true;  menuSel = 0; }
void cmdOpenSettings() { settingsOpen = true; settingsSel = 0; menuOpen = false; }
void cmdOpenReset()    { resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; settingsOpen = false; }
#if BUDDY_ASK_CLAUDE
void cmdOpenAsk()      { menuOpen = false; openAsk(); }
#else
void cmdOpenAsk()      {}
#endif
void cmdOpenBuddies()  { menuOpen = false; openBuddySwitcher(); }
void cmdOpenInfo(uint8_t page) {
  // Close any open overlays first so the Info page renders cleanly
  // instead of being painted over by the menu/settings/etc.
  menuOpen = settingsOpen = resetOpen = false;
#if BUDDY_ASK_CLAUDE
  askOpen  = false; askPickerOpen = false; askReset();
#endif
  if (bswOpen) { bswOpen = false; applyPetGeometry(false); }
  if (page >= INFO_PAGES) page = 0;
  displayMode = DISP_INFO;
  infoPage    = page;
  applyDisplayMode();
  characterInvalidate();
}
void cmdSetRotation(uint8_t r) {
  rotationSave(r);
  Serial.flush();
  delay(100);
  ESP.restart();
}
void cmdCloseAll() {
  menuOpen = settingsOpen = resetOpen = false;
#if BUDDY_ASK_CLAUDE
  askOpen  = false; askPickerOpen = false; askReset();
#endif
  if (bswOpen) { bswOpen = false; applyPetGeometry(false); }
  displayMode = DISP_NORMAL;
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
  applyDisplayMode();
}

static void closeBuddySwitcher() {
  bswOpen = false;
  applyPetGeometry(false);
  uint8_t n = buddySpeciesCount();
  if (bswIdx < n) {
    buddyMode = true;
    buddySetSpeciesIdx(bswIdx);
    speciesIdxSave(bswIdx);
  } else {
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  }
  applyTheme();
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
  applyDisplayMode();
}

static void drawBuddySwitcher() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);

  // Render the preview FIRST so the title/chrome can paint cleanly on top.
  uint8_t n = buddySpeciesCount();
  if (bswIdx < n) {
    buddyTick(P_IDLE);          // draws ASCII species idle pose into spr
  } else {
    characterDraw();
  }

  // Title bar
  spr.fillRect(0, 0, W, 22, p.body);
  spr.setTextSize(2);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(6, 4);
  spr.print("Buddies");
  spr.setTextSize(1);
  spr.setCursor(W - 16, 8);
  spr.print("X");

  // Big arrows on either side, vertically centered with the pet
  // Portrait keeps upstream's CYD positions; landscape pins the name and
  // hints to the bottom edge instead.
  const int arrowY = L.land ? bswPetYOff() + 60 : 90;
  const int nameY  = L.land ? H - 44 : 200;
  spr.setTextSize(5);
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(8, arrowY);
  spr.print("<");
  spr.setCursor(W - 36, arrowY);
  spr.print(">");

  // Name + position counter below the pet
  spr.setTextSize(2);
  spr.setTextColor(p.text, p.bg);
  spr.setTextDatum(MC_DATUM);
  const char* nm = (bswIdx < n) ? buddySpeciesName() : "GIF character";
  spr.drawString(nm, CX, nameY);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  uint8_t total = n + (gifAvailable ? 1 : 0);
  char b[20]; snprintf(b, sizeof(b), "%u / %u", bswIdx + 1, total);
  spr.drawString(b, CX, nameY + 24);

  // Hint
  if (L.land) {
    spr.drawString("tap < or >  to cycle  -  X to close", CX, H - 8);
  } else {
    spr.drawString("tap < or >  to cycle", CX, H - 32);
    spr.drawString("X to close", CX, H - 18);
  }
  spr.setTextDatum(TL_DATUM);
}

static bool buddySwitcherHandleTouch(const HalTouchEvent& evt) {
  if (evt.ex >= W - 30 && evt.ey < 22) { closeBuddySwitcher(); return true; }
  uint8_t n = buddySpeciesCount();
  uint8_t total = n + (gifAvailable ? 1 : 0);
  if (total < 1) return true;
  if (evt.ex < W / 3) {                                 // left third = prev
    bswIdx = (bswIdx + total - 1) % total;
    buddyInvalidate();
    SFX(SFX_NAV);
  } else if (evt.ex > 2 * W / 3) {                      // right third = next
    bswIdx = (bswIdx + 1) % total;
    buddyInvalidate();
    SFX(SFX_NAV);
  }
  return true;
}

// ─── Home-screen bubble shortcuts ────────────────────────────────────────
// Three tappable shortcuts in the dead band between the pet (ends ~y=164)
// and the HUD (starts at H-AREA). Visible only on DISP_NORMAL home with no
// overlays — they go away during approvals, menus, info pages, the
// passkey screen, the clock face, etc.
struct HomeBubble { int x, y, w, h; char tag; };
// Upper-LEFT column. The top-right corner is the HAL's screen-off tap
// zone (46×46), which was eating the heart bubble's taps; moving to the
// left side resolves that — the bubble layer suppresses BtnA so a tap
// inside a bubble doesn't also fire "next screen" / hold-menu.
// The pet stays centered at X=120 with content roughly x=48..192, so
// x≈4..32 is clear left of the pet.
// Landscape lays them out in a row under the pet instead, since the pet's
// pane is too narrow for a column beside it.
static const char HOME_BUBBLE_TAGS[] = {
  'P',   // Pet stats — heart
  'B',   // Buddies switcher — face
  'S',   // Settings menu — gear
  'I',   // Info screens — "i" badge
};
static HomeBubble homeBubble(int i) {
  if (L.bubbleRow) return { L.bubbleX + i * 34, L.bubbleY, 28, 22, HOME_BUBBLE_TAGS[i] };
  return { L.bubbleX, L.bubbleY + i * 26, 28, 22, HOME_BUBBLE_TAGS[i] };
}
// CLAUDE_CORAL / CLAUDE_INK are defined near the top of the file so the
// status strip and HUD activity line can reference them too.
static const int N_HOME_BUBBLES = sizeof(HOME_BUBBLE_TAGS);

// Bubble glyphs at ~12×10. Centered on (cx, cy); fg color stamped in white
// over the Claude-coral bubble background.
static void bubbleHeart(int cx, int cy, uint16_t fg) {
  spr.fillCircle(cx - 3, cy - 1, 3, fg);
  spr.fillCircle(cx + 3, cy - 1, 3, fg);
  spr.fillTriangle(cx - 6, cy, cx + 6, cy, cx, cy + 6, fg);
}
static void bubbleFace(int cx, int cy, uint16_t fg) {
  spr.drawCircle(cx, cy, 6, fg);
  spr.drawCircle(cx, cy, 5, fg);
  spr.fillCircle(cx - 2, cy - 1, 1, fg);
  spr.fillCircle(cx + 2, cy - 1, 1, fg);
  spr.drawLine(cx - 2, cy + 2, cx,     cy + 3, fg);
  spr.drawLine(cx,     cy + 3, cx + 2, cy + 2, fg);
}
static void bubbleInfo(int cx, int cy, uint16_t fg) {
  // Lowercase "i" — dot above + bar below.
  spr.fillCircle(cx, cy - 5, 1, fg);
  spr.fillRect(cx - 1, cy - 2, 3, 8, fg);
}
static void bubbleGear(int cx, int cy, uint16_t fg) {
  spr.fillCircle(cx, cy, 4, fg);
  for (int i = 0; i < 6; i++) {                       // 6 teeth
    float a = i * 3.14159f / 3.0f;
    int tx = cx + (int)(cosf(a) * 6.5f);
    int ty = cy + (int)(sinf(a) * 6.5f);
    spr.fillRect(tx - 1, ty - 1, 2, 2, fg);
  }
  spr.fillCircle(cx, cy, 1, CLAUDE_CORAL);            // inner hole punches through
}

// Small coral-bordered pill in the top-right of the pet area showing the
// pet's name. Painted AFTER buddyTick so the buddy's full-width clear
// rect doesn't wipe it. Truncates names longer than 8 chars with "..".
static void drawPetNameOverlay() {
  const char* name = petName();
  if (!name || !name[0]) return;
  char buf[10];
  int len = (int)strlen(name);
  if (len > 8) { memcpy(buf, name, 6); buf[6]='.'; buf[7]='.'; buf[8]=0; }
  else strcpy(buf, name);
  int w = (int)strlen(buf) * 6;
  int x = L.petX + L.petW - w - 8;
  int y = L.petY + (L.land ? 6 : 22);
  const Palette& p = characterPalette();
  spr.fillRoundRect(x - 4, y - 2, w + 8, 12, 4, p.bg);
  spr.drawRoundRect(x - 4, y - 2, w + 8, 12, 4, CLAUDE_CORAL);
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(1);
  spr.setCursor(x, y);
  spr.print(buf);
}

static void drawHomeBubbles() {
  for (int i = 0; i < N_HOME_BUBBLES; i++) {
    const HomeBubble b = homeBubble(i);
    spr.fillRoundRect(b.x, b.y, b.w, b.h, 6, CLAUDE_CORAL);
    int cx = b.x + b.w / 2;
    int cy = b.y + b.h / 2;
    if      (b.tag == 'P') bubbleHeart(cx, cy, CLAUDE_INK);
    else if (b.tag == 'B') bubbleFace (cx, cy, CLAUDE_INK);
    else if (b.tag == 'S') bubbleGear (cx, cy, CLAUDE_INK);
    else if (b.tag == 'I') bubbleInfo (cx, cy, CLAUDE_INK);
  }
}

// Returns true if the touch landed on a bubble and was handled.
// ─── Direct-tap overlay handler ─────────────────────────────────────────
// Hit-tests a touch release against the active menu / settings / reset
// panel: top-right X closes, each visible row fires its action directly.
// Avoids the previous BtnA-cycle / BtnB-confirm two-tap dance.
static bool handleOverlayTap(const HalTouchEvent& evt) {
  int n;
  if      (resetOpen)    n = RESET_N;
  else if (settingsOpen) n = SETTINGS_N;
  else if (menuOpen)     n = MENU_N;
  else                   return false;
  int mw = 210;
  int mh = 16 + n * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2;
  int my = (H - mh) / 2;

  // X close — generous hit box around the badge so resistive touch can
  // land cleanly on top-right.
  if (evt.ex >= mx + mw - 28 && evt.ex <= mx + mw + 4 &&
      evt.ey >= my - 2      && evt.ey <= my + 20) {
    if (resetOpen)         { resetOpen = false; }
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else if (menuOpen)     { menuOpen = false; characterInvalidate(); }
    SFX(SFX_NAV);
    return true;
  }

  // Row hit-test — each row is 14 px tall starting at my + 8.
  if (evt.ex < mx || evt.ex > mx + mw) return false;
  int rel = evt.ey - (my + 8);
  if (rel < 0) return false;
  int idx = rel / 14;
  if (idx < 0 || idx >= n) return false;
  SFX(SFX_NAV);
  if (resetOpen) {
    resetSel = idx;
    applyReset(idx);
  } else if (settingsOpen) {
    settingsSel = idx;
    applySetting(settingsIds[idx]);
  } else if (menuOpen) {
    menuSel = idx;
    menuConfirm();
  }
  return true;
}

static bool homeBubbleTapped(const HalTouchEvent& evt) {
  for (int i = 0; i < N_HOME_BUBBLES; i++) {
    const HomeBubble b = homeBubble(i);
    bool startIn = (evt.sx >= b.x && evt.sx < b.x + b.w &&
                    evt.sy >= b.y && evt.sy < b.y + b.h);
    bool endIn   = (evt.ex >= b.x && evt.ex < b.x + b.w &&
                    evt.ey >= b.y && evt.ey < b.y + b.h);
    if (!startIn || !endIn) continue;
    SFX(SFX_NAV);
    if (b.tag == 'P') {
      displayMode = DISP_PET;
      applyDisplayMode();
      characterInvalidate();
    } else if (b.tag == 'B') {
      openBuddySwitcher();
    } else if (b.tag == 'S') {
      settingsOpen = true;
      settingsSel = 0;
    } else if (b.tag == 'I') {
      displayMode = DISP_INFO;
      infoPage = 0;
      applyDisplayMode();
      characterInvalidate();
    }
    return true;
  }
  return false;
}

static void applyPetGeometry(bool switcher);

void setup() {
  Serial.begin(115200);
  M5.begin(rotationLoad());
  M5.Imu.Init();

  // Allocate the frame BEFORE the BLE stack comes up — on the no-PSRAM
  // boards NimBLE's heap use would otherwise fragment the ~76 KB blocks
  // the bands need. See canvas.h for how the frame is split.
  if (!spr.begin(&M5.Lcd)) {
    M5.Lcd.fillScreen(TFT_RED);
    M5.Lcd.setTextColor(TFT_WHITE);
    M5.Lcd.drawString("frame buffer alloc failed", 8, 8);
    Serial.println("[main] canvas allocation failed — halting");
    while (true) delay(1000);
  }
  W  = spr.width();
  H  = spr.height();
  CX = W / 2;
  computeLayout();
  M5.setGeometry(W, H, spr.scale(), spr.offX(), spr.offY());
  M5.setZoneSplit(approvalDivX());
  applyPetGeometry(false);
  Serial.printf("[main] layout %s %dx%d, pet %dx @ %d,%d %dx%d, pane x=%d w=%d\n",
                L.land ? "landscape" : "portrait", W, H, L.petScale,
                L.petX, L.petY, L.petW, L.petH, L.paneX, L.paneW);

  startBt();
  applyBrightness();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  petNameLoad();
  buddyInit();
  characterInit(nullptr);  // scan /characters/ for whatever is installed
  gifAvailable = characterLoaded();
  // species NVS: 0..N-1 = ASCII species, 0xFF = use GIF (also the default,
  // so a fresh install lands on the GIF). With no GIF installed, 0xFF falls
  // through to buddyInit()'s clamped default.
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  applyTheme();   // pick palette before the first frame is rendered
  applyDisplayMode();

  spr.render(renderBootSplash);
  delay(1800);

  Serial.printf("buddy: %s, heap %u\n", buddyMode ? "ASCII mode" : "GIF character loaded",
                (unsigned)ESP.getFreeHeap());
}

// Frame state that drawHome() needs from loop(). The UI is drawn
// immediate-mode: drawHome() repaints the whole screen every frame, and may
// run once per band on boards that render in passes (see canvas.h).
static bool g_inPrompt = false;
static bool g_clocking = false;

static void drawHome() {
  const bool inPrompt = g_inPrompt, clocking = g_clocking;
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  if (buddyMode) {
    buddyTick(activeState);
  } else if (characterLoaded()) {
    characterDraw();
  } else {
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextSize(1);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      const int x0 = L.petX + 8, y0 = L.petY;
      spr.setCursor(x0, y0 + 90);
      spr.print("installing");
      spr.setCursor(x0, y0 + 102);
      spr.printf("%luK / %luK", done/1024, total/1024);
      int barW = L.petW - 16;
      spr.drawRect(x0, y0 + 116, barW, 8, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 1) spr.fillRect(x0 + 1, y0 + 117, fill - 1, 6, p.body);
      }
    } else {
      spr.setCursor(L.petX + 8, L.petY + 100);
      spr.print("no character loaded");
    }
  }

  // Landscape: a rule between the pet pane and the content pane.
  if (L.land && !blePasskey() && !(inPrompt && isMultiChoicePrompt()))
    spr.drawFastVLine(L.petW, STATUS_H_ + 1, H - STATUS_H_ - 1, p.textDim);

  if (blePasskey()) drawPasskey();
  else if (clocking) drawClock();
  else if (displayMode == DISP_INFO) { beginPage(); drawInfo(); endPage(); }
  else if (displayMode == DISP_PET)  { beginPage(); drawPet();  endPage(); }
  else if (settings().hud) drawHUD();
  // Status strip is only meaningful on the home screen — Info / Pet have
  // their own headers and the passkey/clock screens commandeer the full
  // sprite.
  if (displayMode == DISP_NORMAL && !blePasskey() && !clocking) {
    // Multi-choice prompts own the whole screen; their renderer
    // fillSprite()s the canvas and lays out cards over it. Skip every
    // home-screen overlay so nothing paints on top of the modal.
    bool fullScreenPrompt = inPrompt && isMultiChoicePrompt();
    if (!fullScreenPrompt) {
      drawStatusStrip();
      if (!inPrompt) drawActivityLine();
      if (!inPrompt && !menuOpen && !settingsOpen && !resetOpen) {
        drawHomeBubbles();
        drawPetNameOverlay();
      }
      if (!inPrompt) {              // egg / sparkle are idle vibes only
        drawEggBubble();
        drawSparklePlay();
      }
    }
  }
  if (resetOpen) drawReset();
  else if (settingsOpen) drawSettings();
  else if (menuOpen) drawMenu();
}

#if BUDDY_ASK_CLAUDE
static void drawAskScreen() {
  if (askPickerOpen) drawAskPicker();
  else               drawAskResponse();
}
#endif

void loop() {
  M5.update();
  M5.Beep.update();
  t++;
  uint32_t now = millis();
  // Lifted from mid-loop so the gesture handler can use it: snapshot of the
  // previous frame's "was the clock face showing" state. Used to suppress
  // the bubble-tap action on the very tap that dismisses the clock — the
  // user can't see what they're tapping until the clock clears.
  static bool wasClocking = false;
  static bool wasLandscape = false;

  dataPoll(&tama);
  // Locally-clear a test-mode multi-choice prompt once its "sent..."
  // window has expired (real prompts get cleared by bridge heartbeats).
  if (testPromptClearAt && (int32_t)(millis() - testPromptClearAt) >= 0) {
    tama.promptId[0]   = 0;
    tama.promptTool[0] = 0;
    tama.promptHint[0] = 0;
    tama.promptChoiceN = 0;
    responseSent       = false;
    testPromptClearAt  = 0;
  }
  sparkTick();
  eggTick();
  sparklePlayTick();

  // Ask Claude modal owns the screen+touch when open. BLE & data parsing
  // above still run; the rest of the loop (buddy render, HUD, button
  // handlers) is suppressed until the user closes the modal.
  // Splash hold (cmd:splash from tools/capture_readme.py): the renderer
  // already painted the splash to the sprite; while we're inside the
  // hold window, just keep pushing the existing sprite contents and
  // skip everything else so the home screen doesn't paint over it.
  if (splashHoldUntilMs && millis() < splashHoldUntilMs) {
    spr.render(renderBootSplash);
    delay(20);
    return;
  }
  if (splashHoldUntilMs && millis() >= splashHoldUntilMs) {
    splashHoldUntilMs = 0;
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
  }

#if BUDDY_ASK_CLAUDE
  if (askOpen) {
    askTick();
    HalTouchEvent evt;
    if (M5.consumeTouchEvent(&evt)) askHandleTouch(evt);
    M5.suppressTouchActions();   // BtnA/B never fire while the modal is up
    // The X close button lives in the top-right corner, which is also the
    // HAL's "power" touch-zone — without draining the latch here, the tap
    // that closes the modal would also flip screenOff once the main loop
    // resumes.
    (void)M5.Axp.GetBtnPress();
    spr.render(drawAskScreen);
    delay(20);
    return;
  }
#endif
  if (bswOpen) {
    HalTouchEvent evt;
    if (M5.consumeTouchEvent(&evt)) buddySwitcherHandleTouch(evt);
    M5.suppressTouchActions();
    (void)M5.Axp.GetBtnPress();   // see askOpen comment — same X/corner conflict
    buddyAdvance();
    if (bswIdx < buddySpeciesCount()) {
      buddySetPeek(false);       // home scale
      buddySetSpeciesIdx(bswIdx);
    } else {
      characterSetState(P_IDLE);
      characterTick();
    }
    spr.render(drawBuddySwitcher);
    delay(20);
    return;
  }
  if (statsPollLevelUp()) { triggerOneShot(P_CELEBRATE, 3000); SFX(SFX_LEVEL_UP); }
  baseState = derive(tama);

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // LED: pulse on attention, otherwise off
  M5.setLed(activeState == P_ATTENTION && settings().led && (now / 400) % 2);

  // shake → dizzy + force scenario advance
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }

  // BtnA: step through fake scenarios
  // Prompt arrival: beep, reset response flag
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      SFX(SFX_APPROVE_ARRIVE);   // distinctive "ding-dong" so the device is usable across the room
      // Jump to the approval screen no matter what was open — drawApproval
      // only runs from drawHUD which only runs in DISP_NORMAL.
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;

  // Gesture layer — runs before the BtnA/B handlers so it can claim the
  // current touch release. Two gestures are recognised in DISP_NORMAL with
  // no overlays open:
  //   * Quick tap on the pet area  → trigger P_HEART (touchscreen replacement
  //     for the M5's "shake → dizzy"; here it's "pet to get a heart")
  //   * Vertical swipe in the HUD area → scroll transcript by a chunk
  //     (smoother than the single-line right-tap step)
  // When a gesture fires, M5.suppressTouchActions() wipes the underlying
  // BtnA/B wasPressed/wasReleased flags so the screen doesn't also cycle.
  // Overlay tap-handling — direct row taps + X close on menu / settings /
  // reset panels. Consumes the event first so the home-gesture block
  // below doesn't also see it.
  {
    HalTouchEvent evt;
    if ((menuOpen || settingsOpen || resetOpen) && !screenOff
        && M5.consumeTouchEvent(&evt)) {
      handleOverlayTap(evt);
      M5.suppressTouchActions();
    }
  }
  {
    HalTouchEvent evt;
    if (M5.consumeTouchEvent(&evt) &&
        !menuOpen && !settingsOpen && !resetOpen && !screenOff) {
      int dx  = evt.ex - evt.sx;
      int dy  = evt.ey - evt.sy;
      int adx = abs(dx), ady = abs(dy);
      bool inNormal = (displayMode == DISP_NORMAL);

      // Multi-choice question card-tap. Highest priority when a choice
      // prompt is active — the BtnA/BtnB approve/deny semantics don't
      // map to N options, so this hit-tests the card rectangles directly.
      if (inPrompt && isMultiChoicePrompt() && handleMultiChoiceTap(evt)) {
        M5.suppressTouchActions();
        goto gesture_done;
      }

      // Home bubble shortcuts (heart / face / gear). Check first so a tap
      // inside a bubble doesn't also fall through to pet-tap or BtnA cycle.
      // Skip when wasClocking — bubbles aren't visible on the clock face,
      // so the user couldn't have aimed; the tap is dismissing the clock.
      if (!inPrompt && inNormal && !wasClocking && homeBubbleTapped(evt)) {
        M5.suppressTouchActions();
        goto gesture_done;
      }

      // Vertical swipe — ady big, ady dominates, ends in HUD lower half
      const int SWIPE_MIN = 36;
      if (!inPrompt && inNormal && settings().hud
          && ady >= SWIPE_MIN && ady > adx * 2
          && (L.land ? evt.sx >= L.paneX : evt.sy >= H / 2)) {
        if (dy < 0) {                       // swipe up → older transcript
          uint8_t add = ady / 18;            // ~1 line per ~18 px
          if (add < 1) add = 1; if (add > 8) add = 8;
          msgScroll = (msgScroll + add > 30) ? 30 : msgScroll + add;
        } else {                            // swipe down → jump to latest
          msgScroll = 0;
        }
        M5.suppressTouchActions();
        // No beep — swipes are noiseless by design.
      }
      // Short tap on the pet — small movement, brief, upper region of NORMAL.
      else if (!inPrompt && inNormal && evt.durMs < 350
               && adx < 16 && ady < 16
               && (L.land ? (evt.sx < L.petW && evt.sy > STATUS_H_)
                          : evt.sy < L.petH)) {
        triggerOneShot(P_HEART, 1800);
        SFX(SFX_HEART);
        M5.suppressTouchActions();
      }
    }
    gesture_done:;
  }

  // Button-press wake. Track which button woke the screen so its full
  // press cycle (including long-press) is swallowed — you don't want
  // BtnA-to-wake to also cycle displayMode or open the menu.
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed()) {
    if (screenOff) {
      if (M5.BtnA.isPressed()) swallowBtnA = true;
      if (M5.BtnB.isPressed()) swallowBtnB = true;
    }
    wake();
  }

  // AXP power button (left side): short-press toggles screen off.
  // Long-press (6s) still powers off the device via AXP hardware.
  if (M5.Axp.GetBtnPress() == 0x02) {
    if (screenOff) {
      wake();
    } else {
      M5.Axp.SetLDO2(false);
      screenOff = true;
    }
  }

  if (M5.BtnA.pressedFor(600) && !btnALong && !swallowBtnA) {
    btnALong = true;
    SFX(SFX_MENU);
    if (resetOpen) { resetOpen = false; }
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) characterInvalidate();
    }
    Serial.println(menuOpen ? "menu open" : "menu close");
  }
  if (M5.BtnA.wasReleased()) {
    if (!btnALong && !swallowBtnA) {
      if (inPrompt && isMultiChoicePrompt()) {
        // Multi-choice owns the touch surface — the BtnA "approve" maps
        // to no specific choice, so just absorb the release.
      } else if (inPrompt) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        uint32_t tookS = (millis() - promptArrivedMs) / 1000;
        statsOnApproval(tookS);
        SFX(SFX_APPROVED);
        if (tookS < 5) { triggerOneShot(P_HEART, 2000); SFX(SFX_HEART); }
      } else if (resetOpen) {
        beep(1800, 30);
        resetSel = (resetSel + 1) % RESET_N;
        resetConfirmIdx = 0xFF;
      } else if (settingsOpen) {
        beep(1800, 30);
        settingsSel = (settingsSel + 1) % SETTINGS_N;
      } else if (menuOpen) {
        beep(1800, 30);
        menuSel = (menuSel + 1) % MENU_N;
      } else if (!wasClocking) {
        // Toggle between Home and Pet stats. Info now has its own bubble
        // (the "i" badge on the home column), so it stays out of the
        // left-tap rotation — Info → Home on left-tap is still allowed
        // so you can back out without using the bubble.
        beep(1800, 30);
        displayMode = (displayMode == DISP_NORMAL) ? DISP_PET : DISP_NORMAL;
        applyDisplayMode();
      }
    }
    btnALong = false;
    swallowBtnA = false;
  }

  // BtnB: pet → heart
  if (M5.BtnB.wasPressed()) {
    if (swallowBtnB) { swallowBtnB = false; }
    else if (inPrompt && isMultiChoicePrompt()) {
      // see BtnA above
    }
    else if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
      SFX(SFX_DENIED);
    } else if (resetOpen) {
      beep(2400, 30);
      applyReset(resetSel);
    } else if (settingsOpen) {
      beep(2400, 30);
      applySetting(settingsIds[settingsSel]);
    } else if (menuOpen) {
      beep(2400, 30);
      menuConfirm();
    } else if (displayMode == DISP_INFO) {
      beep(2400, 30);
      infoPage = (infoPage + 1) % INFO_PAGES;
    } else if (displayMode == DISP_PET) {
      beep(2400, 30);
      petPage = (petPage + 1) % PET_PAGES;
      applyDisplayMode();
    } else {
      beep(2400, 30);
      msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1;
    }
  }

  // blink bookkeeping

  // Charging clock: takes over the home screen when on USB power, no
  // overlays, no prompt, no live Claude data, and the RTC has been set
  // by the bridge. Pet sleeps underneath. Exit restores Y via
  // applyDisplayMode() so the next mode-switch isn't visually offset.
  clockRefreshRtc();   // 1Hz internal throttle; also caches _onUsb
  // Show the clock when nothing is happening — bridge heartbeat alone
  // doesn't count as activity (it's the only way to get the RTC synced).
  // The 30 s idle-gate (CLOCK_IDLE_MS) means any tap pushes lastInteractMs
  // forward and dismisses the clock face immediately, exposing the home
  // screen with its bubble shortcuts. The clock returns after a fresh
  // 30 s of no interaction.
  const uint32_t CLOCK_IDLE_MS = 30000;
  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid() && _onUsb
               && (millis() - lastInteractMs > CLOCK_IDLE_MS);
  // No IMU: the clock never auto-rotates (the screen rotation setting
  // covers orientation instead).
  if (!clocking) { clockOrient = 0; orientFrames = 0; paintedOrient = 0; }
  const bool landscapeClock = false;

  // wasClocking / wasLandscape are declared once at the top of loop() so
  // the gesture handler can read them earlier in the frame.
  if (clocking != wasClocking || landscapeClock != wasLandscape) {
    if (clocking && !L.land) characterSetPeek(true);
    else applyDisplayMode();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    wasClocking = clocking;
    wasLandscape = landscapeClock;
  }
  if (clocking) {
    uint8_t dow = clockDow();
    bool weekend = (dow == 0 || dow == 6);
    bool friday  = (dow == 5);

    uint8_t h = _clkTm.Hours;
    if (h >= 1 && h < 7)             activeState = P_SLEEP;
    else if (weekend)                activeState = (now/8000 % 6 == 0) ? P_HEART : P_SLEEP;
    else if (h < 9)                  activeState = (now/6000 % 4 == 0) ? P_IDLE  : P_SLEEP;
    else if (h == 12)                activeState = (now/5000 % 3 == 0) ? P_HEART : P_IDLE;
    else if (friday && h >= 15)      activeState = (now/4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
    else if (h >= 22 || h == 0)      activeState = (now/7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
    else                             activeState = (now/10000 % 5 == 0) ? P_SLEEP : P_IDLE;
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); SFX(SFX_PASSKEY); }
  lastPasskey = pk;

  // Advance the pet's animation; drawing happens in drawHome().
  if (!napping && !screenOff) {
    buddyAdvance();
    if (!buddyMode && characterLoaded()) {
      characterSetState(activeState);
      characterTick();
    }
    g_inPrompt = inPrompt;
    g_clocking = clocking;
    spr.render(drawHome);
  }

  // Face-down nap: dim immediately, pause animations, accumulate sleep time.
  // Skipped during approval — you're holding it to read, not sleeping it.
  // Exit needs sustained not-down so IMU noise at the threshold doesn't
  // bounce brightness between 8 and full every few frames.
  static int8_t faceDownFrames = 0;
  if (!inPrompt) {
    bool down = isFaceDown();
    if (down)       { if (faceDownFrames < 20) faceDownFrames++; }
    else            { if (faceDownFrames > -10) faceDownFrames--; }
  }

  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    M5.Axp.ScreenBreath(8);
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  // millis() not the cached `now`: wake() runs after `now` is captured,
  // so now - lastInteractMs underflows when a button is held → flicker.
  // No auto-off on USB power — clock face wants to stay visible while charging.
  if (!screenOff && !inPrompt && !_onUsb
      && millis() - lastInteractMs > SCREEN_OFF_MS) {
    M5.Axp.SetLDO2(false);
    screenOff = true;
  }

  delay(screenOff ? 100 : 16);
}
