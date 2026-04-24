/**
 * M5Dial WLED Remote Controller
 * Controls a WLED instance over WiFi using the WLED JSON API.
 */

#include <M5Dial.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

const char* APP_VERSION      = "0.5.1";
const char* WLED_DEFAULT_IP   = "192.168.1.100";
const int   WLED_DEFAULT_PORT = 80;

const unsigned long POLL_INTERVAL_MS = 5000;
const unsigned long LONG_PRESS_MS    = 650;
const unsigned long SEND_DEBOUNCE_MS = 80;
const unsigned long INPUT_GRACE_MS   = 1500;
const unsigned long CACHE_SAVE_IDLE_MS = 2000;
const int UI_LEVEL_MIN               = 1;
const int UI_LEVEL_MAX               = 11;
const int ENCODER_COUNTS_PER_STEP    = 4;
const int LIST_DRAG_STEP_PX          = 24;

#define CLR_BLACK   0x0000
#define CLR_WHITE   0xFFFF
#define CLR_DGRAY   0x2104
#define CLR_LGRAY   0x8410
#define CLR_ACCENT  0xFD20
#define CLR_GREEN   0x07E0
#define CLR_RED     0xF800
#define CLR_BLUE    0x001F
#define CLR_CYAN    0x07FF
#define CLR_MAGENTA 0xF81F
#define CLR_ORANGE  0xFC00
#define CLR_YELLOW  0xFFE0

#define MAX_CONTROLLERS 8
#define MAX_EFFECTS 256
#define MAX_PALETTES 256
#define MAX_PRESETS 250

enum AppState { STATE_SCANNING, STATE_SELECT, STATE_CONTROL };
enum ControlScreen {
  SCREEN_MAIN_MENU = 0,
  SCREEN_BRIGHTNESS,
  SCREEN_PALETTE_HUE,
  SCREEN_PRESET,
  SCREEN_FX_PATTERN,
  SCREEN_FX_SPEED,
  SCREEN_FX_INTENSITY,
  SCREEN_SETTINGS
};
enum ListKind { LIST_CONTROLLERS, LIST_EFFECTS, LIST_PRESETS };
enum SettingsItem { SETTING_WIFI = 0, SETTING_SCREEN_BRIGHTNESS, SETTING_CLICK_VOLUME, SETTING_COUNT };

struct WLEDController {
  String name;
  String ip;
  int port;
};

struct PresetEntry {
  int id;
  String name;
};

struct MenuItem {
  const char* shortLabel;
  const char* title;
  ControlScreen screen;
  uint16_t color;
};

struct WLEDStateSnapshot {
  bool on;
  uint8_t bri;
  int hue;
  uint8_t fxSpeed;
  int fxIndex;
  uint8_t fxIntensity;
  int palIndex;
  int presetId;
};

const MenuItem MAIN_MENU_ITEMS[] = {
  {"SEL", "Select Light",  SCREEN_MAIN_MENU,    CLR_CYAN},
  {"BRI", "Brightness",    SCREEN_BRIGHTNESS,   CLR_ACCENT},
  {"PAL", "Palette",       SCREEN_PALETTE_HUE,  CLR_ORANGE},
  {"PRE", "Preset",        SCREEN_PRESET,       CLR_YELLOW},
  {"FX",  "FX Pattern",    SCREEN_FX_PATTERN,   CLR_BLUE},
  {"SPD", "FX Speed",      SCREEN_FX_SPEED,     CLR_GREEN},
  {"INT", "FX Intensity",  SCREEN_FX_INTENSITY, CLR_MAGENTA},
  {"SET", "Settings",      SCREEN_SETTINGS,     CLR_LGRAY},
};
const int MAIN_MENU_COUNT = sizeof(MAIN_MENU_ITEMS) / sizeof(MAIN_MENU_ITEMS[0]);

WLEDController controllers[MAX_CONTROLLERS];
String effectNames[MAX_EFFECTS];
String paletteNames[MAX_PALETTES];
PresetEntry presetEntries[MAX_PRESETS];

AppState appState = STATE_CONTROL;
ControlScreen controlScreen = SCREEN_MAIN_MENU;
SettingsItem settingsCursor = SETTING_WIFI;

int controllerCount = 0;
int selectionCursor = 0;
int effectCount = 0;
int paletteCount = 0;
int presetCount = 0;
int presetCursor = 0;
int mainMenuIndex = 0;

String wledHost;
int wledPort = 80;

bool wled_on = true;
uint8_t wled_bri = 128;
int wled_hue = 0;
uint8_t wled_fxSpeed = 128;
int wled_fxIndex = 0;
uint8_t wled_fxIntensity = 128;
int wled_palIndex = 0;
int wled_presetId = -1;

bool pendingSend = false;
bool pendingPresetApply = false;
int pendingPresetId = -1;
unsigned long lastSendMs = 0;
unsigned long lastPollMs = 0;
unsigned long lastInteractionMs = 0;
int lastHttpCode = 0;
bool wifiConnected = false;
bool settingsEditing = false;
bool wifiPortalDidSave = false;
String cachedEffectLabel;
String cachedPaletteLabel;
String cachedPresetLabel;

Preferences preferences;
uint8_t screenBrightness = 180;
uint8_t clickVolume = 64;

bool btnHeld = false;
bool longFired = false;
unsigned long btnPressedAt = 0;
int encoderPulseRemainder = 0;
unsigned long lastTouchSetMs = 0;
bool arcTouchActive = false;
int listDragAccumY = 0;
bool cacheSavePending = false;
unsigned long cacheSaveNotBeforeMs = 0;

SemaphoreHandle_t networkMutex = nullptr;
TaskHandle_t networkTaskHandle = nullptr;
bool networkSendRequested = false;
bool networkSendRequestedIsPreset = false;
bool networkSendInFlight = false;
bool networkSendInFlightIsPreset = false;
bool networkSendSuccessPending = false;
bool networkSendSuccessWasPreset = false;
String networkSendHost;
int networkSendPort = 80;
String networkSendBody;
bool networkFetchRequested = false;
bool networkFetchInFlight = false;
bool networkFetchResultPending = false;
bool networkFetchRedraw = false;
String networkFetchHost;
int networkFetchPort = 80;
uint32_t networkFetchInteractionSeq = 0;
uint32_t networkFetchControllerSeq = 0;
WLEDStateSnapshot networkFetchState;
bool networkCatalogRequested = false;
bool networkCatalogInFlight = false;
bool networkCatalogResultPending = false;
bool networkCatalogRedraw = false;
bool networkCatalogEffectsReady = false;
bool networkCatalogPalettesReady = false;
bool networkCatalogPresetsReady = false;
String networkCatalogHost;
int networkCatalogPort = 80;
uint32_t networkCatalogControllerSeq = 0;
String networkEffectNames[MAX_EFFECTS];
String networkPaletteNames[MAX_PALETTES];
PresetEntry networkPresetEntries[MAX_PRESETS];
int networkEffectCount = 0;
int networkPaletteCount = 0;
int networkPresetCount = 0;
uint32_t interactionSeq = 0;
uint32_t controllerSeq = 0;

const int ARC_CX = 120;
const int ARC_CY = 120;
const int ARC_RAD = 104;
const int ARC_THICK = 18;
const float ARC_START_DEG = 135.0f;
const float ARC_SWEEP_DEG = 270.0f;

const int MENU_CX = 120;
const int MENU_CY = 118;
const int MENU_RING_R = 92;
const int MENU_ITEM_R = 18;
const int MENU_ITEM_R_SEL = 23;
const float MENU_ANGLES_DEG[MAIN_MENU_COUNT] = {225.0f, 260.0f, 295.0f, 330.0f, 25.0f, 60.0f, 95.0f, 130.0f};

void drawDisplay();
void discoverWLED();
void sendState();
int httpRequest(const String& host, int port, const char* method, const char* path, const String& body, String* payload);
void fetchState();
void fetchEffects();
void fetchPalettes();
void fetchPresets();
void wifiConnect();
bool connectSavedWifi(unsigned long timeoutMs);
bool connectWifiCredentials(const String& ssid, const String& pass, unsigned long timeoutMs);
bool waitForWifiConnection(unsigned long timeoutMs, const char* label);
const char* wifiStatusName(wl_status_t status);
void saveStoredWifiCredentials(const String& ssid, const String& pass);
void loadStoredWifiCredentials(String& ssid, String& pass);
bool hasStoredWifiCredentials();
bool readEspStoredWifiCredentials(String& ssid, String& pass);
void noteInteraction();
void noteControllerChanged();
void clearControllerCatalogs();
void queueStateSend();
void queuePresetApply(int presetId);
void scheduleControllerCacheSave();
bool backgroundControlSendBusy();
void processDeferredCacheSave();
void startNetworkWorker();
void networkWorkerTask(void* param);
void captureCurrentState(WLEDStateSnapshot& state);
void applyStateSnapshot(const WLEDStateSnapshot& state);
bool parseStatePayload(const String& payload, WLEDStateSnapshot& state);
String buildJsonFromSnapshot(const WLEDStateSnapshot& state);
String buildPresetJson(int presetId);
bool requestBackgroundStateSend();
bool requestBackgroundStateFetch(bool redrawAfter);
bool requestBackgroundCatalogFetch(bool redrawAfter);
void processNetworkResults();
void updateCachedLabels();
int findPresetIndexById(int presetId);
void syncPresetCursorToCurrentPreset();
void saveControllerCache();
bool loadControllerCache(const String& host, int port);
void refreshActiveController(bool redrawAfter);
void drawWifiPortal();
void onWifiPortal(WiFiManager* wm);
void onWifiConfigSaved();
void handleTouch();
void applyEncoderSteps(int steps);
void handleShortPress();
void handleLongPress();
bool touchToArcFraction(int x, int y, float* fraction);
bool touchAngleToFraction(int x, int y, float* fraction);
bool handleListDrag(const m5::touch_detail_t& t);
String buildJson();
String parseWLEDNameFromInfo(const String& payload, const String& fallback);
uint16_t hue16(int hue);
void drawArc(float fraction, uint16_t color);
void openMainMenu();
void openScreen(ControlScreen screen);
void startControllerSelection(bool rescan);
void selectCurrentController();
void beginWifiPortal();
bool runWifiPortal(bool returnToSettings);
void drawMainMenu();
void drawMenuIcon(int index, int cx, int cy, bool selected);
void drawListScreen(const char* title, ListKind kind);
void drawPaletteHueScreen();
void drawNumericScreen(const char* title, int displayValue, const char* suffix, float fraction, uint16_t color, const String& secondary);
void drawSettingsScreen();
void drawStatusDot(int x, int y);
void drawPalettePreview(int cx, int cy, int w, int h, const String& paletteName);
void getPalettePreviewColors(const String& paletteName, uint16_t* outColors, int count);
void loadPreferences();
void savePreferences();
void applyLocalSettings();
void playClick(int freq, int duration);
void resetEncoderState();
int readEncoderSteps();
int byteToLevel(uint8_t value);
uint8_t levelToByte(int level);
int brightnessToUi(uint8_t value);
String currentControllerLabel();
String currentScreenSummary();
String currentEffectLabel();
String currentPaletteLabel();
String currentPresetLabel();
void drawCenteredLabel(const String& text, int y, uint16_t color, const lgfx::IFont* font);
bool pointInCircle(int x, int y, int cx, int cy, int r);
bool containsIgnoreCase(const String& text, const char* needle);
String wifiStatusText();
bool fetchEffectsFor(const String& host, int port, String* names, int& count);
bool fetchPalettesFor(const String& host, int port, String* names, int& count);
bool fetchPresetsFor(const String& host, int port, PresetEntry* presets, int& count);

void loadPreferences() {
  preferences.begin("m5dial-wled", false);
  screenBrightness = preferences.getUChar("screen", 180);
  clickVolume = preferences.getUChar("click", 64);
}

void savePreferences() {
  preferences.putUChar("screen", screenBrightness);
  preferences.putUChar("click", clickVolume);
}

void saveStoredWifiCredentials(const String& ssid, const String& pass) {
  if (ssid.length() == 0) return;
  preferences.putString("wifi_ssid", ssid);
  preferences.putString("wifi_pass", pass);
  Serial.printf("Stored WiFi credentials in Preferences for '%s'\n", ssid.c_str());
}

void loadStoredWifiCredentials(String& ssid, String& pass) {
  ssid = preferences.getString("wifi_ssid", "");
  pass = preferences.getString("wifi_pass", "");
}

bool hasStoredWifiCredentials() {
  return preferences.getString("wifi_ssid", "").length() > 0;
}

void noteInteraction() {
  lastInteractionMs = millis();
  interactionSeq++;
}

void noteControllerChanged() {
  controllerSeq++;
  interactionSeq++;
}

void clearControllerCatalogs() {
  effectCount = 0;
  paletteCount = 0;
  presetCount = 0;
  presetCursor = 0;
  cachedEffectLabel = "";
  cachedPaletteLabel = "";
  cachedPresetLabel = "";
  lastHttpCode = 0;
}

void queueStateSend() {
  pendingPresetApply = false;
  pendingPresetId = -1;
  pendingSend = true;
}

void queuePresetApply(int presetId) {
  if (presetId <= 0) return;
  pendingPresetApply = true;
  pendingPresetId = presetId;
  pendingSend = true;
}

void scheduleControllerCacheSave() {
  cacheSavePending = true;
  cacheSaveNotBeforeMs = millis() + CACHE_SAVE_IDLE_MS;
}

bool backgroundControlSendBusy() {
  if (!networkMutex) return false;
  if (xSemaphoreTake(networkMutex, pdMS_TO_TICKS(5)) != pdTRUE) return true;
  bool busy = networkSendRequested || networkSendInFlight;
  xSemaphoreGive(networkMutex);
  return busy;
}

void processDeferredCacheSave() {
  if (!cacheSavePending || pendingSend) return;
  unsigned long now = millis();
  if ((int32_t)(now - cacheSaveNotBeforeMs) < 0) return;
  if (now - lastInteractionMs < INPUT_GRACE_MS) return;
  if (backgroundControlSendBusy()) return;
  saveControllerCache();
  cacheSavePending = false;
}

void startNetworkWorker() {
  if (!networkMutex) {
    networkMutex = xSemaphoreCreateMutex();
  }
  if (!networkMutex || networkTaskHandle) return;
  if (xTaskCreatePinnedToCore(networkWorkerTask, "wled-net", 8192, nullptr, 1, &networkTaskHandle, 0) != pdPASS) {
    networkTaskHandle = nullptr;
  }
}

void captureCurrentState(WLEDStateSnapshot& state) {
  state.on = wled_on;
  state.bri = wled_bri;
  state.hue = wled_hue;
  state.fxSpeed = wled_fxSpeed;
  state.fxIndex = wled_fxIndex;
  state.fxIntensity = wled_fxIntensity;
  state.palIndex = wled_palIndex;
  state.presetId = wled_presetId;
}

void applyStateSnapshot(const WLEDStateSnapshot& state) {
  wled_on = state.on;
  wled_bri = state.bri;
  wled_hue = state.hue;
  wled_fxSpeed = state.fxSpeed;
  wled_fxIndex = state.fxIndex;
  wled_fxIntensity = state.fxIntensity;
  wled_palIndex = state.palIndex;
  wled_presetId = state.presetId;
  syncPresetCursorToCurrentPreset();
  updateCachedLabels();
}

bool parseStatePayload(const String& payload, WLEDStateSnapshot& state) {
  StaticJsonDocument<1536> doc;
  if (deserializeJson(doc, payload)) return false;

  state.on = doc["on"] | state.on;
  state.bri = doc["bri"] | state.bri;
  state.fxIndex = doc["seg"][0]["fx"] | state.fxIndex;
  state.fxSpeed = doc["seg"][0]["sx"] | state.fxSpeed;
  state.fxIntensity = doc["seg"][0]["ix"] | state.fxIntensity;
  state.palIndex = doc["seg"][0]["pal"] | state.palIndex;
  state.presetId = doc["ps"] | state.presetId;
  JsonArray col = doc["seg"][0]["col"][0];
  if (!col.isNull() && col.size() >= 3) {
    int r = col[0] | 0;
    int g = col[1] | 0;
    int b = col[2] | 0;
    float maxv = max(r, max(g, b));
    float minv = min(r, min(g, b));
    float delta = maxv - minv;
    if (delta > 0.0f) {
      float hue;
      if (maxv == r) hue = 60.0f * fmodf(((g - b) / delta), 6.0f);
      else if (maxv == g) hue = 60.0f * (((b - r) / delta) + 2.0f);
      else hue = 60.0f * (((r - g) / delta) + 4.0f);
      if (hue < 0) hue += 360.0f;
      state.hue = (int)roundf(hue) % 360;
    }
  }
  return true;
}

bool requestBackgroundStateSend() {
  if (!networkMutex || !networkTaskHandle) return false;

  WLEDStateSnapshot state;
  captureCurrentState(state);
  String body = pendingPresetApply ? buildPresetJson(pendingPresetId) : buildJsonFromSnapshot(state);
  if (body.length() == 0) return false;

  if (xSemaphoreTake(networkMutex, pdMS_TO_TICKS(25)) != pdTRUE) return false;
  networkSendHost = wledHost;
  networkSendPort = wledPort;
  networkSendBody = body;
  networkSendRequestedIsPreset = pendingPresetApply;
  networkSendRequested = true;
  xSemaphoreGive(networkMutex);
  return true;
}

bool requestBackgroundStateFetch(bool redrawAfter) {
  if (!networkMutex || !networkTaskHandle) return false;
  if (xSemaphoreTake(networkMutex, pdMS_TO_TICKS(25)) != pdTRUE) return false;
  if (networkFetchRequested || networkFetchInFlight) {
    xSemaphoreGive(networkMutex);
    return true;
  }
  networkFetchHost = wledHost;
  networkFetchPort = wledPort;
  networkFetchInteractionSeq = interactionSeq;
  networkFetchControllerSeq = controllerSeq;
  networkFetchRedraw = redrawAfter;
  captureCurrentState(networkFetchState);
  networkFetchRequested = true;
  xSemaphoreGive(networkMutex);
  return true;
}

bool requestBackgroundCatalogFetch(bool redrawAfter) {
  if (!networkMutex || !networkTaskHandle || wledHost.length() == 0) return false;
  if (xSemaphoreTake(networkMutex, pdMS_TO_TICKS(25)) != pdTRUE) return false;
  if (networkCatalogRequested || networkCatalogInFlight || networkCatalogResultPending) {
    xSemaphoreGive(networkMutex);
    return true;
  }
  networkCatalogHost = wledHost;
  networkCatalogPort = wledPort;
  networkCatalogControllerSeq = controllerSeq;
  networkCatalogRedraw = redrawAfter;
  networkCatalogRequested = true;
  xSemaphoreGive(networkMutex);
  return true;
}

void processNetworkResults() {
  bool sendSucceeded = false;
  bool sendWasPreset = false;
  bool fetchReady = false;
  bool redrawAfterFetch = false;
  uint32_t fetchInteraction = 0;
  uint32_t fetchController = 0;
  WLEDStateSnapshot fetchedState;
  bool catalogReady = false;
  bool redrawAfterCatalog = false;
  uint32_t catalogController = 0;

  if (!networkMutex) return;
  if (xSemaphoreTake(networkMutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
  if (networkSendSuccessPending) {
    networkSendSuccessPending = false;
    sendSucceeded = true;
    sendWasPreset = networkSendSuccessWasPreset;
  }
  if (networkFetchResultPending) {
    networkFetchResultPending = false;
    fetchReady = true;
    redrawAfterFetch = networkFetchRedraw;
    fetchInteraction = networkFetchInteractionSeq;
    fetchController = networkFetchControllerSeq;
    fetchedState = networkFetchState;
  }
  if (networkCatalogResultPending) {
    networkCatalogResultPending = false;
    catalogReady = true;
    redrawAfterCatalog = networkCatalogRedraw;
    catalogController = networkCatalogControllerSeq;
    bool catalogMatchesController = (catalogController == controllerSeq);
    if (catalogMatchesController && networkCatalogEffectsReady) {
      effectCount = networkEffectCount;
      for (int i = 0; i < effectCount; ++i) effectNames[i] = networkEffectNames[i];
    }
    if (catalogMatchesController && networkCatalogPalettesReady) {
      paletteCount = networkPaletteCount;
      for (int i = 0; i < paletteCount; ++i) paletteNames[i] = networkPaletteNames[i];
    }
    if (catalogMatchesController && networkCatalogPresetsReady) {
      presetCount = networkPresetCount;
      for (int i = 0; i < presetCount; ++i) presetEntries[i] = networkPresetEntries[i];
    }
    networkCatalogEffectsReady = false;
    networkCatalogPalettesReady = false;
    networkCatalogPresetsReady = false;
  }
  xSemaphoreGive(networkMutex);

  if (sendSucceeded) {
    scheduleControllerCacheSave();
    if (sendWasPreset) {
      lastPollMs = 0;
      requestBackgroundStateFetch(true);
    }
  }

  if (fetchReady &&
      fetchInteraction == interactionSeq &&
      fetchController == controllerSeq &&
      appState == STATE_CONTROL &&
      !pendingSend) {
    applyStateSnapshot(fetchedState);
    scheduleControllerCacheSave();
    if (redrawAfterFetch) drawDisplay();
  }

  if (catalogReady &&
      catalogController == controllerSeq &&
      appState == STATE_CONTROL) {
    syncPresetCursorToCurrentPreset();
    updateCachedLabels();
    scheduleControllerCacheSave();
    if (redrawAfterCatalog) drawDisplay();
  }
}

void networkWorkerTask(void* param) {
  (void)param;
  for (;;) {
    bool doSend = false;
    bool doFetch = false;
    bool doCatalog = false;
    String host;
    int port = 80;
    String body;
    WLEDStateSnapshot state;
    uint32_t fetchInteraction = 0;
    uint32_t fetchController = 0;
    uint32_t catalogController = 0;
    bool redrawAfterFetch = false;
    bool redrawAfterCatalog = false;
    bool sendWasPreset = false;

    if (networkMutex && xSemaphoreTake(networkMutex, portMAX_DELAY) == pdTRUE) {
      if (networkSendRequested) {
        networkSendRequested = false;
        networkSendInFlight = true;
        sendWasPreset = networkSendRequestedIsPreset;
        networkSendInFlightIsPreset = sendWasPreset;
        host = networkSendHost;
        port = networkSendPort;
        body = networkSendBody;
        doSend = true;
      } else if (networkFetchRequested) {
        networkFetchRequested = false;
        networkFetchInFlight = true;
        host = networkFetchHost;
        port = networkFetchPort;
        state = networkFetchState;
        fetchInteraction = networkFetchInteractionSeq;
        fetchController = networkFetchControllerSeq;
        redrawAfterFetch = networkFetchRedraw;
        doFetch = true;
      } else if (networkCatalogRequested) {
        networkCatalogRequested = false;
        networkCatalogInFlight = true;
        host = networkCatalogHost;
        port = networkCatalogPort;
        catalogController = networkCatalogControllerSeq;
        redrawAfterCatalog = networkCatalogRedraw;
        doCatalog = true;
      }
      xSemaphoreGive(networkMutex);
    }

    if (doSend) {
      Serial.printf("POST -> %s\n", body.c_str());
      int code = httpRequest(host, port, "POST", "/json/state", body, nullptr);
      Serial.printf("HTTP %d\n", code);
      if (networkMutex && xSemaphoreTake(networkMutex, portMAX_DELAY) == pdTRUE) {
        lastHttpCode = code;
        if (code == 200) {
          networkSendSuccessPending = true;
          networkSendSuccessWasPreset = sendWasPreset;
        }
        networkSendInFlight = false;
        networkSendInFlightIsPreset = false;
        xSemaphoreGive(networkMutex);
      }
    } else if (doFetch) {
      String payload;
      int code = httpRequest(host, port, "GET", "/json/state", "", &payload);
      bool parsed = false;
      if (code == 200) {
        parsed = parseStatePayload(payload, state);
      }
      if (networkMutex && xSemaphoreTake(networkMutex, portMAX_DELAY) == pdTRUE) {
        lastHttpCode = code;
        if (parsed) {
          networkFetchState = state;
          networkFetchInteractionSeq = fetchInteraction;
          networkFetchControllerSeq = fetchController;
          networkFetchRedraw = redrawAfterFetch;
          networkFetchResultPending = true;
        }
        networkFetchInFlight = false;
        xSemaphoreGive(networkMutex);
      }
    } else if (doCatalog) {
      int fetchedEffectCount = 0;
      int fetchedPaletteCount = 0;
      int fetchedPresetCount = 0;
      bool effectsOk = fetchEffectsFor(host, port, networkEffectNames, fetchedEffectCount);
      bool palettesOk = fetchPalettesFor(host, port, networkPaletteNames, fetchedPaletteCount);
      bool presetsOk = fetchPresetsFor(host, port, networkPresetEntries, fetchedPresetCount);
      if (networkMutex && xSemaphoreTake(networkMutex, portMAX_DELAY) == pdTRUE) {
        networkEffectCount = fetchedEffectCount;
        networkPaletteCount = fetchedPaletteCount;
        networkPresetCount = fetchedPresetCount;
        networkCatalogEffectsReady = effectsOk;
        networkCatalogPalettesReady = palettesOk;
        networkCatalogPresetsReady = presetsOk;
        networkCatalogControllerSeq = catalogController;
        networkCatalogRedraw = redrawAfterCatalog;
        networkCatalogResultPending = effectsOk || palettesOk || presetsOk;
        networkCatalogInFlight = false;
        xSemaphoreGive(networkMutex);
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

void updateCachedLabels() {
  if (effectCount > 0 && wled_fxIndex >= 0 && wled_fxIndex < effectCount) {
    cachedEffectLabel = effectNames[wled_fxIndex];
  }
  if (paletteCount > 0 && wled_palIndex >= 0 && wled_palIndex < paletteCount) {
    cachedPaletteLabel = paletteNames[wled_palIndex];
  }
  int presetIndex = findPresetIndexById(wled_presetId);
  if (presetIndex >= 0) {
    cachedPresetLabel = presetEntries[presetIndex].name;
  }
}

int findPresetIndexById(int presetId) {
  if (presetId <= 0) return -1;
  for (int i = 0; i < presetCount; ++i) {
    if (presetEntries[i].id == presetId) return i;
  }
  return -1;
}

void syncPresetCursorToCurrentPreset() {
  int idx = findPresetIndexById(wled_presetId);
  if (idx >= 0) presetCursor = idx;
}

void saveControllerCache() {
  if (wledHost.length() == 0) return;
  updateCachedLabels();
  preferences.putString("cache_host", wledHost);
  preferences.putInt("cache_port", wledPort);
  preferences.putBool("cache_on", wled_on);
  preferences.putUChar("cache_bri", wled_bri);
  preferences.putInt("cache_hue", wled_hue);
  preferences.putInt("cache_fx", wled_fxIndex);
  preferences.putUChar("cache_sx", wled_fxSpeed);
  preferences.putUChar("cache_ix", wled_fxIntensity);
  preferences.putInt("cache_pal", wled_palIndex);
  preferences.putInt("cache_ps", wled_presetId);
  preferences.putString("cache_eff", cachedEffectLabel);
  preferences.putString("cache_pn", cachedPaletteLabel);
  preferences.putString("cache_pr", cachedPresetLabel);
}

bool loadControllerCache(const String& host, int port) {
  if (host.length() == 0) return false;
  String cachedHost = preferences.getString("cache_host", "");
  int cachedPort = preferences.getInt("cache_port", -1);
  if (cachedHost != host || cachedPort != port) return false;

  wled_on = preferences.getBool("cache_on", wled_on);
  wled_bri = preferences.getUChar("cache_bri", wled_bri);
  wled_hue = preferences.getInt("cache_hue", wled_hue);
  wled_fxIndex = preferences.getInt("cache_fx", wled_fxIndex);
  wled_fxSpeed = preferences.getUChar("cache_sx", wled_fxSpeed);
  wled_fxIntensity = preferences.getUChar("cache_ix", wled_fxIntensity);
  wled_palIndex = preferences.getInt("cache_pal", wled_palIndex);
  wled_presetId = preferences.getInt("cache_ps", wled_presetId);
  cachedEffectLabel = preferences.getString("cache_eff", cachedEffectLabel);
  cachedPaletteLabel = preferences.getString("cache_pn", cachedPaletteLabel);
  cachedPresetLabel = preferences.getString("cache_pr", cachedPresetLabel);
  Serial.printf("Loaded cached controller state for %s:%d\n", host.c_str(), port);
  return true;
}

bool readEspStoredWifiCredentials(String& ssid, String& pass) {
  ssid = "";
  pass = "";

  wifi_config_t conf = {};
  esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &conf);
  if (err != ESP_OK) {
    Serial.printf("ESP WiFi config read failed: %d\n", (int)err);
    return false;
  }

  ssid = String(reinterpret_cast<const char*>(conf.sta.ssid));
  pass = String(reinterpret_cast<const char*>(conf.sta.password));
  return ssid.length() > 0;
}

void refreshActiveController(bool redrawAfter) {
  if (!wifiConnected || wledHost.length() == 0) return;
  fetchEffects();
  fetchPalettes();
  fetchPresets();
  fetchState();
  saveControllerCache();
  lastPollMs = millis();
  if (redrawAfter) drawDisplay();
}

void applyLocalSettings() {
  M5Dial.Display.setBrightness(screenBrightness);
  M5Dial.Speaker.setVolume(clickVolume);
}

void playClick(int freq, int duration) {
  if (clickVolume == 0) return;
  M5Dial.Speaker.tone(freq, duration);
}

void resetEncoderState() {
  encoderPulseRemainder = 0;
  M5Dial.Encoder.readAndReset();
}

bool connectSavedWifi(unsigned long timeoutMs) {
  String storedSsid;
  String storedPass;
  loadStoredWifiCredentials(storedSsid, storedPass);
  if (storedSsid.length() > 0) {
    Serial.printf("WiFi: found stored Preferences credentials for '%s'\n", storedSsid.c_str());
    if (connectWifiCredentials(storedSsid, storedPass, timeoutMs)) {
      return true;
    }
    Serial.println("WiFi: Preferences credentials failed, trying ESP saved credentials");
  } else {
    Serial.println("WiFi: no stored Preferences credentials found");
  }

  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);
  WiFi.setAutoReconnect(true);
  WiFi.disconnect(false, false);
  delay(250);
  Serial.println("WiFi: trying saved credentials");
  WiFi.begin();

  return waitForWifiConnection(timeoutMs, "saved");
}

bool connectWifiCredentials(const String& ssid, const String& pass, unsigned long timeoutMs) {
  if (ssid.length() == 0) {
    Serial.println("WiFi: no SSID provided for explicit connect");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);
  WiFi.setAutoReconnect(true);
  WiFi.disconnect(false, false);
  delay(250);
  Serial.printf("WiFi: trying explicit credentials for '%s'\n", ssid.c_str());
  if (pass.length() > 0) WiFi.begin(ssid.c_str(), pass.c_str());
  else WiFi.begin(ssid.c_str());

  return waitForWifiConnection(timeoutMs, "explicit");
}

bool waitForWifiConnection(unsigned long timeoutMs, const char* label) {
  wl_status_t lastStatus = (wl_status_t)(-1);
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    wl_status_t status = WiFi.status();
    if (status != lastStatus) {
      Serial.printf("WiFi %s status: %s\n", label, wifiStatusName(status));
      lastStatus = status;
    }
    if (status == WL_CONNECTED) {
      Serial.printf("WiFi %s connected to '%s' with IP %s\n",
                    label,
                    WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
      return true;
    }
    delay(250);
  }

  wl_status_t finalStatus = WiFi.status();
  Serial.printf("WiFi %s failed after %lu ms: %s\n", label, timeoutMs, wifiStatusName(finalStatus));
  return finalStatus == WL_CONNECTED;
}

const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "idle";
    case WL_NO_SSID_AVAIL: return "no_ssid";
    case WL_SCAN_COMPLETED: return "scan_completed";
    case WL_CONNECTED: return "connected";
    case WL_CONNECT_FAILED: return "connect_failed";
    case WL_CONNECTION_LOST: return "connection_lost";
    case WL_DISCONNECTED: return "disconnected";
    default: return "unknown";
  }
}

int readEncoderSteps() {
  int raw = M5Dial.Encoder.readAndReset();
  if (raw == 0) return 0;
  encoderPulseRemainder += raw;
  int steps = encoderPulseRemainder / ENCODER_COUNTS_PER_STEP;
  encoderPulseRemainder -= steps * ENCODER_COUNTS_PER_STEP;
  return steps;
}

int byteToLevel(uint8_t value) {
  if (value <= 1) return UI_LEVEL_MIN;
  return constrain((int)lroundf((value - 1) * (float)(UI_LEVEL_MAX - UI_LEVEL_MIN) / 254.0f) + UI_LEVEL_MIN,
                   UI_LEVEL_MIN, UI_LEVEL_MAX);
}

uint8_t levelToByte(int level) {
  level = constrain(level, UI_LEVEL_MIN, UI_LEVEL_MAX);
  return (uint8_t)(1 + (level - UI_LEVEL_MIN) * 254 / (UI_LEVEL_MAX - UI_LEVEL_MIN));
}

int brightnessToUi(uint8_t value) {
  return byteToLevel(value);
}

void openMainMenu() {
  controlScreen = SCREEN_MAIN_MENU;
  settingsEditing = false;
  arcTouchActive = false;
  listDragAccumY = 0;
  resetEncoderState();
}

void openScreen(ControlScreen screen) {
  controlScreen = screen;
  settingsEditing = false;
  arcTouchActive = false;
  listDragAccumY = 0;
  resetEncoderState();
  if (screen == SCREEN_PALETTE_HUE || screen == SCREEN_PRESET || screen == SCREEN_FX_PATTERN) {
    requestBackgroundCatalogFetch(true);
  }
}

void setup() {
  Serial.begin(115200);
  auto cfg = M5.config();
  M5Dial.begin(cfg, true, false);
  M5Dial.Display.setRotation(0);
  startNetworkWorker();
  loadPreferences();
  applyLocalSettings();
  M5Dial.Display.fillScreen(CLR_BLACK);
  M5Dial.Display.setTextColor(CLR_WHITE, CLR_BLACK);
  M5Dial.Display.drawCenterString("WLED Remote", 120, 104, &fonts::FreeSansBold9pt7b);
  M5Dial.Display.drawCenterString("Connecting...", 120, 128, &fonts::FreeSans9pt7b);
  wifiConnect();
  if (wifiConnected) {
    appState = STATE_SCANNING;
    drawDisplay();
    discoverWLED();
    if (controllerCount > 0) {
      appState = STATE_SELECT;
    } else {
      wledHost = WLED_DEFAULT_IP;
      wledPort = WLED_DEFAULT_PORT;
      noteControllerChanged();
      appState = STATE_CONTROL;
      openMainMenu();
      loadControllerCache(wledHost, wledPort);
      drawDisplay();
      refreshActiveController(false);
    }
  } else {
    appState = STATE_CONTROL;
    openMainMenu();
  }
  resetEncoderState();
  drawDisplay();
}

void startControllerSelection(bool rescan) {
  if (rescan && wifiConnected) {
    appState = STATE_SCANNING;
    drawDisplay();
    discoverWLED();
  }
  if (controllerCount == 0 && wledHost.length() > 0) {
    controllers[0].ip = wledHost;
    controllers[0].port = wledPort;
    controllers[0].name = "WLED";
    controllerCount = 1;
  }
  selectionCursor = 0;
  for (int i = 0; i < controllerCount; ++i) {
    if (controllers[i].ip == wledHost) {
      selectionCursor = i;
      break;
    }
  }
  appState = STATE_SELECT;
  resetEncoderState();
  drawDisplay();
}

void selectCurrentController() {
  if (controllerCount <= 0) return;
  wledHost = controllers[selectionCursor].ip;
  wledPort = controllers[selectionCursor].port;
  noteControllerChanged();
  clearControllerCatalogs();
  Serial.printf("Selected: %s (%s:%d)\n", controllers[selectionCursor].name.c_str(), wledHost.c_str(), wledPort);
  appState = STATE_CONTROL;
  openMainMenu();
  loadControllerCache(wledHost, wledPort);
  drawDisplay();
  if (requestBackgroundStateFetch(true)) lastPollMs = millis();
  requestBackgroundCatalogFetch(true);
}

void handleShortPress() {
  if (appState == STATE_SELECT) {
    selectCurrentController();
    return;
  }
  if (appState != STATE_CONTROL) return;

  switch (controlScreen) {
    case SCREEN_MAIN_MENU:
      if (mainMenuIndex == 0) startControllerSelection(true);
      else openScreen(MAIN_MENU_ITEMS[mainMenuIndex].screen);
      noteInteraction();
      drawDisplay();
      break;
    case SCREEN_SETTINGS:
      if (settingsCursor == SETTING_WIFI) {
        beginWifiPortal();
      } else {
        settingsEditing = !settingsEditing;
        playClick(settingsEditing ? 5400 : 3600, 18);
        noteInteraction();
        drawDisplay();
      }
      break;
    default:
      openMainMenu();
      drawDisplay();
      break;
  }
}

void handleLongPress() {
  if (appState == STATE_SELECT) {
    appState = STATE_CONTROL;
    openMainMenu();
    drawDisplay();
    return;
  }
  if (appState != STATE_CONTROL) return;

  if (controlScreen == SCREEN_MAIN_MENU) {
    wled_on = !wled_on;
    queueStateSend();
    playClick(wled_on ? 5200 : 2800, 32);
    scheduleControllerCacheSave();
    noteInteraction();
    drawDisplay();
    return;
  }

  settingsEditing = false;
  openMainMenu();
  noteInteraction();
  drawDisplay();
}

void applyEncoderSteps(int steps) {
  noteInteraction();
  if (appState == STATE_SELECT) {
    if (controllerCount > 0) {
      selectionCursor = ((selectionCursor + steps) % controllerCount + controllerCount) % controllerCount;
      playClick(6000, 15);
      drawDisplay();
    }
    return;
  }
  if (appState != STATE_CONTROL) return;

  switch (controlScreen) {
    case SCREEN_MAIN_MENU:
      mainMenuIndex = ((mainMenuIndex + steps) % MAIN_MENU_COUNT + MAIN_MENU_COUNT) % MAIN_MENU_COUNT;
      playClick(5200, 15);
      break;
    case SCREEN_BRIGHTNESS: {
      int level = brightnessToUi(wled_bri);
      level = constrain(level + steps, UI_LEVEL_MIN, UI_LEVEL_MAX);
      wled_bri = levelToByte(level);
      queueStateSend();
      playClick(7600, 16);
      break;
    }
    case SCREEN_PALETTE_HUE:
      if (paletteCount > 0) {
        wled_palIndex = ((wled_palIndex + steps) % paletteCount + paletteCount) % paletteCount;
        queueStateSend();
      }
      playClick(7000, 16);
      break;
    case SCREEN_FX_PATTERN:
      if (effectCount > 0) {
        wled_fxIndex = ((wled_fxIndex + steps) % effectCount + effectCount) % effectCount;
        queueStateSend();
        playClick(7000, 16);
      }
      break;
    case SCREEN_PRESET:
      if (presetCount > 0) {
        presetCursor = ((presetCursor + steps) % presetCount + presetCount) % presetCount;
        wled_presetId = presetEntries[presetCursor].id;
        queuePresetApply(wled_presetId);
        updateCachedLabels();
        scheduleControllerCacheSave();
        playClick(7000, 16);
      }
      break;
    case SCREEN_FX_SPEED: {
      int level = byteToLevel(wled_fxSpeed);
      level = constrain(level + steps, UI_LEVEL_MIN, UI_LEVEL_MAX);
      wled_fxSpeed = levelToByte(level);
      queueStateSend();
      playClick(7600, 16);
      break;
    }
    case SCREEN_FX_INTENSITY: {
      int level = byteToLevel(wled_fxIntensity);
      level = constrain(level + steps, UI_LEVEL_MIN, UI_LEVEL_MAX);
      wled_fxIntensity = levelToByte(level);
      queueStateSend();
      playClick(7600, 16);
      break;
    }
    case SCREEN_SETTINGS:
      if (settingsEditing) {
        if (settingsCursor == SETTING_SCREEN_BRIGHTNESS) {
          int level = byteToLevel(screenBrightness);
          level = constrain(level + steps, UI_LEVEL_MIN, UI_LEVEL_MAX);
          screenBrightness = levelToByte(level);
          applyLocalSettings();
          savePreferences();
        } else if (settingsCursor == SETTING_CLICK_VOLUME) {
          int level = constrain((int)lroundf(clickVolume * 11.0f / 255.0f), 0, 11);
          level = constrain(level + steps, 0, 11);
          clickVolume = (uint8_t)(level * 255 / 11);
          applyLocalSettings();
          savePreferences();
        }
        playClick(6800, 14);
      } else {
        settingsCursor = (SettingsItem)(((int)settingsCursor + steps + SETTING_COUNT) % SETTING_COUNT);
        playClick(5200, 15);
      }
      break;
  }

  drawDisplay();
}

void loop() {
  M5Dial.update();
  int encoderSteps = readEncoderSteps();
  if (encoderSteps != 0) {
    applyEncoderSteps(encoderSteps);
  }
  handleTouch();

  if (M5Dial.BtnA.wasPressed()) {
    btnPressedAt = millis();
    btnHeld = true;
    longFired = false;
  }
  if (btnHeld && !longFired && (millis() - btnPressedAt >= LONG_PRESS_MS)) {
    longFired = true;
    handleLongPress();
  }
  if (M5Dial.BtnA.wasReleased()) {
    if (btnHeld && !longFired) handleShortPress();
    btnHeld = false;
  }

  processNetworkResults();

  if (pendingSend && wifiConnected && (millis() - lastSendMs >= SEND_DEBOUNCE_MS)) {
    if (requestBackgroundStateSend()) {
      pendingSend = false;
      pendingPresetApply = false;
      pendingPresetId = -1;
      lastSendMs = millis();
    }
  }
  if (wifiConnected &&
      appState == STATE_CONTROL &&
      !pendingSend &&
      (millis() - lastInteractionMs >= INPUT_GRACE_MS) &&
      (millis() - lastPollMs >= POLL_INTERVAL_MS)) {
    if (requestBackgroundStateFetch(true)) {
      lastPollMs = millis();
    }
  }

  processNetworkResults();
  processDeferredCacheSave();
}

bool touchToArcFraction(int x, int y, float* fraction) {
  float dx = x - ARC_CX;
  float dy = y - ARC_CY;
  float dist = sqrtf(dx * dx + dy * dy);
  if (dist < ARC_RAD - ARC_THICK - 10 || dist > ARC_RAD + ARC_THICK + 10) return false;
  return touchAngleToFraction(x, y, fraction);
}

bool touchAngleToFraction(int x, int y, float* fraction) {
  float dx = x - ARC_CX;
  float dy = y - ARC_CY;
  float deg = atan2f(dy, dx) * 180.0f / PI;
  if (deg < 0) deg += 360.0f;
  float along = deg - ARC_START_DEG;
  if (along < 0) along += 360.0f;
  if (along > ARC_SWEEP_DEG) return false;
  *fraction = along / ARC_SWEEP_DEG;
  return true;
}

bool handleListDrag(const m5::touch_detail_t& t) {
  bool isListScreen = (appState == STATE_SELECT) ||
                      (appState == STATE_CONTROL &&
                       (controlScreen == SCREEN_FX_PATTERN || controlScreen == SCREEN_PRESET));
  if (!isListScreen) return false;
  int count = controllerCount;
  if (appState == STATE_CONTROL && controlScreen == SCREEN_FX_PATTERN) count = effectCount;
  else if (appState == STATE_CONTROL && controlScreen == SCREEN_PRESET) count = presetCount;
  if (count <= 0) return false;
  if (t.wasPressed()) listDragAccumY = 0;
  if (!t.isPressed()) return false;
  listDragAccumY += t.deltaY();
  int steps = listDragAccumY / LIST_DRAG_STEP_PX;
  if (steps == 0) return true;
  listDragAccumY -= steps * LIST_DRAG_STEP_PX;
  applyEncoderSteps(-steps);
  return true;
}

bool pointInCircle(int x, int y, int cx, int cy, int r) {
  int dx = x - cx;
  int dy = y - cy;
  return dx * dx + dy * dy <= r * r;
}

void handleTouch() {
  auto t = M5Dial.Touch.getDetail();
  if (!t.isPressed() && !t.wasClicked() && !t.wasReleased()) {
    arcTouchActive = false;
    listDragAccumY = 0;
    return;
  }

  bool fresh = t.wasPressed();
  if (fresh) {
    noteInteraction();
    arcTouchActive = false;
    listDragAccumY = 0;
  }

  if (handleListDrag(t)) return;

  if (appState == STATE_CONTROL && controlScreen == SCREEN_MAIN_MENU && fresh) {
    for (int i = 0; i < MAIN_MENU_COUNT; ++i) {
      float rad = MENU_ANGLES_DEG[i] * DEG_TO_RAD;
      int cx = MENU_CX + (int)roundf(cosf(rad) * MENU_RING_R);
      int cy = MENU_CY + (int)roundf(sinf(rad) * MENU_RING_R);
      if (pointInCircle(t.x, t.y, cx, cy, MENU_ITEM_R_SEL + 6)) {
        if (mainMenuIndex == i) handleShortPress();
        else {
          mainMenuIndex = i;
          playClick(5200, 15);
          noteInteraction();
          drawDisplay();
        }
        return;
      }
    }
  }

  if (appState == STATE_CONTROL &&
      (controlScreen == SCREEN_BRIGHTNESS || controlScreen == SCREEN_FX_SPEED ||
       controlScreen == SCREEN_FX_INTENSITY)) {
    if (millis() - lastTouchSetMs < SEND_DEBOUNCE_MS) return;
    float fraction;
    if (fresh && touchToArcFraction(t.x, t.y, &fraction)) {
      arcTouchActive = true;
    }
    if (arcTouchActive && t.isPressed() && touchAngleToFraction(t.x, t.y, &fraction)) {
      noteInteraction();
      lastTouchSetMs = millis();
      if (controlScreen == SCREEN_BRIGHTNESS) {
        wled_bri = levelToByte(UI_LEVEL_MIN + (int)roundf(fraction * (UI_LEVEL_MAX - UI_LEVEL_MIN)));
      } else if (controlScreen == SCREEN_FX_SPEED) {
        wled_fxSpeed = levelToByte(UI_LEVEL_MIN + (int)roundf(fraction * (UI_LEVEL_MAX - UI_LEVEL_MIN)));
      } else if (controlScreen == SCREEN_FX_INTENSITY) {
        wled_fxIntensity = levelToByte(UI_LEVEL_MIN + (int)roundf(fraction * (UI_LEVEL_MAX - UI_LEVEL_MIN)));
      }
      queueStateSend();
      scheduleControllerCacheSave();
      drawDisplay();
    }
    if (t.wasReleased()) arcTouchActive = false;
  }

  if (appState == STATE_CONTROL && controlScreen == SCREEN_SETTINGS && fresh) {
    int row = (t.y - 58) / 42;
    if (row >= 0 && row < SETTING_COUNT) {
      settingsCursor = (SettingsItem)row;
      if (settingsCursor == SETTING_WIFI) beginWifiPortal();
      else {
        settingsEditing = !settingsEditing;
        playClick(settingsEditing ? 5400 : 3600, 18);
        noteInteraction();
        drawDisplay();
      }
    }
  }
}

void wifiConnect() {
  String storedSsid;
  String storedPass;
  loadStoredWifiCredentials(storedSsid, storedPass);
  if (storedSsid.length() == 0) {
    WiFi.mode(WIFI_STA);
    String espSsid;
    String espPass;
    if (readEspStoredWifiCredentials(espSsid, espPass)) {
      Serial.printf("Imported ESP WiFi config for '%s' into Preferences\n", espSsid.c_str());
      saveStoredWifiCredentials(espSsid, espPass);
      storedSsid = espSsid;
      storedPass = espPass;
    }
  }
  if (storedSsid.length() > 0) {
    Serial.printf("Saved WiFi SSID in Preferences: '%s' (passlen=%u)\n",
                  storedSsid.c_str(),
                  (unsigned)storedPass.length());
  } else {
    Serial.println("Saved WiFi SSID in Preferences: <none>");
  }
  Serial.println("Connecting with saved WiFi credentials...");
  wifiConnected = connectSavedWifi(30000);
  if (wifiConnected) {
    Serial.printf("WiFi OK - %s\n", WiFi.localIP().toString().c_str());
    return;
  }

  Serial.println("Saved WiFi failed. Opening config portal...");
  runWifiPortal(false);
}

void beginWifiPortal() {
  runWifiPortal(true);
}

void onWifiConfigSaved() {
  wifiPortalDidSave = true;
}

bool runWifiPortal(bool returnToSettings) {
  WiFiManager wm;
  wifiPortalDidSave = false;
  WiFi.mode(WIFI_STA);
  wm.setDebugOutput(false);
  wm.setConfigPortalTimeout(180);
  wm.setConnectTimeout(30);
  wm.setSaveConnectTimeout(30);
  wm.setRestorePersistent(true);
  wm.setWiFiAutoReconnect(true);
  wm.setSaveConnect(true);
  wm.setCleanConnect(true);
  wm.setBreakAfterConfig(true);
  wm.setAPCallback(onWifiPortal);
  wm.setSaveConfigCallback(onWifiConfigSaved);
  drawWifiPortal();
  bool portalResult = wm.startConfigPortal("M5Dial-WLED");

  String savedSsid;
  String savedPass;
  if (!readEspStoredWifiCredentials(savedSsid, savedPass)) {
    savedSsid = wm.getWiFiSSID(true);
    savedPass = wm.getWiFiPass(true);
  }
  if (savedSsid.length() == 0) savedSsid = WiFi.SSID();
  if (savedSsid.length() > 0) {
    saveStoredWifiCredentials(savedSsid, savedPass);
  }
  Serial.printf("WiFi portal persistent SSID='%s' passlen=%u\n",
                savedSsid.c_str(),
                (unsigned)savedPass.length());
  wifiConnected = (WiFi.status() == WL_CONNECTED);

  bool shouldRetrySavedWifi = wifiPortalDidSave || savedSsid.length() > 0 || portalResult;
  if (!wifiConnected && shouldRetrySavedWifi) {
    wifiConnected = connectWifiCredentials(savedSsid, savedPass, 20000);
  }
  if (!wifiConnected && shouldRetrySavedWifi) {
    wifiConnected = connectSavedWifi(20000);
  }

  Serial.printf("WiFi portal result=%d saved=%d connected=%d ssid='%s' status=%s\n",
                portalResult ? 1 : 0,
                wifiPortalDidSave ? 1 : 0,
                wifiConnected ? 1 : 0,
                savedSsid.c_str(),
                wifiStatusName(WiFi.status()));

  if (wifiConnected) {
    discoverWLED();
    if (controllerCount > 0) {
      wledHost = controllers[0].ip;
      wledPort = controllers[0].port;
      noteControllerChanged();
    }
    loadControllerCache(wledHost, wledPort);
    refreshActiveController(false);
  }
  if (returnToSettings) {
    appState = STATE_CONTROL;
    settingsEditing = false;
    openScreen(SCREEN_SETTINGS);
    drawDisplay();
  }
  return wifiConnected;
}

void onWifiPortal(WiFiManager* wm) {
  (void)wm;
  drawWifiPortal();
}

void drawWifiPortal() {
  M5Dial.Display.fillScreen(CLR_BLACK);
  M5Dial.Display.setTextColor(CLR_CYAN, CLR_BLACK);
  M5Dial.Display.drawCenterString("SETUP WIFI", 120, 66, &fonts::FreeSansBold9pt7b);
  M5Dial.Display.setTextColor(CLR_WHITE, CLR_BLACK);
  M5Dial.Display.drawCenterString("M5Dial-WLED", 120, 102, &fonts::FreeSans9pt7b);
  M5Dial.Display.setTextColor(CLR_LGRAY, CLR_BLACK);
  M5Dial.Display.drawCenterString("Open 192.168.4.1", 120, 132, &fonts::FreeSans9pt7b);
  M5Dial.Display.drawCenterString("Save to continue", 120, 158, &fonts::Font0);
}

String parseWLEDNameFromInfo(const String& payload, const String& fallback) {
  StaticJsonDocument<64> filter;
  filter["name"] = true;
  StaticJsonDocument<128> doc;
  DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (!err) {
    const char* n = doc["name"];
    if (n && strlen(n) > 0) return String(n);
  }
  return fallback;
}

static String fetchWLEDName(const String& ip, int port, const String& fallback) {
  String payload;
  if (httpRequest(ip, port, "GET", "/json/info", "", &payload) == 200) {
    return parseWLEDNameFromInfo(payload, fallback);
  }
  return fallback;
}

void discoverWLED() {
  controllerCount = 0;
  MDNS.end();
  MDNS.begin("m5dial-remote");
  int n = MDNS.queryService("wled", "tcp");
  Serial.printf("mDNS: %d WLED service(s) found\n", n);
  for (int i = 0; i < n && controllerCount < MAX_CONTROLLERS; ++i) {
    String ip = MDNS.address(i).toString();
    int port = MDNS.port(i) > 0 ? MDNS.port(i) : 80;
    String name = fetchWLEDName(ip, port, MDNS.hostname(i));
    controllers[controllerCount].ip = ip;
    controllers[controllerCount].name = name;
    controllers[controllerCount].port = port;
    Serial.printf("  [%d] %s @ %s:%d\n", controllerCount, name.c_str(), ip.c_str(), port);
    controllerCount++;
  }
  if (controllerCount == 0) {
    Serial.printf("mDNS: none - probing %s...\n", WLED_DEFAULT_IP);
    String payload;
    if (httpRequest(WLED_DEFAULT_IP, WLED_DEFAULT_PORT, "GET", "/json/info", "", &payload) == 200) {
      String name = parseWLEDNameFromInfo(payload, "WLED");
      controllers[0].ip = WLED_DEFAULT_IP;
      controllers[0].name = name;
      controllers[0].port = WLED_DEFAULT_PORT;
      controllerCount = 1;
    }
  }
}

String buildJson() {
  WLEDStateSnapshot state;
  captureCurrentState(state);
  return buildJsonFromSnapshot(state);
}

String buildJsonFromSnapshot(const WLEDStateSnapshot& state) {
  StaticJsonDocument<512> doc;
  doc["on"] = state.on;
  doc["bri"] = state.bri;
  JsonArray seg = doc.createNestedArray("seg");
  JsonObject s = seg.createNestedObject();
  s["fx"] = state.fxIndex;
  s["sx"] = state.fxSpeed;
  s["ix"] = state.fxIntensity;
  s["pal"] = state.palIndex;

  uint8_t r, g, b;
  int hue = ((state.hue % 360) + 360) % 360;
  int sector = hue / 60;
  int frac = (hue % 60) * 255 / 60;
  switch (sector) {
    case 0: r = 255; g = frac; b = 0; break;
    case 1: r = 255 - frac; g = 255; b = 0; break;
    case 2: r = 0; g = 255; b = frac; break;
    case 3: r = 0; g = 255 - frac; b = 255; break;
    case 4: r = frac; g = 0; b = 255; break;
    default: r = 255; g = 0; b = 255 - frac; break;
  }
  JsonArray col = s.createNestedArray("col");
  JsonArray rgb = col.createNestedArray();
  rgb.add(r);
  rgb.add(g);
  rgb.add(b);

  String out;
  serializeJson(doc, out);
  return out;
}

String buildPresetJson(int presetId) {
  if (presetId <= 0) return String();
  StaticJsonDocument<64> doc;
  doc["ps"] = presetId;
  String out;
  serializeJson(doc, out);
  return out;
}

int httpRequest(const String& host, int port, const char* method, const char* path, const String& body, String* payload) {
  WiFiClient client;
  client.setTimeout(3000);
  if (!client.connect(host.c_str(), port, 3000)) return -1;
  client.print(method);
  client.print(' ');
  client.print(path);
  client.println(F(" HTTP/1.1"));
  client.print(F("Host: "));
  client.println(host);
  client.println(F("Connection: close"));
  if (body.length() > 0) {
    client.println(F("Content-Type: application/json"));
    client.print(F("Content-Length: "));
    client.println(body.length());
  }
  client.println();
  if (body.length() > 0) client.print(body);
  String status = client.readStringUntil('\n');
  int firstSpace = status.indexOf(' ');
  int code = (firstSpace >= 0) ? status.substring(firstSpace + 1).toInt() : -1;
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) break;
  }
  if (payload) *payload = client.readString();
  client.stop();
  return code;
}

void sendState() {
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    return;
  }
  String body = buildJson();
  Serial.printf("POST -> %s\n", body.c_str());
  lastHttpCode = httpRequest(wledHost, wledPort, "POST", "/json/state", body, nullptr);
  if (lastHttpCode == 200) saveControllerCache();
  Serial.printf("HTTP %d\n", lastHttpCode);
}

void fetchState() {
  if (WiFi.status() != WL_CONNECTED) return;
  String payload;
  if (httpRequest(wledHost, wledPort, "GET", "/json/state", "", &payload) == 200) {
    WLEDStateSnapshot state;
    captureCurrentState(state);
    if (parseStatePayload(payload, state)) {
      applyStateSnapshot(state);
      lastHttpCode = 200;
      saveControllerCache();
    }
  }
}

bool fetchEffectsFor(const String& host, int port, String* names, int& count) {
  if (WiFi.status() != WL_CONNECTED) return false;
  String payload;
  if (httpRequest(host, port, "GET", "/json/eff", "", &payload) == 200) {
    DynamicJsonDocument doc(8192);
    if (!deserializeJson(doc, payload)) {
      int loadedCount = 0;
      for (JsonVariant v : doc.as<JsonArray>()) {
        if (loadedCount >= MAX_EFFECTS) break;
        names[loadedCount++] = v.as<String>();
      }
      count = loadedCount;
      return true;
    }
  }
  return false;
}

bool fetchPalettesFor(const String& host, int port, String* names, int& count) {
  if (WiFi.status() != WL_CONNECTED) return false;
  String payload;
  if (httpRequest(host, port, "GET", "/json/pal", "", &payload) == 200) {
    DynamicJsonDocument doc(4096);
    if (!deserializeJson(doc, payload)) {
      int loadedCount = 0;
      for (JsonVariant v : doc.as<JsonArray>()) {
        if (loadedCount >= MAX_PALETTES) break;
        names[loadedCount++] = v.as<String>();
      }
      count = loadedCount;
      return true;
    }
  }
  return false;
}

bool fetchPresetsFor(const String& host, int port, PresetEntry* presets, int& count) {
  if (WiFi.status() != WL_CONNECTED) return false;
  String payload;
  if (httpRequest(host, port, "GET", "/presets.json", "", &payload) == 200) {
    DynamicJsonDocument doc(32768);
    DeserializationError err = deserializeJson(doc, payload);
    if (!err && doc.is<JsonObject>()) {
      int loadedCount = 0;
      for (JsonPair kv : doc.as<JsonObject>()) {
        if (loadedCount >= MAX_PRESETS) break;
        int id = atoi(kv.key().c_str());
        if (id <= 0) continue;
        JsonObject preset = kv.value().as<JsonObject>();
        const char* name = preset["n"];
        presets[loadedCount].id = id;
        if (name && strlen(name) > 0) presets[loadedCount].name = String(name);
        else presets[loadedCount].name = String("Preset ") + id;
        loadedCount++;
      }
      for (int i = 0; i < loadedCount - 1; ++i) {
        for (int j = i + 1; j < loadedCount; ++j) {
          if (presets[j].id < presets[i].id) {
            PresetEntry tmp = presets[i];
            presets[i] = presets[j];
            presets[j] = tmp;
          }
        }
      }
      count = loadedCount;
      return true;
    }
  }
  return false;
}

void fetchEffects() {
  if (fetchEffectsFor(wledHost, wledPort, effectNames, effectCount)) {
    updateCachedLabels();
    saveControllerCache();
  }
}

void fetchPalettes() {
  if (fetchPalettesFor(wledHost, wledPort, paletteNames, paletteCount)) {
    updateCachedLabels();
    saveControllerCache();
  }
}

void fetchPresets() {
  if (fetchPresetsFor(wledHost, wledPort, presetEntries, presetCount)) {
    syncPresetCursorToCurrentPreset();
    updateCachedLabels();
    saveControllerCache();
  }
}

uint16_t hue16(int h) {
  uint8_t r, g, b;
  int sector = h / 60;
  int frac = (h % 60) * 255 / 60;
  switch (sector) {
    case 0: r = 255; g = frac; b = 0; break;
    case 1: r = 255 - frac; g = 255; b = 0; break;
    case 2: r = 0; g = 255; b = frac; break;
    case 3: r = 0; g = 255 - frac; b = 255; break;
    case 4: r = frac; g = 0; b = 255; break;
    default: r = 255; g = 0; b = 255 - frac; break;
  }
  return M5Dial.Display.color565(r, g, b);
}

void drawArc(float fraction, uint16_t color) {
  int r0 = ARC_RAD - ARC_THICK / 2;
  int r1 = ARC_RAD + ARC_THICK / 2;
  float arcStart = ARC_START_DEG + 90.0f;
  float arcEnd = ARC_START_DEG + ARC_SWEEP_DEG + 90.0f;
  float split = arcStart + ARC_SWEEP_DEG * fraction;
  if (fraction < 1.0f) M5Dial.Display.fillArc(ARC_CX, ARC_CY, r0, r1, split, arcEnd, CLR_DGRAY);
  if (fraction > 0.0f) M5Dial.Display.fillArc(ARC_CX, ARC_CY, r0, r1, arcStart, split, color);
}

String currentControllerLabel() {
  for (int i = 0; i < controllerCount; ++i) {
    if (controllers[i].ip == wledHost) return controllers[i].name;
  }
  if (wledHost.length() > 0) return wledHost;
  return "No Light";
}

String currentEffectLabel() {
  if (effectCount > 0 && wled_fxIndex >= 0 && wled_fxIndex < effectCount) return effectNames[wled_fxIndex];
  if (cachedEffectLabel.length() > 0) return cachedEffectLabel;
  return "Effect";
}

String currentPaletteLabel() {
  if (paletteCount > 0 && wled_palIndex >= 0 && wled_palIndex < paletteCount) return paletteNames[wled_palIndex];
  if (cachedPaletteLabel.length() > 0) return cachedPaletteLabel;
  return "Palette";
}

String currentPresetLabel() {
  int idx = findPresetIndexById(wled_presetId);
  if (idx >= 0) return presetEntries[idx].name;
  if (cachedPresetLabel.length() > 0) return cachedPresetLabel;
  return "Preset";
}

String wifiStatusText() {
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) return "Not connected";
  String ssid = WiFi.SSID();
  if (ssid.length() == 0) ssid = "Connected";
  return ssid;
}

String currentScreenSummary() {
  switch (mainMenuIndex) {
    case 0: return currentControllerLabel();
    case 1: return String("Level ") + brightnessToUi(wled_bri);
    case 2: return currentPaletteLabel();
    case 3: return currentPresetLabel();
    case 4: return currentEffectLabel();
    case 5: return String("Level ") + byteToLevel(wled_fxSpeed);
    case 6: return String("Level ") + byteToLevel(wled_fxIntensity);
    case 7: return wifiStatusText();
  }
  return String();
}

void drawCenteredLabel(const String& text, int y, uint16_t color, const lgfx::IFont* font) {
  M5Dial.Display.setTextColor(color, CLR_BLACK);
  M5Dial.Display.drawCenterString(text.c_str(), 120, y, font);
}

void drawStatusDot(int x, int y) {
  uint16_t dotColor = (lastHttpCode == 200) ? CLR_GREEN : (lastHttpCode > 0 ? CLR_RED : CLR_LGRAY);
  M5Dial.Display.fillCircle(x, y, 3, dotColor);
}

bool containsIgnoreCase(const String& text, const char* needle) {
  String lowerText = text;
  String lowerNeedle = needle;
  lowerText.toLowerCase();
  lowerNeedle.toLowerCase();
  return lowerText.indexOf(lowerNeedle) >= 0;
}

void getPalettePreviewColors(const String& paletteName, uint16_t* outColors, int count) {
  String name = paletteName;
  name.toLowerCase();
  if (containsIgnoreCase(name, "lava") || containsIgnoreCase(name, "fire")) {
    uint16_t preset[] = {CLR_RED, CLR_ORANGE, CLR_YELLOW, 0x9A60, CLR_WHITE};
    for (int i = 0; i < count; ++i) outColors[i] = preset[min(i, 4)];
    return;
  }
  if (containsIgnoreCase(name, "ocean") || containsIgnoreCase(name, "breeze")) {
    uint16_t preset[] = {0x0010, CLR_BLUE, 0x03FF, CLR_CYAN, 0x5EFF};
    for (int i = 0; i < count; ++i) outColors[i] = preset[min(i, 4)];
    return;
  }
  if (containsIgnoreCase(name, "forest") || containsIgnoreCase(name, "beech")) {
    uint16_t preset[] = {0x0200, 0x03E0, 0x7FE0, 0x8C40, 0xAE60};
    for (int i = 0; i < count; ++i) outColors[i] = preset[min(i, 4)];
    return;
  }
  if (containsIgnoreCase(name, "party") || containsIgnoreCase(name, "mag")) {
    uint16_t preset[] = {CLR_MAGENTA, 0xB81F, CLR_BLUE, CLR_CYAN, CLR_YELLOW};
    for (int i = 0; i < count; ++i) outColors[i] = preset[min(i, 4)];
    return;
  }
  if (containsIgnoreCase(name, "sunset") || containsIgnoreCase(name, "april night")) {
    uint16_t preset[] = {CLR_RED, CLR_ORANGE, 0xF81F, 0x781F, 0x2815};
    for (int i = 0; i < count; ++i) outColors[i] = preset[min(i, 4)];
    return;
  }
  if (containsIgnoreCase(name, "cloud")) {
    uint16_t preset[] = {CLR_WHITE, 0xC7DF, 0x9D7C, 0x4BFF, 0x7D7F};
    for (int i = 0; i < count; ++i) outColors[i] = preset[min(i, 4)];
    return;
  }
  if (containsIgnoreCase(name, "ice")) {
    uint16_t preset[] = {0xB7FF, CLR_CYAN, 0x03FF, CLR_BLUE, CLR_WHITE};
    for (int i = 0; i < count; ++i) outColors[i] = preset[min(i, 4)];
    return;
  }
  if (containsIgnoreCase(name, "rainbow")) {
    uint16_t preset[] = {CLR_RED, CLR_ORANGE, CLR_YELLOW, CLR_GREEN, CLR_BLUE};
    for (int i = 0; i < count; ++i) outColors[i] = preset[min(i, 4)];
    return;
  }
  for (int i = 0; i < count; ++i) outColors[i] = hue16((wled_hue + i * (360 / count)) % 360);
}

void drawPalettePreview(int cx, int cy, int w, int h, const String& paletteName) {
  const int swatchCount = 5;
  uint16_t colors[swatchCount];
  getPalettePreviewColors(paletteName, colors, swatchCount);
  int gap = 3;
  int totalGap = gap * (swatchCount - 1);
  int sw = (w - totalGap) / swatchCount;
  int x = cx - w / 2;
  int y = cy - h / 2;
  for (int i = 0; i < swatchCount; ++i) {
    int sx = x + i * (sw + gap);
    M5Dial.Display.fillRoundRect(sx, y, sw, h, 4, colors[i]);
    M5Dial.Display.drawRoundRect(sx, y, sw, h, 4, CLR_DGRAY);
  }
}

void drawMenuIcon(int index, int cx, int cy, bool selected) {
  uint16_t ink = selected ? CLR_BLACK : MAIN_MENU_ITEMS[index].color;

  switch (index) {
    case 0:
      M5Dial.Display.drawCircle(cx, cy - 2, 5, ink);
      M5Dial.Display.drawLine(cx - 3, cy + 2, cx + 3, cy + 2, ink);
      M5Dial.Display.fillRect(cx - 2, cy + 4, 5, 2, ink);
      M5Dial.Display.drawPixel(cx - 1, cy + 7, ink);
      M5Dial.Display.drawPixel(cx + 1, cy + 7, ink);
      break;
    case 1:
      M5Dial.Display.drawCircle(cx, cy, 4, ink);
      for (int i = 0; i < 8; ++i) {
        float rad = i * (PI / 4.0f);
        int x0 = cx + (int)roundf(cosf(rad) * 7);
        int y0 = cy + (int)roundf(sinf(rad) * 7);
        int x1 = cx + (int)roundf(cosf(rad) * 10);
        int y1 = cy + (int)roundf(sinf(rad) * 10);
        M5Dial.Display.drawLine(x0, y0, x1, y1, ink);
      }
      break;
    case 2:
      M5Dial.Display.fillCircle(cx - 1, cy + 1, 7, ink);
      M5Dial.Display.fillCircle(cx + 4, cy - 2, 5, selected ? MAIN_MENU_ITEMS[index].color : CLR_BLACK);
      M5Dial.Display.drawCircle(cx - 1, cy + 1, 7, ink);
      M5Dial.Display.fillCircle(cx - 4, cy - 1, 1, selected ? MAIN_MENU_ITEMS[index].color : ink);
      M5Dial.Display.fillCircle(cx - 1, cy - 4, 1, selected ? MAIN_MENU_ITEMS[index].color : ink);
      M5Dial.Display.fillCircle(cx + 2, cy + 2, 1, selected ? MAIN_MENU_ITEMS[index].color : ink);
      break;
    case 3:
      M5Dial.Display.drawRoundRect(cx - 7, cy - 7, 14, 14, 2, ink);
      M5Dial.Display.drawLine(cx - 3, cy - 2, cx + 3, cy - 2, ink);
      M5Dial.Display.drawLine(cx - 3, cy + 2, cx + 3, cy + 2, ink);
      M5Dial.Display.fillCircle(cx - 1, cy - 5, 1, ink);
      break;
    case 4:
      M5Dial.Display.drawLine(cx, cy - 8, cx, cy + 8, ink);
      M5Dial.Display.drawLine(cx - 8, cy, cx + 8, cy, ink);
      M5Dial.Display.drawLine(cx - 6, cy - 6, cx + 6, cy + 6, ink);
      M5Dial.Display.drawLine(cx - 6, cy + 6, cx + 6, cy - 6, ink);
      break;
    case 5:
      for (int deg = 210; deg <= 330; deg += 20) {
        float rad = deg * DEG_TO_RAD;
        int px = cx + (int)roundf(cosf(rad) * 8);
        int py = cy + 2 + (int)roundf(sinf(rad) * 8);
        M5Dial.Display.drawPixel(px, py, ink);
      }
      M5Dial.Display.drawLine(cx, cy + 2, cx + 5, cy - 3, ink);
      M5Dial.Display.fillCircle(cx, cy + 2, 1, ink);
      break;
    case 6:
      M5Dial.Display.fillRect(cx - 6, cy + 1, 3, 6, ink);
      M5Dial.Display.fillRect(cx - 1, cy - 2, 3, 9, ink);
      M5Dial.Display.fillRect(cx + 4, cy - 5, 3, 12, ink);
      break;
    case 7:
      M5Dial.Display.drawCircle(cx, cy, 4, ink);
      for (int i = 0; i < 6; ++i) {
        float rad = i * (PI / 3.0f);
        int x0 = cx + (int)roundf(cosf(rad) * 6);
        int y0 = cy + (int)roundf(sinf(rad) * 6);
        int x1 = cx + (int)roundf(cosf(rad) * 9);
        int y1 = cy + (int)roundf(sinf(rad) * 9);
        M5Dial.Display.drawLine(x0, y0, x1, y1, ink);
      }
      break;
  }
}

void drawMainMenu() {
  drawCenteredLabel(MAIN_MENU_ITEMS[mainMenuIndex].title, 70, MAIN_MENU_ITEMS[mainMenuIndex].color, &fonts::FreeSansBold9pt7b);
  drawCenteredLabel(currentScreenSummary(), 110, CLR_WHITE, &fonts::FreeSans9pt7b);
  drawCenteredLabel(currentControllerLabel(), 145, CLR_DGRAY, &fonts::Font0);
  if (!wled_on) drawCenteredLabel("OFF", 176, CLR_RED, &fonts::FreeSansBold9pt7b);
  else if (!wifiConnected) drawCenteredLabel("NO WIFI", 176, CLR_RED, &fonts::FreeSansBold9pt7b);
  else drawCenteredLabel("Press to open", 176, CLR_DGRAY, &fonts::Font0);

  for (int i = 0; i < MAIN_MENU_COUNT; ++i) {
    float rad = MENU_ANGLES_DEG[i] * DEG_TO_RAD;
    int cx = MENU_CX + (int)roundf(cosf(rad) * MENU_RING_R);
    int cy = MENU_CY + (int)roundf(sinf(rad) * MENU_RING_R);
    bool selected = (i == mainMenuIndex);
    int radius = selected ? MENU_ITEM_R_SEL : MENU_ITEM_R;
    uint16_t fill = selected ? MAIN_MENU_ITEMS[i].color : CLR_BLACK;
    uint16_t stroke = selected ? MAIN_MENU_ITEMS[i].color : CLR_DGRAY;
    M5Dial.Display.fillCircle(cx, cy, radius, fill);
    M5Dial.Display.drawCircle(cx, cy, radius, stroke);
    if (!selected) M5Dial.Display.drawCircle(cx, cy, radius - 2, stroke);
    drawMenuIcon(i, cx, cy, selected);
  }

  if (mainMenuIndex == 2) {
    String palette = (paletteCount > 0 && wled_palIndex < paletteCount) ? paletteNames[wled_palIndex] : String("Rainbow");
    drawPalettePreview(120, 203, 118, 18, palette);
  }
  drawStatusDot(222, 22);
}

void drawListScreen(const char* title, ListKind kind) {
  int count = controllerCount;
  int current = selectionCursor;
  uint16_t accent = CLR_CYAN;
  if (kind == LIST_EFFECTS) {
    count = effectCount;
    current = wled_fxIndex;
    accent = CLR_BLUE;
  } else if (kind == LIST_PRESETS) {
    count = presetCount;
    current = presetCursor;
    accent = CLR_YELLOW;
  }
  drawCenteredLabel(title, 18, accent, &fonts::Font0);
  if (count <= 0) {
    drawCenteredLabel("Loading...", 118, CLR_LGRAY, &fonts::FreeSans9pt7b);
    return;
  }
  auto itemName = [&](int idx) -> String {
    idx = (idx % count + count) % count;
    if (kind == LIST_EFFECTS) return effectNames[idx];
    if (kind == LIST_PRESETS) return presetEntries[idx].name;
    return controllers[idx].name;
  };
  drawCenteredLabel(itemName(current - 3), 38, CLR_DGRAY, &fonts::Font0);
  drawCenteredLabel(itemName(current - 2), 60, CLR_DGRAY, &fonts::Font0);
  drawCenteredLabel(itemName(current - 1), 82, CLR_LGRAY, &fonts::FreeSans9pt7b);
  drawCenteredLabel(itemName(current), 114, CLR_WHITE, &fonts::FreeSansBold9pt7b);
  drawCenteredLabel(itemName(current + 1), 158, CLR_LGRAY, &fonts::FreeSans9pt7b);
  drawCenteredLabel(itemName(current + 2), 180, CLR_DGRAY, &fonts::Font0);
  drawCenteredLabel(itemName(current + 3), 202, CLR_DGRAY, &fonts::Font0);
  if (kind == LIST_CONTROLLERS && controllerCount > 0) drawCenteredLabel(controllers[current].ip, 218, CLR_DGRAY, &fonts::Font0);
  else {
    char cnt[16];
    snprintf(cnt, sizeof(cnt), "%d / %d", current + 1, count);
    drawCenteredLabel(cnt, 222, CLR_DGRAY, &fonts::Font0);
  }
  drawStatusDot(232, 120);
}

void drawPaletteHueScreen() {
  String palette = currentPaletteLabel();
  drawCenteredLabel("PALETTE", 22, CLR_ORANGE, &fonts::Font0);
  if (paletteCount <= 0) {
    drawCenteredLabel("Loading palettes...", 108, CLR_LGRAY, &fonts::FreeSans9pt7b);
    drawCenteredLabel("Turn knob once loaded", 140, CLR_DGRAY, &fonts::Font0);
    drawStatusDot(120, 222);
    return;
  }

  auto paletteNameAt = [&](int idx) -> String {
    idx = (idx % paletteCount + paletteCount) % paletteCount;
    return paletteNames[idx];
  };

  drawPalettePreview(120, 74, 128, 22, palette);
  drawCenteredLabel(paletteNameAt(wled_palIndex - 2), 106, CLR_DGRAY, &fonts::Font0);
  drawCenteredLabel(paletteNameAt(wled_palIndex - 1), 126, CLR_LGRAY, &fonts::Font0);
  drawCenteredLabel(palette, 148, CLR_WHITE, &fonts::FreeSansBold9pt7b);
  drawCenteredLabel(paletteNameAt(wled_palIndex + 1), 172, CLR_LGRAY, &fonts::Font0);
  drawCenteredLabel(paletteNameAt(wled_palIndex + 2), 192, CLR_DGRAY, &fonts::Font0);

  char countBuf[16];
  snprintf(countBuf, sizeof(countBuf), "%d / %d", wled_palIndex + 1, paletteCount);
  drawCenteredLabel(countBuf, 214, CLR_DGRAY, &fonts::Font0);
  drawStatusDot(120, 222);
}

void drawNumericScreen(const char* title, int displayValue, const char* suffix, float fraction, uint16_t color, const String& secondary) {
  drawArc(fraction, color);
  char buf[24];
  if (suffix && suffix[0]) snprintf(buf, sizeof(buf), "%d%s", displayValue, suffix);
  else snprintf(buf, sizeof(buf), "%d", displayValue);
  drawCenteredLabel(buf, 100, CLR_WHITE, &fonts::FreeSansBold12pt7b);
  drawCenteredLabel(title, 134, color, &fonts::FreeSans9pt7b);
  drawCenteredLabel(secondary, 170, CLR_DGRAY, &fonts::Font0);
  drawStatusDot(120, 188);
}

void drawSettingsScreen() {
  drawCenteredLabel("SETTINGS", 20, CLR_LGRAY, &fonts::Font0);
  const int rowTop = 74;
  const int rowH = 42;
  for (int i = 0; i < SETTING_COUNT; ++i) {
    int y = rowTop + i * rowH;
    bool selected = (settingsCursor == i);
    uint16_t border = selected ? CLR_WHITE : CLR_DGRAY;
    uint16_t text = selected ? CLR_WHITE : CLR_LGRAY;
    if (selected && settingsEditing) border = CLR_ACCENT;
    M5Dial.Display.drawRoundRect(20, y - 16, 200, 30, 6, border);
    const char* label = "";
    String value;
    if (i == SETTING_WIFI) {
      label = "WiFi";
      value = wifiStatusText();
    } else if (i == SETTING_SCREEN_BRIGHTNESS) {
      label = "Screen";
      value = String(byteToLevel(screenBrightness));
    } else if (i == SETTING_CLICK_VOLUME) {
      label = "Click";
      value = String((int)lroundf(clickVolume * 11.0f / 255.0f));
    }
    M5Dial.Display.setTextColor(text, CLR_BLACK);
    M5Dial.Display.drawString(label, 30, y - 2, &fonts::Font0);
    M5Dial.Display.drawRightString(value.c_str(), 210, y - 2, &fonts::Font0);
  }
  drawCenteredLabel(settingsEditing ? "Turn knob to adjust" : "Press to edit / open", 206, CLR_DGRAY, &fonts::Font0);
}

void drawDisplay() {
  M5Dial.Display.fillScreen(CLR_BLACK);
  if (appState == STATE_SCANNING) {
    drawCenteredLabel("SCANNING", 96, CLR_CYAN, &fonts::FreeSansBold9pt7b);
    drawCenteredLabel("for WLED", 120, CLR_LGRAY, &fonts::FreeSans9pt7b);
    drawCenteredLabel("controllers...", 142, CLR_LGRAY, &fonts::FreeSans9pt7b);
    return;
  }
  if (appState == STATE_SELECT) {
    drawListScreen("SELECT LIGHT", LIST_CONTROLLERS);
    return;
  }
  if (!wifiConnected && controlScreen != SCREEN_SETTINGS) {
    drawCenteredLabel("NO WIFI", 108, CLR_RED, &fonts::FreeSansBold9pt7b);
    drawCenteredLabel("Open Settings -> WiFi", 140, CLR_LGRAY, &fonts::Font0);
    return;
  }
  if (!wled_on && controlScreen != SCREEN_MAIN_MENU && controlScreen != SCREEN_SETTINGS) {
    drawCenteredLabel("LIGHT OFF", 108, CLR_RED, &fonts::FreeSansBold9pt7b);
    drawCenteredLabel("Hold from Home to toggle", 140, CLR_LGRAY, &fonts::Font0);
    return;
  }
  switch (controlScreen) {
    case SCREEN_MAIN_MENU:
      drawMainMenu();
      break;
    case SCREEN_BRIGHTNESS:
      drawNumericScreen("BRIGHTNESS", brightnessToUi(wled_bri), "", (brightnessToUi(wled_bri) - UI_LEVEL_MIN) / (float)(UI_LEVEL_MAX - UI_LEVEL_MIN), CLR_ACCENT, currentControllerLabel());
      break;
    case SCREEN_PALETTE_HUE:
      drawPaletteHueScreen();
      break;
    case SCREEN_PRESET:
      drawListScreen("PRESET", LIST_PRESETS);
      break;
    case SCREEN_FX_PATTERN:
      drawListScreen("FX PATTERN", LIST_EFFECTS);
      break;
    case SCREEN_FX_SPEED:
      drawNumericScreen("FX SPEED", byteToLevel(wled_fxSpeed), "", (byteToLevel(wled_fxSpeed) - UI_LEVEL_MIN) / (float)(UI_LEVEL_MAX - UI_LEVEL_MIN), CLR_GREEN, currentEffectLabel());
      break;
    case SCREEN_FX_INTENSITY:
      drawNumericScreen("FX INTENSITY", byteToLevel(wled_fxIntensity), "", (byteToLevel(wled_fxIntensity) - UI_LEVEL_MIN) / (float)(UI_LEVEL_MAX - UI_LEVEL_MIN), CLR_MAGENTA, currentEffectLabel());
      break;
    case SCREEN_SETTINGS:
      drawSettingsScreen();
      break;
  }
}
