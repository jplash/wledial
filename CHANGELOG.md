# Changelog

All notable changes to this project will be documented in this file.

This changelog was started after the first round of development work, so the
initial entry captures the codebase changes made so far rather than a single
atomic release.

## [0.5.1] - 2026-04-24

### Fixed

- Fixed controller display names by parsing the WLED `/json/info` `name`
  field with a filtered JSON document instead of falling back when the full
  info payload is too large.
- Removed the blocking live refresh after selecting a controller; cached state
  is shown immediately while state and catalog data refresh in the background.
- Moved the HTTP status dot away from the bottom main-menu ring so it no longer
  overlaps the `FX Intensity` icon.

## [0.5.0] - 2026-04-22

### Added

- Added a `PRE` preset selection menu, populated from WLED's `/presets.json`
  file, that applies presets with queued `{"ps": <id>}` state updates.
- Added `build-firmware.ps1` to compile the sketch with
  `esp32:esp32:m5stack_dial:PartitionScheme=min_spiffs`, copy artifacts into
  `.arduino-build`, and refresh the app and merged images in `firmware/`.
- Added `recover-flash.ps1` to erase and fully reflash the M5Dial from the
  repo's merged firmware image using `esptool`.
- Added controller-state caching in `Preferences` so the UI can immediately
  show the last known WLED power, brightness, FX, palette, and label data
  before fresh network polling completes.

### Changed

- Removed the browser simulator from the repo and cleaned the README so the
  project is focused on the M5Dial firmware and flash/recovery scripts.
- Moved the 5-second WLED state poll and debounced control POSTs onto a
  background FreeRTOS worker so slow controller HTTP responses no longer block
  touch, encoder, or button handling. Background refresh results are discarded
  if the user interacts while the request is in flight, and repeated control
  changes are coalesced to the latest state instead of queued as stale sends.
- Deferred controller-cache writes until the dial is idle and no control send
  is queued or in flight, avoiding flash writes in the active control path.
- Updated README board instructions to match the actual build target:
  **M5Stack Dial**, **Minimal SPIFFS**, **OPI PSRAM**, and closing any active
  serial monitor before upload.
- Reduced navigation lag by loading cached controller state before live WLED
  refreshes and by suppressing background polling while the user is actively
  turning, tapping, or adjusting controls.
- Simplified the `PAL` menu item from `Palette / Hue` to a palette-only
  browser. Hue is no longer exposed as a user-facing menu control.
- Reworked the palette screen from a shared palette/hue editor into a dedicated
  palette picker with nearby entries shown above and below the current
  selection.
- Reduced the list-view selected-item typography so the focused entry is only
  slightly larger than neighboring items instead of jumping to a large display
  font.
- Cleaned the packaged project contents down to source, docs, scripts,
  simulator, and exported firmware only.

### Fixed

- Fixed WiFi persistence by storing credentials in the app's own `Preferences`
  namespace instead of relying solely on WiFiManager's internal save behavior.
- Fixed WiFi recovery after portal submission by reading the ESP32 station
  config directly with `esp_wifi_get_config(...)`, importing it into
  `Preferences`, and reconnecting from that stored data on boot.
- Fixed stale firmware export confusion by ensuring the documented build flow
  produces a fresh `.bin` in the repo rather than leaving the newest artifact
  hidden in transient Arduino build locations.

### Verified

- Verified the updated firmware compiles successfully after the WiFi,
  controller-cache, palette-only UI, and list typography changes.
- Verified full erase-and-reflash recovery on `COM8` using the merged firmware
  image; flash verification passed after write.
- Verified WiFi settings now persist across power cycles after the clean reflash.
- Created a cleaned shareable archive at `gen2-package.zip`.

## [Unreleased] - 2026-04-16 (session 4)

### Added

- Added a radial home menu for the main control flow with these entries in
  order: `Select Light`, `Brightness`, `Palette / Hue`, `FX Pattern`,
  `FX Speed`, `FX Intensity`, and `Settings`.
- Added dedicated control screens for the new home-menu flow, including a
  combined `Palette / Hue` screen and a `Settings` screen.
- Added persistent local settings storage for screen brightness and click
  volume using `Preferences`.
- Added small drawn icons for each main menu item so the outer ring uses
  pictograms instead of text badges.
- Added firmware version constant `APP_VERSION` and bumped it to `0.4.0`.

### Changed

- Reworked encoder handling to use detent-style stepping via
  `Encoder.readAndReset()` and a 4-count step accumulator. This replaces the
  previous raw absolute-count approach and is intended to make rotation feel
  closer to the stock M5Dial examples.
- Simplified WLED brightness, FX speed, and FX intensity controls from the raw
  `0-255` range to an on-device `1-11` scale, while still mapping back to
  WLED's byte values when sending state.
- Replaced the previous mode-cycling interaction with a home-menu-driven UI so
  short press opens the selected item, long press returns to Home, and the Home
  screen long press toggles light power.
- Combined palette selection and hue control into a shared `Palette / Hue`
  screen and added palette preview swatches to show an approximate color sample
  for the active palette.
- Expanded Settings so it now covers WiFi setup, display brightness, and click
  volume.
- Rebuilt the sketch using `esp32:esp32:m5stack_dial:PartitionScheme=min_spiffs`
  to preserve comfortable app headroom after the UI changes.

### Fixed

- Fixed the WiFi settings save path by removing the mixed `autoConnect()` /
  `startConfigPortal()` behavior. Boot now attempts a direct reconnect using
  saved station credentials first, while the on-demand WiFi settings action
  runs through a single shared portal path.
- Updated the on-demand WiFi portal flow to use
  `setSaveConnect(false)`, `setBreakAfterConfig(true)`, and
  `setSaveConfigCallback(...)`, then explicitly reconnect with saved station
  credentials after the user presses Save. This addresses the case where the
  portal appeared to save settings without reliably returning to a connected
  WiFi state.

## [Unreleased] - 2026-04-15 (session 3)

### Added

- Added return-to-controller-select gesture: hold the button for 3 seconds in
  control mode to re-scan for WLED controllers and return to the selection
  screen. A low beep confirms the hold threshold was reached. The cursor is
  pre-positioned on the currently active controller if it is found again.
- Added fallback in the return-to-select path: if re-scan finds nothing, the
  currently connected controller is inserted into the list so the selection
  screen is never shown empty.

### Changed

- Replaced the custom pixel-by-pixel `drawArc()` implementation (220 steps ×
  18 pixels = 3,960 individual `drawPixel()` SPI calls per frame) with two
  `M5GFX fillArc()` calls. This is the root cause of the sluggish encoder
  response; the display was spending ~10–20 ms per tick redrawing the arc,
  causing the encoder to appear to skip steps under fast rotation.
- `discoverWLED()` now fetches each controller's friendly name from
  `/json/info` (`name` field) instead of using the raw mDNS SRV hostname.
  The mDNS hostname (e.g. `wled-abc123`) is kept as a fallback if the request
  fails. The fallback default-IP probe is updated the same way.
- Added `MDNS.end()` before `MDNS.begin()` in `discoverWLED()` so re-scans
  start from a clean mDNS state.
- Increased WiFiManager `setConnectTimeout` from 12 s to 30 s. The previous
  value was too short for slow or cold-booting routers, causing `autoConnect`
  to give up and re-open the setup portal even with valid saved credentials.
- Added explicit `wm.setSaveConnect(true)` to ensure credentials are persisted
  regardless of WiFiManager build defaults.

## [Unreleased] - 2026-04-15 (session 2)

### Added

- Added WiFiManager-based setup so users can configure WiFi on-device through
  the `M5Dial-WLED` setup portal at `http://192.168.4.1`.
- Added an on-device WiFi setup screen shown while the WiFiManager portal is
  active.
- Added mDNS WLED discovery and controller selection flow for multiple WLED
  controllers on the local network.
- Added fallback probing of `WLED_DEFAULT_IP` when mDNS does not find a WLED
  controller.
- Added touch support for the M5Dial display.
- Added touch navigation arrows for moving left and right between modes or
  controller selections.
- Added drag-to-set behavior on numeric arc modes so users can jump directly to
  a value instead of only turning the encoder.
- Added vertical drag behavior for the FX and palette list picker modes.
- Added support for FX intensity and palette selection modes.
- Added a self-contained browser simulator in `simulator.html` for testing the
  firmware logic without hardware.
- Added simulator pointer/touch handling for click and drag interactions.
- Added simulator fault controls for WiFi drop, WLED HTTP 500 errors, and slow
  network behavior.
- Added exported firmware binary at `firmware/M5Dial_WLED_Remote.bin`.

### Changed

- Replaced hard-coded WiFi SSID/password setup with WiFiManager-managed saved
  credentials.
- Widened the value arc on numeric control faces for easier viewing and touch
  targeting.
- Moved navigation arrows inward on arc faces so they no longer overlap the arc
  ring.
- Kept navigation arrows wide on FX and palette list faces to preserve list
  readability.
- Updated touch hit zones to match the visible arrow layout for each face type.
- Replaced Arduino `HTTPClient` usage in the firmware with a smaller
  `WiFiClient`-based HTTP helper to reduce sketch size.
- Updated mDNS IP retrieval for ESP32 core compatibility by using
  `MDNS.address(i).toString()`.
- Updated README setup instructions for WiFiManager, dependencies, controls,
  and firmware configuration.

### Fixed

- Fixed compile failure with ESP32 Arduino core 3.3.8 caused by `MDNS.IP(i)`.
- Fixed the default partition build being too large after WiFiManager was
  introduced by reducing HTTP client code size.
- Fixed simulator mDNS compatibility by adding an `address(i)` shim while
  preserving the older `IP(i)` helper.
- Fixed simulator FX/palette list entries rendering as `undefined` by parsing
  the mock JSON arrays directly in the embedded simulator firmware.
- Fixed simulator and firmware arrow placement drift by updating both copies of
  the drawing and touch logic.

### Verified

- Verified firmware compile using Arduino CLI with board
  `esp32:esp32:m5stack_dial`.
- Verified default partition build passes after size reduction.
- Current build size after the latest changes:
  - Program storage: `1,277,447 bytes` of `1,310,720 bytes` (`97%`)
  - Dynamic memory: `62,012 bytes` of `327,680 bytes` (`18%`)
- Verified simulator script parses successfully.
- Verified simulator embedded firmware transpiles and loads mock effect/palette
  names such as `Solid`, `Blink`, and `Default`.
