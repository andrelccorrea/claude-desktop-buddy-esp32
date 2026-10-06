# claude-desktop-buddy-esp32

The [Claude desktop Hardware Buddy](https://github.com/anthropics/claude-desktop-buddy)
firmware for **ESP32 touch displays from 2.4" to 7"** — the cheap Sunton
"CYD" family, Elecrow CrowPanels and Waveshare's ESP32-P4. It pairs with the
**Hardware Buddy** window in Claude desktop over Bluetooth LE, shows what
your sessions are doing, lets you approve or deny tool requests with a tap,
and keeps a small pet that reacts to your work.

Forked from [jdperich/claude-desktop-buddy-cyd](https://github.com/jdperich/claude-desktop-buddy-cyd)
(the 2.8" CYD port of Anthropic's M5StickC original), with the display layer
rebuilt so one codebase lays itself out natively on each panel.

**Install from the browser:** https://alinke.github.io/claude-desktop-buddy-esp32/

<table>
  <tr>
    <td align="center" width="62%">
      <img src="docs/esp32-5in-home.png" width="420"><br>
      <sub><b>5" 800x480</b> — landscape layout: pet pane, live transcript</sub>
    </td>
    <td align="center" width="38%">
      <img src="docs/esp32-35in-home.png" width="170"><br>
      <sub><b>3.5" 320x480</b> — portrait layout, 3x pet</sub>
    </td>
  </tr>
</table>

> **Community fork. Not affiliated with, endorsed by, or sponsored by
> Anthropic.** "Claude" and the Anthropic asterisk mark are trademarks
> of Anthropic, PBC, used here nominatively to identify the service
> this device integrates with. Firmware is MIT-licensed (see
> [`LICENSE`](LICENSE)); the brand identity is not.

See [`REFERENCE.md`](REFERENCE.md) for the wire protocol.

---

## Supported boards

| Board | Env | Chip | Panel | Touch | Status |
| --- | --- | --- | --- | --- | --- |
| 2.8" ESP32-2432S028R ("CYD") | `cyd` | ESP32 | 240x320 | resistive | builds |
| 2.8" ESP32-2432S028R, TPM408-2.8 panel | `cyd-tpm408` | ESP32 | 240x320 | resistive | **tested** |
| 2.4" ESP32-2432S024C | `sunton-2432s024c` | ESP32 | 240x320 | capacitive | builds |
| 3.5" ESP32-3248S035R | `sunton-3248s035r` | ESP32 | 320x480 | resistive | **tested** |
| 3.5" ESP32-3248S035C | `sunton-3248s035c` | ESP32 | 320x480 | capacitive | builds |
| 3.5" Elecrow CrowPanel Advance | `elecrow-advance-3-5` | ESP32-S3 | 480x320 | capacitive | builds |
| 4.3" ESP32-4827S043C | `sunton-4827s043c` | ESP32-S3 | 480x272 RGB | capacitive | builds |
| 4.3" Elecrow CrowPanel Advance | `elecrow-advance-4-3` | ESP32-S3 | 800x480 RGB | capacitive | builds |
| 5" ESP32-8048S050C | `sunton-8048s050c` | ESP32-S3 | 800x480 RGB | capacitive | runs (BLE pairing unconfirmed) |
| 5" Elecrow CrowPanel (red) | `elecrow-5-0` | ESP32-S3 | 800x480 RGB | capacitive | builds |
| 5" Elecrow CrowPanel Advance v1.2/1.3 | `elecrow-advance-5-0` | ESP32-S3 | 800x480 RGB | capacitive | builds |
| 5" Elecrow CrowPanel Advance v1.1 | `elecrow-advance-5-0-v1_1` | ESP32-S3 | 800x480 RGB | capacitive | builds |
| 7" ESP32-8048S070 | `sunton-8048s070` | ESP32-S3 | 800x480 RGB | capacitive | builds |
| 7" Elecrow CrowPanel 7.0 | `elecrow-7-0` | ESP32-S3 | 800x480 RGB | capacitive | builds |
| 4.3" Waveshare ESP32-P4 | `waveshare-p4-4-3` | ESP32-P4 + C6 | 480x800 DSI | capacitive | runs (BLE via the C6 unconfirmed) |
| 2.1" Elecrow CrowPanel round | `elecrow-round-2-1` | ESP32-S3 | 480x480 RGB | capacitive | builds, no round layout yet |

*Tested* = run on real hardware, paired with Claude desktop; *runs* = boots
and renders on real hardware; *builds* = compiles from the same code but
hasn't been run on that board yet. Reports welcome.

The panel configurations come from the Pixelcade Sidekick firmware, where
each was brought up on real hardware.

## How it adapts to each panel

- **Logical pixels.** The UI draws on a logical canvas scaled by an integer
  factor from the panel's short side (240-320 px -> 1x, 480 -> 2x), so an
  800x480 panel is a 400x240 logical landscape. Text, boxes and strokes
  scale together and render at the panel's full resolution
  ([`src/canvas.h`](src/canvas.h)).
- **Layouts.** Portrait keeps upstream's arrangement (pet on top, transcript
  below) and gives taller screens a 3x pet and more transcript rows.
  Landscape puts the pet in a left pane and the transcript, approval card,
  clock and Info/Pet pages in the right pane.
- **Rotation is a setting** (Settings -> rotation). The board restarts in the
  new orientation and lays itself out again; touch calibration carries over.
- **Memory.** Boards without PSRAM draw the frame in horizontal passes
  through one ~76 KB buffer and send only the 16-row strips that changed;
  boards with PSRAM keep a full 16-bit frame. RGB-parallel panels swap
  frames at VSYNC (no tearing).
- **Ask Claude** (WiFi + your own API key, typed on the touch keyboard) is
  built only for the 4.3"-and-larger boards.

## Install

**Web flasher** (Chrome or Edge on a desktop):
https://alinke.github.io/claude-desktop-buddy-esp32/ — pick your board,
click Install. Installing erases the board.

**Command line:** each board's merged image (bootloader + partitions + app)
is attached to the [releases](https://github.com/alinke/claude-desktop-buddy-esp32/releases)
and flashes at offset 0:

```bash
esptool.py --chip esp32   --port <PORT> write_flash 0x0 buddy-<env>.bin   # classic ESP32
esptool.py --chip esp32s3 --port <PORT> write_flash 0x0 buddy-<env>.bin   # ESP32-S3
```

Resistive-touch boards run a 4-corner touch calibration on first boot.

## Build from source

Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/)
(Python 3.10+), then from the repo root:

```bash
pio run -e sunton-3248s035r -t upload       # build + flash one board
pio device monitor -e sunton-3248s035r      # serial console, 115200 baud
python scripts/build_site.py                # every board + the web flasher, into site/
```

The env names are in the table above. The first build downloads the
pioarduino platform (Arduino-ESP32 3.3), which takes a while. RGB-panel envs
patch LovyanGFX's RGB bus driver before compiling
(`scripts/apply_lovyangfx_rgb_patch.py`).

Pushes to `main` build every board and publish the web flasher through
GitHub Actions (`.github/workflows/flasher.yml`); `v*` tags also attach the
images to a release.

**ESP32-P4 boards** bring BLE up on the onboard ESP32-C6 through
ESP-Hosted, so the C6 needs ESP-Hosted firmware with Bluetooth (2.x). Their
native USB-C port can't reset into the bootloader: flash through the board's
UART (CH343) USB-C port, or, once this firmware is on it, over USB with
`serial_ota_upload.py` (`FWB1` protocol, `src/serial_ota.h`).

## Pairing

1. In Claude for Windows/macOS: **Help → Troubleshooting → Enable
   Developer Mode**
2. **Developer → Open Hardware Buddy…**
3. Click **Connect**, pick `Claude-XXXX` from the list (XXXX = last
   two bytes of the device's MAC — also on Info → Bluetooth)
4. The link is **unencrypted** (as in the CYD fork): the protocol
   explicitly supports unencrypted devices, and the desktop reports
   `sec: false` in the status panel.

**USB:** the firmware also reads the same newline-delimited JSON on its USB
serial port and sends its replies there too, so anything that speaks the
protocol over serial (`tools/devcmd.py`, a test harness, a host bridge) can
drive it without Bluetooth. The status-strip sparkle turns green while the
data is coming over USB. The Claude desktop app itself only talks BLE.

---

## What it looks like

Screenshots in this section are from the 2.8" CYD (240x320 portrait); the
same screens lay out in the right pane on landscape boards:

<p align="center"><img src="docs/esp32-35in-landscape-approval.png" width="360"></p>

### Home & pet

The home screen runs the show: an always-on status strip at the top
(sparkle ✻ + `run N wait N` + 10-minute token sparkline + total
tokens today), four shortcut bubbles down the left rail, the
buddy/pet centred, a coral pill in the top-right with the pet's name,
a coral activity line showing what Claude is currently doing, and a
scrolling transcript HUD at the bottom.

<table>
  <tr>
    <td align="center" width="33%">
      <img src="docs/home.png" width="220"><br>
      <sub><b>home</b><br>live status + transcript</sub>
    </td>
    <td align="center" width="33%">
      <img src="docs/pet_stats.png" width="220"><br>
      <sub><b>pet stats</b><br>mood / fed / energy / lifetime tokens</sub>
    </td>
    <td align="center" width="33%">
      <img src="docs/buddies.png" width="220"><br>
      <sub><b>buddies</b><br>cycle through 18 ASCII species</sub>
    </td>
  </tr>
</table>

### Menus

Every overlay uses direct-tap rows with an X close badge in the
corner — no "tap left to move the cursor, tap right to change the
value" dance from the upstream stick UI. Each row is a button.

<table>
  <tr>
    <td align="center" width="33%">
      <img src="docs/menu.png" width="220"><br>
      <sub><b>main menu</b><br>ask · buddies · settings · power · help · about · demo</sub>
    </td>
    <td align="center" width="33%">
      <img src="docs/settings.png" width="220"><br>
      <sub><b>settings</b><br>brightness, sound, theme, wifi, api key, calibrate…</sub>
    </td>
    <td align="center" width="33%">
      <img src="docs/reset.png" width="220"><br>
      <sub><b>reset</b><br>delete character · factory reset</sub>
    </td>
  </tr>
</table>

### Prompts from the desktop bridge

When the desktop wants approval to run a tool, the device pops a
modal with the tool name, the action being requested, and a giant
**approve / deny** split-button. Multi-choice prompts get a
card-stack layout instead of yes/no — forward-compatible with a
future `prompt.choices[]` field in the wire protocol.

The screenshots use a deliberately silly demo payload — the real
prompts read like actual shell commands and tool calls.

<table>
  <tr>
    <td align="center" width="50%">
      <img src="docs/approval.png" width="220"><br>
      <sub><b>approval</b><br>tool icon + hint + green/coral buttons</sub>
    </td>
    <td align="center" width="50%">
      <img src="docs/multichoice.png" width="220"><br>
      <sub><b>multichoice</b><br>tap a card to answer</sub>
    </td>
  </tr>
</table>

### Ask Claude (WiFi, no desktop)

When the desktop isn't around but WiFi + an Anthropic API key are
configured (entered via the on-device touch keyboard), the device
can hit `api.anthropic.com/v1/messages` directly with one of four
preset prompts and stream the reply into the transcript over SSE.

<table>
  <tr>
    <td align="center" width="50%">
      <img src="docs/ask_picker.png" width="220"><br>
      <sub><b>ask claude</b><br>four preset prompts, streamed reply</sub>
    </td>
    <td align="center" width="50%">
      <em>(stream lands directly in the home transcript HUD)</em>
    </td>
  </tr>
</table>

### Info pages

The Info section is a paginated read-only status book — tap the **i**
bubble on home to enter, tap right to advance, X to exit. Page 5
(sessions) and page 6 (device) update live from heartbeat events.

<table>
  <tr>
    <td align="center" width="25%">
      <img src="docs/info_about.png" width="170"><br>
      <sub><b>1/8 about</b><br>what the device does</sub>
    </td>
    <td align="center" width="25%">
      <img src="docs/info_controls.png" width="170"><br>
      <sub><b>2/8 controls</b><br>full touch reference</sub>
    </td>
    <td align="center" width="25%">
      <img src="docs/info_claude.png" width="170"><br>
      <sub><b>3/8 claude</b><br>session + BLE link state</sub>
    </td>
    <td align="center" width="25%">
      <img src="docs/info_response.png" width="170"><br>
      <sub><b>4/8 response</b><br>last assistant turn, in full</sub>
    </td>
  </tr>
  <tr>
    <td align="center" width="25%">
      <img src="docs/info_sessions.png" width="170"><br>
      <sub><b>5/8 sessions</b><br>visual grid of running/waiting/idle</sub>
    </td>
    <td align="center" width="25%">
      <img src="docs/info_device.png" width="170"><br>
      <sub><b>6/8 device</b><br>battery, heap, uptime, brightness</sub>
    </td>
    <td align="center" width="25%">
      <img src="docs/info_bluetooth.png" width="170"><br>
      <sub><b>7/8 bluetooth</b><br>link state + MAC + last-msg age</sub>
    </td>
    <td align="center" width="25%">
      <img src="docs/info_credits.png" width="170"><br>
      <sub><b>8/8 credits</b><br>upstream + fork + hardware</sub>
    </td>
  </tr>
</table>

---

## Features beyond upstream

- **Full 240×320 UI** — every hardcoded coord was reworked from the
  original 135×240 M5StickC layout
- **Touch-only controls** mapped to tap zones (left = A, right = B,
  hold-left = menu, top-right corner = power), plus on-screen bubble
  shortcuts down the left rail for **Pet stats / Buddies / Settings /
  Info**
- **Persistent status strip** at the top with run/wait counters, a
  10-minute token-activity sparkline, and today's total tokens
- **Live activity line** above the HUD showing the bridge's current
  `msg` field in Claude coral (`(called Bash)`, `generating reply`,
  etc.)
- **Built-in themes** — Claude Light, Claude Dark, Terminal — cycle
  from **Settings → theme**
- **Event-specific beep patterns** (approval ping, denial buzz,
  done-chord, etc.) routed through the LEDC tone driver
- **"Last response" Info page** showing the most recent assistant
  turn in full, captured from per-turn `text` events
- **Sessions Info page** with a visual grid breakdown of
  running/waiting/idle sessions
- **Tool icons** on the approval prompt — distinct glyphs for Bash,
  Read, Write, Edit, WebFetch, WebSearch, etc.
- **Multi-choice question UI** — card-stack modal that renders
  whenever the bridge sends `prompt.choices[]`; a test trigger in
  **Settings → test choice** exercises it today
- **Easter-egg idle animations** — speech bubbles, weekday/hour-gated
  jokes, and a "pet plays with the Claude logo" coral-sparkle particle
- **Touch keyboard** for entering WiFi credentials and an Anthropic
  API key — masked password input, shift / symbol modes, X to cancel
- **Standalone Ask Claude** — when paired, the bridge talks to
  Claude on the desktop; when not, the device can call the public
  API directly and stream the reply
- **Custom partition table** — 2.25 MB factory app + 1.66 MB
  LittleFS for GIF character packs, auto-formatted on first boot
- **NVS-backed everything** — touch calibration, theme, owner, pet
  name, species, WiFi creds, API key

---

## Touch controls

Resistive panels are calibrated on first boot (LovyanGFX's 4-corner
calibration, stored in NVS under `tcal`, valid for every rotation; redo it
from Settings → calibrate). Capacitive panels need none. Then:

| Action | Where |
| --- | --- |
| Approve / next screen | Tap left side (landscape: the pet pane or the left of the approval card) |
| Deny / page through info | Tap right side |
| Open menu | Hold left side ~0.6 s |
| Floating hearts | Tap the pet |
| Scroll transcript | Swipe up/down in the HUD |
| Screen off / wake | Tap top-right corner / tap anywhere |
| Pet stats | Heart bubble (left column; a row under the pet in landscape) |
| Switch buddy species | Face bubble |
| Open settings | Gear bubble |
| Open info pages | "i" bubble |

---

## Project layout

```
src/
  main.cpp           — loop, state machine, UI screens, portrait/landscape layout
  canvas.{h,cpp}     — logical-pixel drawing surface: scale, passes, dirty strips
  board.{h,cpp}      — per-board identity, pins and bring-up (expanders, backlight chips)
  buddy.{cpp,h}      — ASCII species dispatch + render helpers
  buddies/           — one file per species, seven anim functions each
  character.{cpp,h}  — GIF decode + render
  ble_bridge.cpp     — Nordic UART service over NimBLE-Arduino
  ble_bridge_hosted.cpp — same, for ESP32-P4 (core BLE library over ESP-Hosted)
  serial_ota.h       — firmware update over USB serial (native-USB boards)
  version.h          — firmware version
  data.h             — wire protocol parser + tooling dispatch
  xfer.h             — folder-push receiver
  stats.h            — NVS-backed stats, settings, owner, species
  hal_m5.{h,cpp}     — M5 API shim over LovyanGFX: touch zones, backlight, sound, RTC
  touch_keyboard.cpp — on-device QWERTY keyboard widget
  ask_claude.{h,cpp} — standalone Anthropic API client over WiFi
  wifi_creds.h       — NVS storage for SSID, password, API key
board_configs/       — LovyanGFX panel/touch config per board
boards/              — PlatformIO board definitions
scripts/
  build_site.py      — build every board + assemble the web flasher (site/)
  apply_lovyangfx_rgb_patch.py — RGB panel bounce-buffer + VSYNC double buffer
web/                 — web flasher page + boards.json (board list)
tools/
  snap.py            — pull a pixel-perfect PNG screenshot over USB (8/16 bpp)
  devcmd.py          — send one JSON line over USB and print the reply
  sim.py             — inject synthetic taps/swipes over USB
  capture_readme.py  — one-shot README screenshot orchestrator
PORT.md              — the CYD port's notes (what changed vs the M5StickC)
partitions.csv       — 4 MB layout (2.25 MB app + 1.66 MB LittleFS)
characters/          — example GIF character pack (bufo)
docs/                — README screenshots (auto-generated)
```

---

## Acknowledgments

- **[anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)** —
  the original M5StickC Plus reference firmware by Felix Rieseberg.
  The wire protocol, the buddy concept, and the ASCII species
  rendering all come straight from upstream.
- **[jdperich/claude-desktop-buddy-cyd](https://github.com/jdperich/claude-desktop-buddy-cyd)** —
  the CYD port this fork builds on: the M5 HAL shim, touch UI, themes,
  multi-choice prompts and Ask Claude.
- **[LovyanGFX](https://github.com/lovyan03/LovyanGFX)** drives every
  panel; the board configs come from the Pixelcade Sidekick firmware.
- **[vthinkxie/claude-desktop-buddy-esp32](https://github.com/vthinkxie/claude-desktop-buddy-esp32)** —
  a separate ESP32-S3 AMOLED fork that's a useful comparison point
  for board-HAL structure and a software-RTC pattern.
- The **bufo GIF assets** in `characters/bufo/` come from the
  community bufo emoji set ([bufo.zone](https://bufo.zone)) and
  remain the property of their original creators; not covered by
  the MIT license. See `characters/bufo/README.md`.

---

## License

MIT — see [`LICENSE`](LICENSE).

```
Copyright 2026 Anthropic, PBC.       (original upstream)
Copyright 2026 J. Perich.            (CYD port additions)
```

The Claude name and any visual references to Anthropic's brand
identity are not licensed under MIT and remain the property of
Anthropic, PBC.
