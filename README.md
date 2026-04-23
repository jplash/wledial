# M5Dial WLED Remote Controller

WiFi-only WLED remote built on the M5Stack M5Dial (ESP32-S3).
No LEDs are attached to this device — it controls a WLED instance over the local network using the WLED JSON API.

---

## Files

| File | Description |
|---|---|
| `M5Dial_WLED_Remote.ino` | Arduino firmware for the M5Dial hardware |
| `build-firmware.ps1` | Compile firmware and export the flashable binary |
| `recover-flash.ps1` | Erase and reflash the M5Dial from the exported firmware image |
| `firmware/` | Exported firmware binary |
| `README.md` | This file |
| `CHANGELOG.md` | Development history and notable changes |

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

### Control menus

| Mode | Range | WLED API field |
|---|---|---|
| BRIGHTNESS | 0-255 | `bri` |
| PALETTE | 0-N | `seg[0].pal` |
| PRESET | 1-250 | `ps` |
| FX PATTERN | 0-N | `seg[0].fx` |
| FX SPEED | 0-255 | `seg[0].sx` |
| FX INTENSITY | 0-255 | `seg[0].ix` |

### WLED JSON API endpoints used

```
GET  http://<WLED_IP>/json/eff     - fetch effect list on boot/controller refresh
GET  http://<WLED_IP>/json/pal     - fetch palette list on boot/controller refresh
GET  http://<WLED_IP>/presets.json - fetch preset list on boot/controller refresh
GET  http://<WLED_IP>/json/state   - background poll every 5s
POST http://<WLED_IP>/json/state   - queued control and preset updates
```

---

## Board setup (Arduino IDE)

1. Install ESP32 board package via Boards Manager
2. Select **M5Stack Dial**
3. Set **Partition Scheme** to **Minimal SPIFFS**
4. Set PSRAM: **OPI PSRAM**
5. Close any Serial Monitor using the board's COM port before uploading
6. Flash and open Serial Monitor at 115200 baud

### Exporting fresh firmware images

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\build-firmware.ps1
```

This compiles the sketch with `esp32:esp32:m5stack_dial:PartitionScheme=min_spiffs`,
puts the build artifacts in `.arduino-build\`, and refreshes:

```text
firmware\M5Dial_WLED_Remote.bin         app image, flash at 0x10000
firmware\M5Dial_WLED_Remote.merged.bin  full flash image, flash at 0x0
```

Use the merged image for recovery or first-time full-device flashing. Flashing
the app image at `0x0` will boot with an invalid-header error because the
bootloader belongs there.
