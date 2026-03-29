# TACTICAL TIMER 2

A professional shooting timer built on M5Stack Core2 (ESP32), featuring a real-time Web Dashboard for remote monitoring and control via Wi-Fi.

![Platform](https://img.shields.io/badge/Platform-M5Stack%20Core2-blue)
![Framework](https://img.shields.io/badge/Framework-Arduino-teal)
![License](https://img.shields.io/badge/License-MIT-green)
![Version](https://img.shields.io/badge/Version-2.0.0-orange)

---

## Features

### Game Modes

| Mode | Description |
|------|-------------|
| **FREE** | Free shooting with optional Par Time (second beep at deadline) |
| **DRILL** | Structured drill — set shots / par time / pass % threshold |
| **DRY FIRE** | Metronome-style beat training (500–5000 ms interval) |
| **SPY** | Passive listening — records all shots without game state control |
| **RO** | Range Officer mode — measures draw time from command to first shot |
| **HISTORY** | Session log on SD card, with paged list and detail view |
| **SETTINGS** | Adjust preset / detection source / mic threshold / random delay |

### Web Dashboard
- Real-time shot display via SSE (Server-Sent Events) — no page refresh needed
- Remote START / STOP / RESTART control
- Par Time, Random Delay, Drill parameters, Dry Fire BPM — all adjustable from browser
- **Full bidirectional sync** — changes on Core2 reflect on Web, and vice versa
- History browser with split chart visualization and session delete
- 4 color themes: Dark / Navy / Light / Amber
- Responsive layout — phone portrait / landscape / tablet all supported

### Wi-Fi: Dual-Mode (STA / AP Auto-Failover)

TT2 automatically selects the best Wi-Fi mode at boot:

| Scenario | Mode | How to connect |
|----------|------|----------------|
| Home / Range with Router | **STA** (Station) | Connect phone to same Wi-Fi → open `http://<IP shown on screen>` |
| Outdoor / No Router | **AP** (Access Point) | Connect phone to `TacticalTimer2` hotspot → open `http://192.168.4.1` |

**How it works:**
1. At boot, TT2 attempts to connect to the configured Wi-Fi router (15-second timeout)
2. **Success → STA mode**: IP address shown in **cyan** on the bottom bar
3. **Failure → AP mode**: TT2 becomes its own hotspot. Bottom bar shows **`[AP] 192.168.4.1`** in **orange**

> AP hotspot credentials: SSID `TacticalTimer2` / Password `tt2admin`
> These can be changed in `config.h` → `NetCfg::AP_SSID` / `NetCfg::AP_PASSWORD`

---

## Hardware

| Component | Spec |
|-----------|------|
| Board | M5Stack Core2 (ESP32-D0WDQ6-V3, 240 MHz dual-core) |
| Microphone | SPM1423 PDM (built-in) |
| RTC | BM8563 (built-in, NTP sync on boot) |
| Display | ILI9342C 320×240 touch LCD |
| Speaker | Internal (I2S, shared with mic via time-division multiplexing) |
| Storage | microSD card (for session history JSON files) |

---

## Requirements

### Arduino IDE
- Arduino IDE 1.8.x
- ESP32 Arduino Core **2.x** (IDF 4.4.x) — tested on 2.0.17

### Libraries
- M5Stack Core2
- ESP8266Audio
- ESPAsyncWebServer + AsyncTCP
- ArduinoJson
- esp_now (built-in with ESP32 Arduino Core)

---

## Getting Started

### 1. Clone this repository

```bash
git clone https://github.com/YOUR_USERNAME/TacticalTimer2.git
```

### 2. Create credentials file

Copy the template and fill in your Wi-Fi credentials:

```bash
cp credentials.h.example credentials.h
```

Edit `credentials.h`:

```cpp
#pragma once
#define WIFI_SSID     "your_wifi_ssid"
#define WIFI_PASSWORD "your_wifi_password"
```

> ⚠️ `credentials.h` is listed in `.gitignore` and will **never** be committed.
>
> If Wi-Fi connection fails at boot, TT2 **automatically starts AP hotspot mode** — no router needed for outdoor use.

### 3. Prepare SD card

Create a `mp3/` folder on the SD card root and add your own audio files.

> ⚠️ **MP3 files are NOT included in this repository** to avoid copyright issues.
> You need to provide your own audio files and place them with the exact filenames below.

```
SD card root/
└── mp3/
    ├── 7001.mp3    ← "Are you ready?" voice prompt  (~2s)
    ├── 7002.mp3    ← Start beep (single tone)        (~0.3s)
    ├── 7003.mp3    ← End / show clear tone            (~0.5s)
    └── tick.mp3    ← Dry Fire beat tick               (~0.1s)
```

**Free audio sources (royalty-free / CC0):**
- [Freesound.org](https://freesound.org) — search "beep", "ready", "gunshot"
- [Pixabay Sound Effects](https://pixabay.com/sound-effects/) — no attribution required
- Record your own voice prompt with your phone

**Audio tips:**
- All files must be **MP3 format**, mono or stereo, any sample rate
- Keep file size small (< 500 KB each) for fast SD card loading
- Adjust volume balance in `config.h` → `HW::OUTPUT_GAIN` (default `0.4f`, range `0.0–4.0`)

### 4. Open in Arduino IDE

- Open `TacticalTimer2/TacticalTimer2.ino`
- Select board: **M5Stack-Core2**
- Upload

### 5. Access the Web Dashboard

Check the bottom bar of the Core2 screen:

| Display | Mode | Browser URL |
|---------|------|-------------|
| `192.168.x.x` (cyan) | STA — connected to Router | `http://192.168.x.x` |
| `[AP] 192.168.4.1` (orange) | AP — Core2 is the hotspot | Connect to `TacticalTimer2` Wi-Fi, then `http://192.168.4.1` |

---

## Architecture

```
TacticalTimer2/
├── config.h            # Global constants, enums, structs, NVS keys
├── ipc.h / ipc.cpp     # RTOS IPC (Queue / Mutex / EventGroup)
├── safety.h / .cpp     # Watchdog, panic handler, memory diagnostics
│
├── hal_mic.h / .cpp    # SPM1423 PDM microphone HAL
├── hal_audio.h / .cpp  # I2S audio HAL (ESP8266Audio, time-division mux with mic)
├── hal_rtc.h / .cpp    # BM8563 RTC + NTP sync
├── hal_storage.h / .cpp# SD card sessions (JSON) + NVS settings / presets
│
├── timer_core.h / .cpp # Game state machine & timing engine
├── mic_engine.h / .cpp # Gunshot detection RTOS Task
├── preset_mgr.h / .cpp # 5 firearm presets (NVS-backed)
├── session_mgr.h / .cpp# Session save / load
├── network_mgr.h / .cpp# WiFi STA/AP auto-failover + ESP-NOW + NetworkTask
│
├── game_fsm.h / .cpp   # Main FSM — touch/button dispatch, mode switching
├── mode_free.h / .cpp  # FREE mode
├── mode_drill.h / .cpp # DRILL mode
├── mode_dryfire.h/.cpp # DRY FIRE mode
├── mode_spy.h / .cpp   # SPY mode
├── mode_ro.h / .cpp    # RO mode
├── mode_history.h/.cpp # HISTORY mode
├── mode_settings.h/.cpp# SETTINGS mode
│
├── ui_screen.h / .cpp  # All LCD drawing primitives
├── web_server.h / .cpp # AsyncWebServer + SSE dashboard (HTML embedded)
│
├── TacticalTimer2.ino  # Entry point — setup() / loop() / RTOS tasks
├── credentials.h       # Wi-Fi credentials (NOT committed — see .gitignore)
└── credentials.h.example # Credentials template
```

### RTOS Task Layout

| Task | Core | Priority | Responsibility |
|------|------|----------|----------------|
| `loopTask` | 1 | 2 | UI / GameFSM (Arduino `loop()`) |
| `AudioTask` | 0 | 5 | MP3 decode + I2S output |
| `MicTask` | 0 | 4 | PDM sampling + gunshot detection |
| `NetTask` | 0 | 3 | SSE push to Web Dashboard |

### I2S Time-Division Multiplexing

SPM1423 mic and speaker share **I2S_NUM_0**. A FreeRTOS EventGroup handshake coordinates access:

```
Play request:
  AudioTask → requestMicPause() → sets MIC_PAUSE_BIT, waits MIC_SUSPENDED_BIT (200ms)
  MicTask   → deinit I2S → confirmMicSuspended() → waits for bit clear

Playback done:
  AudioTask → releaseMicPause() → MicTask reinitializes I2S, resumes sampling
```

---

## Default Preset Thresholds

| Preset | RMS Threshold | Notes |
|--------|--------------|-------|
| Air Pistol | 1700 | Quiet, indoor |
| Air Rifle | 1500 | Quieter than pistol |
| Pistol | 3000 | Standard centerfire |
| Rifle | 2500 | Louder muzzle blast |
| Custom | 2000 | User-adjustable |

Threshold range: **500–8000** (adjustable in SETTINGS or Web Dashboard)

---

## Web Dashboard SSE Events

| Event | Payload | Triggered by |
|-------|---------|-------------|
| `hit` | `{hitIdx, stationId, elapsed, split}` | Each shot detected |
| `gs` | GameState integer | State change |
| `newgame` | `"1"` | New session start |
| `best` | `{t, a}` | New personal best |
| `mode` | AppMode integer | Mode switch |
| `settings` | `{preset, thresh, src}` | Settings saved |
| `partime` | `{parMs, rand}` | Par time changed (Core2 or Web) |
| `drybeat` | beat ms | Dry Fire interval changed |
| `drill` | `{shots, parMs, passPct}` | Drill definition changed |
| `drillresult` | `{score, passed, ...}` | Drill session ended |

---

## Key Configuration (`config.h`)

```cpp
// Wi-Fi AP hotspot (auto-enabled when STA fails)
NetCfg::AP_SSID     = "TacticalTimer2"   // Hotspot name
NetCfg::AP_PASSWORD = "tt2admin"          // Min 8 characters
NetCfg::AP_IP       = "192.168.4.1"

// Audio output
HW::OUTPUT_GAIN     = 0.4f    // Range 0.0–4.0; increase if volume too low

// Mic detection
MicCfg::THRESH_DEF  = 2000    // Default RMS threshold
MicCfg::THRESH_MIN  = 500
MicCfg::THRESH_MAX  = 8000
MicCfg::COOLDOWN_MS = 300     // Post-shot lockout (ms)

// Timing
Timing::GOING_GUARD_MS  = 600    // Post-beep false-trigger lockout
Timing::PAR_TIME_MAX_MS = 30000  // Max par time (30 s)
Timing::DRY_BEAT_MIN_MS = 500
Timing::DRY_BEAT_MAX_MS = 5000
```

---

## License

MIT License — see [LICENSE](LICENSE) for details.

## Credits

Developed by Louis Chou.
