# 📟 Pocket-AI Terminal by fajer abednabie btw. 

A pocket-sized AI chat terminal running on an **ESP8266 D1 Mini Lite** with a
0.91″ SSD1306 OLED (128×32) and a 4-button input system.

---

## Hardware Bill of Materials

| Component | Part |
|---|---|
| Microcontroller | Wemos D1 Mini Lite (ESP8266EX, 1 MB flash) |
| Display | 0.91″ SSD1306 OLED 128×32 (I²C) |
| Buttons × 4 | 6×6 mm tactile momentary switches |
| Resistors × 3 | 10 kΩ (pull-ups for D5/D6/D7 — or use internal) |
| Resistor × 1 | 10 kΩ (external pull-up for D0 / GPIO16) |
| Power | 3.7 V LiPo + TP4056 module **or** USB |
| Wire / PCB | Perfboard recommended |

---

## Wiring Diagram

```
                      D1 Mini Lite
                    ┌─────────────────┐
                    │            RST ←│── Bridge to D0 (deep-sleep wake)
          3V3 ──────│3V3         GND  │── GND
    OLED VCC        │                 │
          SDA ──────│D2          D5   │──[ BTN_PREV  ]── GND
          SCL ──────│D1          D6   │──[ BTN_NEXT  ]── GND
                    │            D7   │──[ BTN_SELECT]── GND
    D0 wake ────────│D0 (GPIO16) D0   │── (same pin — bridge to RST)
                    └─────────────────┘

OLED (I²C):
  VCC → 3V3
  GND → GND
  SDA → D2
  SCL → D1

Buttons D5/D6/D7:  one leg → pin, other leg → GND (INPUT_PULLUP used)
Button  D0:        one leg → D0, other leg → GND, 10kΩ to 3V3 (external pull-up)

⚠️  CRITICAL: solder a wire between RST and D0 (GPIO16).
    Without this bridge, deep sleep will not wake up.
```

---

## Software Setup

### 1 — Install PlatformIO

```bash
pip install platformio
# or use the VS Code PlatformIO extension
```

### 2 — Clone / copy this project

```
pocket_ai/
├── platformio.ini
└── src/
    └── main.cpp
```

### 3 — Flash

```bash
cd pocket_ai
pio run -t upload        # compiles + uploads
pio device monitor       # open serial monitor at 115200
```

---

## First Boot — WiFi Configuration

On first boot (or after a **Reset WiFi** from the Settings menu):

1. The device enters **AP mode** and broadcasts **`PocketAI-Setup`** (password: `12345678`).
2. Connect your phone or laptop to that network.
3. Open a browser → **`http://192.168.4.1`**
4. Fill in your WiFi SSID, password, and your **Groq or OpenAI API key**.
5. Tap **Save & Connect** — the device reboots and connects automatically.

> **Groq** offers a generous **free tier** and ultra-fast inference.
> Get a key at https://console.groq.com/keys
> Key format: `gsk_...`

---

## Button Reference

| Button | Short Press | Long Press (≥ 800 ms) |
|---|---|---|
| **D5 PREV** | Carousel ← / Menu up | — |
| **D6 NEXT** | Carousel → / Menu down | — |
| **D7 SELECT** | Confirm character / Enter menu | — |
| **D0 FUNC** | **Backspace** | **Send message** (triggers confirm screen) |

### Keyboard — The Linear Carousel

```
 ┌────────────────────────────────────────────────────────────┐
 │  My typed text here|                              [abc]    │
 │ ─────────────────────────────────────────────────────────  │
 │       c   d  ┌───┐  f   g   h                             │
 │              │ e │                                         │
 │              └───┘                                         │
 └────────────────────────────────────────────────────────────┘
      PREV ◄         ► NEXT          SELECT = type it
```

- Scroll with **PREV/NEXT**.  The **centre character** is highlighted and enlarged.
- Press **SELECT** to type the selected character.
- Reach the **`[S]`** (shift) entry at the end of the strip → SELECT cycles
  through `abc → ABC → 123 → abc`.
- After a ~1.4 s pause, **predictive bubbles** appear.
  Use PREV/NEXT to cycle predictions; SELECT accepts.

---

## Menus

```
Main Menu
├── New Chat      → opens keyboard; starts fresh conversation
├── Settings
│   ├── API Key (WebUI)   → launches AP portal to change key
│   ├── Temperature       → cycles 0.3 / 0.7 / 1.0
│   ├── Dark Mode         → reduces OLED brightness
│   ├── Reset WiFi        → clears saved credentials
│   └── < Back
└── System Info   → free RAM, RSSI, IP address
```

---

## Switching API Provider

To use **OpenAI** instead of Groq, edit three lines near the top of `main.cpp`:

```cpp
#define API_HOST   "api.openai.com"
#define API_PATH   "/v1/chat/completions"
#define API_MODEL  "gpt-4o-mini"        // or gpt-3.5-turbo
```

Any OpenAI-compatible endpoint (Anthropic, Together, local Ollama with
`ngrok`) works the same way.

---

## Memory Map & RAM Budget

| Resource | Usage |
|---|---|
| U8g2 full frame buffer | 512 B |
| Input buffer | 221 B |
| Response buffer | 701 B |
| JSON doc (build) | 768 B (stack, freed after call) |
| JSON doc (parse) | 900 B (stack, freed after call) |
| History ring (3 × 2) | ~570 B |
| WiFi / SSL overhead | ~20 KB (heap) |
| **Free heap at idle** | **≈ 24–28 KB** |

`StaticJsonDocument` keeps JSON off the heap entirely.
`client.setInsecure()` avoids the ~10 KB CA-chain cost.

---

## Deep Sleep

- The device sleeps after **2 minutes** of inactivity.
- Wake: press any button that pulls **D0/GPIO16 LOW** — this pulses RST.
- The RST → D0 solder bridge is **mandatory** for this to work.

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| `[ERR] Cannot reach API server` | Check WiFi; verify API key is set |
| OLED blank after flash | Check SDA/SCL wiring (D2/D1) |
| Device stuck in AP mode | Erase flash: `pio run -t erase`, then reflash |
| Crash / WDT reset during API call | Free heap < 15 KB; shorten history or reduce `MAX_RESP` |
| Won't wake from sleep | Verify RST ↔ D0 bridge is soldered |
| JSON parse errors | API returned chunked encoding; increase `rawResp` buffer |

---

## License

MIT — do whatever you like. A star or photo of your build is always welcome! ⭐
