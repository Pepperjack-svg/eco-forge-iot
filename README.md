# SoundAmp IoT

**Ambient Sound Amplification & Basic ANC for Wired Earphones** — powered by ESP32 + BLE.

Brings TWS-style features (like Samsung Buds / AirPods transparency mode) to any standard 3.5mm wired earphone. Controlled via a Vercel-hosted React dashboard over Web Bluetooth — no WiFi AP needed.

---

## Hardware Required

| # | Component | You Have It? |
|---|-----------|:---:|
| 1 | ESP32 DevKit | ✅ |
| 2 | INMP441 MEMS I2S Microphone Module | ✅ |
| 3 | CJMCU TRRS 3.5mm Jack Module | ✅ |

---

## Wiring

```
┌──────────────┐         ┌──────────┐         ┌────────────┐
│   INMP441    │         │  ESP32   │         │ CJMCU TRRS │
│  I2S Mic     │         │  DevKit  │         │  3.5mm Jack│
├──────────────┤         ├──────────┤         ├────────────┤
│  SCK ────────┼────────►│ GPIO 32  │         │            │
│  WS ─────────┼────────►│ GPIO 33  │         │            │
│  SD ─────────┼────────►│ GPIO 34  │         │            │
│  L/R ────────┼────────►│ GND      │         │            │
│  VDD ────────┼────────►│ 3.3V     │         │            │
│  GND ────────┼────────►│ GND ─────┼────────►│ RING2      │
└──────────────┘         │ GPIO 25 ─┼────────►│ TIP        │
                         │ GPIO 26 ─┼────────►│ RING1      │
                         └──────────┘         └────────────┘
                                                    │
                                               🎧 Earphones
```

### Wire-by-Wire

| From | To | Color Suggestion |
|------|----|:---:|
| INMP441 **SCK** | ESP32 **GPIO 32** | 🟡 Yellow |
| INMP441 **WS** | ESP32 **GPIO 33** | 🟠 Orange |
| INMP441 **SD** | ESP32 **GPIO 34** | 🟣 Purple |
| INMP441 **L/R** | ESP32 **GND** | ⚫ Black |
| INMP441 **VDD** | ESP32 **3.3V** | 🔴 Red |
| INMP441 **GND** | ESP32 **GND** | ⚫ Black |
| ESP32 **GPIO 25** | TRRS **TIP** | 🟢 Green |
| ESP32 **GPIO 26** | TRRS **RING1** | 🔵 Blue |
| ESP32 **GND** | TRRS **RING2** | ⚫ Black |

> **Note:** TRRS **SLEEVE** pin is left unconnected. Tying INMP441 **L/R** to GND selects the left channel.

---

## Architecture

```
INMP441 Mic → [I2S GPIO 32/33/34] → ESP32 DSP Core 1 → [DAC GPIO 25/26] → TRRS → 🎧
                                            ↕  BLE GATT
                               React Dashboard (Vercel)
                               ← Chrome/Edge Web Bluetooth
```

- **ESP32** advertises as `SoundAmp-IoT` over BLE. No WiFi AP needed.
- **Dashboard** is a React/Vite app hosted on Vercel. Uses Web Bluetooth API to connect directly to the ESP32.
- Pairing happens in the browser — no phone BT settings required.

---

## Arduino IDE Setup

### 1. Install ESP32 Board Support

1. Open Arduino IDE → **File** → **Preferences**
2. In **Additional Board Manager URLs**, add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Go to **Tools** → **Board** → **Boards Manager**
4. Search **"esp32"** → Install **"esp32 by Espressif Systems"** (v2.x or later)

### 2. Board Settings

| Setting | Value |
|---------|-------|
| Board | ESP32 Dev Module |
| Upload Speed | 921600 |
| CPU Frequency | 240MHz |
| Flash Size | 4MB |
| **Partition Scheme** | **Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)** |
| Port | (your COM port) |

> **Important:** The default partition scheme may not have enough space for the BLE stack. Use **Minimal SPIFFS**.

### 3. Upload

1. Open `SoundAmpIoT/SoundAmpIoT.ino` in Arduino IDE
2. Click **Upload** (→ button)
3. Open **Serial Monitor** (115200 baud) — you should see:
   ```
   [BLE]  Advertising as 'SoundAmp-IoT'
   [I2S]  INMP441 ready
   [AUDIO] DSP on Core 1
   Ready!
   ```

**No external libraries needed** — uses only built-in ESP32 BLE, I2S, and DAC drivers.

---

## Dashboard Setup

### Local development

```bash
cd dashboard
npm install
npm run dev        # opens https://localhost:5173 (HTTPS required for Web Bluetooth)
```

### Deploy to Vercel

```bash
cd dashboard
npx vercel         # follow prompts
```

Or connect the repo to Vercel and set the **Root Directory** to `dashboard`.

> Web Bluetooth requires HTTPS. Vercel provides this automatically. For local dev, Vite's `@vitejs/plugin-basic-ssl` provides a self-signed cert — accept the browser warning.

---

## How to Use

1. Flash the ESP32 and plug it into USB
2. Plug wired earphones into the CJMCU TRRS jack
3. Open the dashboard URL (Vercel) in **Chrome or Edge** on Android or Desktop
4. Click **Connect** → browser shows a BLE device picker → select **SoundAmp-IoT**
5. Ambient mode enables automatically — you can hear ambient sound through your earphones
6. Use the dashboard to switch modes, adjust gain, toggle noise gate / bass boost

> **iOS not supported** — Web Bluetooth is not available on Safari or any iOS browser.

---

## BLE GATT Reference

| Characteristic | UUID suffix | Type | Description |
|---|---|---|---|
| Mode | `...0002` | R/W uint8 | 0=Off, 1=Ambient, 2=ANC |
| Gain | `...0003` | R/W uint8 | value = gain × 10 (10=1.0x … 200=20.0x) |
| Noise Gate | `...0004` | R/W uint8 | 0=Off, 1=On |
| Bass Boost | `...0005` | R/W uint8 | 0=Off, 1=On |
| Level | `...0006` | Notify uint8 | VU meter, 0-255 |

Service UUID: `a1b2c3d4-0001-0001-0001-000000000001`

---

## Features

| Feature | Description |
|---------|-------------|
| **Ambient Mode** | Amplifies surrounding sounds — hear conversations while wearing earphones |
| **ANC Mode** | Basic active noise cancellation via phase inversion (experimental) |
| **Off Mode** | Silence — no audio processing |
| **Gain Control** | Adjustable 1x – 20x amplification |
| **Noise Gate** | Cuts silence noise when no sound is detected |
| **Bass Boost** | Enhances low frequencies |
| **Live VU Meter** | Visual audio level indicator on dashboard |

---

## Troubleshooting

| Problem | Fix |
|---------|-----|
| No sound in earphones | Check TRRS wiring: TIP→GPIO25, RING1→GPIO26, RING2→GND |
| No mic signal | Check INMP441: SCK→32, WS→33, SD→34, L/R→GND |
| BLE not advertising | Change Partition Scheme to **Minimal SPIFFS** in Arduino IDE |
| Device not appearing in picker | Make sure you're on Chrome/Edge — Firefox and Safari don't support Web Bluetooth |
| Dashboard can't connect | Must be on HTTPS (Vercel) or localhost with the dev server |
| iOS not working | Web Bluetooth is unavailable on iOS — use Android or Desktop Chrome |
| Distorted audio | Lower the gain on the dashboard |
| Compile error | Ensure ESP32 board package v2.x or later |

---

## Project Structure

```
sound-amplification-iot/
├── README.md
├── SoundAmpIoT/
│   └── SoundAmpIoT.ino          ← Arduino sketch (flash this)
└── dashboard/
    ├── package.json
    ├── vite.config.js
    ├── index.html
    └── src/
        ├── main.jsx
        ├── App.jsx
        ├── index.css
        ├── hooks/
        │   └── useSoundAmp.js   ← BLE logic (Web Bluetooth API)
        └── components/
            ├── Dashboard.jsx
            └── Dashboard.module.css
```

---

## License

MIT — use it however you want.
