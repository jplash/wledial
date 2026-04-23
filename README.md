# M5Dial WLED Remote Controller

WiFi-only WLED remote built on the M5Stack M5Dial (ESP32-S3).
No LEDs are attached to this device — it controls a WLED instance over the local network using the WLED JSON API.

---

## Files

| File | Description |
|---|---|
| `M5Dial_WLED_Remote.ino` | Arduino firmware for the M5Dial hardware |
| `simulator.html` | Browser-based simulator — open locally to test firmware logic without hardware |
| `README.md` | This file |
| `CHANGELOG.md` | Development history and notable changes |

---

## Simulator

Open `simulator.html` in any modern browser (no server needed — it's a single self-contained file).

- Click **Load firmware** to load the firmware into the editor
- Press **▶ Upload & Run** to execute it
- Use the **‹ ›** buttons to rotate the encoder
- Hold **●** to simulate a long press (cycles encoder mode)
- Click **●** briefly to toggle power
- Use the fault injection buttons to test WiFi drop, WLED 500 errors, and slow network behaviour

---

## Firmware

### Dependencies (Arduino Library Manager)

- **M5Dial** by M5Stack
- **WiFiManager** by tzapu
- **ArduinoJson** >= 6.x by Benoit Blanchon
- **WiFi** / **ESPmDNS** - included with ESP32 Arduino core

### Configuration

WiFi is configured on-device with WiFiManager. On first boot, or when saved credentials fail, connect your phone or laptop to:

```
M5Dial-WLED
```

Then open `http://192.168.4.1`, choose your WiFi network, enter the password, and save.

You can still edit the fallback WLED IP at the top of `M5Dial_WLED_Remote.ino`:

```cpp
const char* WLED_DEFAULT_IP = "192.168.1.100";   // fallback if mDNS finds nothing
```

### Controls

| Input | Action |
|---|---|
| Rotate encoder | Adjust active parameter |
| Short press | Toggle WLED power on/off |
| Long press (600ms) | Cycle encoder mode |

### Encoder modes

| Mode | Range | WLED API field |
|---|---|---|
| BRIGHTNESS | 0–255 | `bri` |
| COLOR TEMP | 1900–6500 K | `cct` (mapped 0–255) |
| HUE | 0–360° | `seg[0].col` (RGB) |
| FX SPEED | 0–255 | `seg[0].sx` |
| FX SELECT | 0–N | `seg[0].fx` |

### WLED JSON API endpoints used

```
GET  http://<WLED_IP>/json/eff     — fetch effect list on boot
GET  http://<WLED_IP>/json/state   — background poll every 5s
POST http://<WLED_IP>/json/state   — queued control updates
```

---

## Board setup (Arduino IDE)

1. Install ESP32 board package via Boards Manager
2. Select **M5Stack Dial**
3. Set **Partition Scheme** to **Minimal SPIFFS**
4. Set PSRAM: **OPI PSRAM**
5. Close any Serial Monitor using the board's COM port before uploading
6. Flash and open Serial Monitor at 115200 baud

### Exporting a fresh firmware `.bin`

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\build-firmware.ps1
```

This compiles the sketch with `esp32:esp32:m5stack_dial:PartitionScheme=min_spiffs`,
puts the build artifacts in `.arduino-build\`, and refreshes:

```text
firmware\M5Dial_WLED_Remote.bin
```
