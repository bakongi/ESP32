/*
 * ═══════════════════════════════════════════════════════════════════════════════
 *   PC & ESP32 Hardware Monitor Dashboard with Animated Circular Gauges
 *   Board: Waveshare ESP32-C6-LCD-1.47 (ST7789 172×320 IPS)
 * ═══════════════════════════════════════════════════════════════════════════════
 *   Pages:
 *     - Page 1: PC CPU Load, PC CPU Temp, PC RAM Load
 *     - Page 2: PC GPU Load, PC GPU Temp, PC VRAM Load
 *     - Page 3: ESP32-C6 CPU Load, ESP32 Internal Temp, ESP32 SRAM Heap Load
 *   Controls (single BOOT button, GPIO 9):
 *     - Dashboard: TAP  = Switch Pages (1 -> 2 -> 3 -> 1)
 *                  HOLD = Open Display Settings
 *     - Settings:  TAP  = Brightness 20 -> 40 -> 60 -> 80 -> 100 -> 20 %
 *                  HOLD = Save & Exit (also auto-saves after 10 s idle)
 *     Brightness is stored in NVS and restored on boot.
 * ═══════════════════════════════════════════════════════════════════════════════
 */

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Preferences.h>
#include "driver/temperature_sensor.h"

// ─── Hardware Pins ───────────────────────────────────────────────────────────
#define PIN_LCD_MOSI 6
#define PIN_LCD_SCLK 7
#define PIN_LCD_MISO 5
#define PIN_LCD_CS   14
#define PIN_LCD_DC   15
#define PIN_LCD_RST  21
#define PIN_LCD_BL   22
#define PIN_BTN_BOOT 9     // BOOT button for switching pages
#define PIN_RGB_LED  8     // WS2812 RGB LED

// ─── Screen Layout ───────────────────────────────────────────────────────────
#define SCREEN_W     320
#define SCREEN_H     172

// ─── Colors (RGB565) ─────────────────────────────────────────────────────────
#define COLOR_BG         0x0824   // Dark Slate Navy (#080B18)
#define COLOR_CARD_BG    0x10A6   // Dark Card (#101424)
#define COLOR_BORDER     0x2510   // Dark Track Gray (#252B40)
#define COLOR_TEXT_MAIN  0xFFFF   // White
#define COLOR_TEXT_MUTED 0x8C71   // Muted Blue/Gray

// Accent Colors
#define COLOR_CPU        0x05DF   // Cyan (#00BCFF)
#define COLOR_RAM        0x9B1F   // Magenta/Purple (#9B51E0)
#define COLOR_GPU        0x07E0   // Green (#00E676)
#define COLOR_VRAM       0xFCE0   // Gold/Orange (#FFD600)
#define COLOR_TEMP_OK    0x07E0   // Green (< 55°C)
#define COLOR_TEMP_WARM  0xFFE0   // Yellow (55 - 74°C)
#define COLOR_TEMP_HOT   0xF800   // Red (>= 75°C)

// ─── Display & Canvas ────────────────────────────────────────────────────────
Adafruit_ST7789 tft = Adafruit_ST7789(&SPI, PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST);
GFXcanvas16 canvas(SCREEN_W, SCREEN_H);

// ─── PC Metrics Data ─────────────────────────────────────────────────────────
struct PcMetrics {
  int cpuLoad = 0;       // 0 - 100%
  int cpuTemp = 0;       // °C
  int ramLoad = 0;       // 0 - 100%
  int gpuLoad = 0;       // 0 - 100%
  int gpuTemp = 0;       // °C
  int vramLoad = 0;      // 0 - 100%
  uint32_t lastPacketTime = 0;
  bool isConnected = false;
};

PcMetrics sysData;

// ─── ESP32-C6 Internal Metrics Data ──────────────────────────────────────────
struct EspMetrics {
  float cpuLoad = 15.0f;
  float chipTemp = 0.0f;
  float ramLoad = 0.0f;
  uint32_t usedHeapKb = 0;
  uint32_t totalHeapKb = 0;
};

EspMetrics espData;

// Smooth Animated Gauge Values
struct SmoothGauges {
  float cpuLoad = 0.0f;
  float cpuTemp = 0.0f;
  float ramLoad = 0.0f;
  float gpuLoad = 0.0f;
  float gpuTemp = 0.0f;
  float vramLoad = 0.0f;
  float espCpuLoad = 15.0f;
  float espChipTemp = 35.0f;
  float espRamLoad = 10.0f;
};

SmoothGauges animGauges;

// Active Page (0 = PC CPU, 1 = PC GPU, 2 = ESP32 Board)
uint8_t activePage = 0;

// ─── UI Mode ─────────────────────────────────────────────────────────────────
enum UiMode : uint8_t {
  MODE_DASHBOARD = 0,   // Gauge pages
  MODE_SETTINGS  = 1    // Display settings screen
};

UiMode uiMode = MODE_DASHBOARD;
uint32_t settingsLastInput = 0;      // For the idle auto-close timer

// ─── Brightness Settings (persisted in NVS) ──────────────────────────────────
const uint8_t BRIGHTNESS_LEVELS[] = { 20, 40, 60, 80, 100 };
const uint8_t BRIGHTNESS_COUNT    = sizeof(BRIGHTNESS_LEVELS) / sizeof(BRIGHTNESS_LEVELS[0]);
#define BRIGHTNESS_DEFAULT_IDX 3     // 80% ~ matches the previous fixed 190/255

uint8_t brightnessIdx      = BRIGHTNESS_DEFAULT_IDX;
uint8_t brightnessSavedIdx = BRIGHTNESS_DEFAULT_IDX;   // Last value written to flash

Preferences prefs;

// ─── Button State Machine (debounce + tap / hold detection) ──────────────────
#define BTN_DEBOUNCE_MS      30
#define BTN_HOLD_MS          700     // How long to hold to trigger the hold action
#define SETTINGS_TIMEOUT_MS  10000   // Auto save & close the settings screen

struct ButtonState {
  bool     stable       = HIGH;      // Debounced level
  bool     lastRead     = HIGH;      // Last raw sample
  uint32_t lastChangeMs = 0;         // When the raw sample last flipped
  uint32_t pressStartMs = 0;         // When the debounced press began
  bool     holdFired    = false;     // Hold action already executed for this press
};

ButtonState btn;

// ESP32 Temperature Driver Handle
temperature_sensor_handle_t temp_handle = NULL;

// ─── Prototypes ──────────────────────────────────────────────────────────────
void initEspTempSensor();
float readEspChipTemp();
void processSerialInput();
void parsePacketLine(String line);
void updateButton();
void onButtonTap();
void onButtonHold();
float getHoldProgress();
void applyBrightness();
void loadBrightness();
void saveBrightness();
void closeSettings();
void renderScreen();
void renderSettings();
void drawSunIcon(int cx, int cy, uint8_t pct);
void drawHeader();
void drawFooter();
void drawCircularGauge(int cx, int cy, int rIn, int rOut, float currentVal, float maxVal, const char* title, const char* unit, uint16_t color);
void drawRingArc(int cx, int cy, int rIn, int rOut, float startDeg, float endDeg, uint16_t color);
void setRgbLed(uint8_t r, uint8_t g, uint8_t b);
uint16_t getLoadColor(float val);
uint16_t getTempColor(int temp);

// =============================================================================
//  Setup
// =============================================================================
void setup() {
  Serial.setRxBufferSize(1024);
  Serial.begin(115200);

  pinMode(PIN_BTN_BOOT, INPUT_PULLUP);

  // Backlight PWM - restore the brightness the user picked last time
  pinMode(PIN_LCD_BL, OUTPUT);
  loadBrightness();
  applyBrightness();

  // SPI & TFT setup
  SPI.begin(PIN_LCD_SCLK, PIN_LCD_MISO, PIN_LCD_MOSI, PIN_LCD_CS);
  SPI.setFrequency(27000000);

  tft.init(172, 320);
  tft.setRotation(1); // Landscape 320x172
  tft.fillScreen(COLOR_BG);

  // Initialize ESP32-C6 Internal Silicon Temperature Sensor
  initEspTempSensor();

  // Initial LED status: Amber
  setRgbLed(255, 120, 0);
}

// ─── ESP32 Internal Silicon Temperature Sensor Driver ───────────────────────
void initEspTempSensor() {
  temperature_sensor_config_t temp_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
  if (temperature_sensor_install(&temp_config, &temp_handle) == ESP_OK) {
    temperature_sensor_enable(temp_handle);
  }
}

float readEspChipTemp() {
  float tsens_out = 0.0f;
  if (temp_handle != NULL) {
    if (temperature_sensor_get_celsius(temp_handle, &tsens_out) == ESP_OK) {
      return tsens_out;
    }
  }
  return temperatureRead();
}

// =============================================================================
//  Brightness Control (persisted in NVS namespace "pcmon", key "bright")
// =============================================================================
void loadBrightness() {
  if (prefs.begin("pcmon", true)) {           // Read-only open
    brightnessIdx = prefs.getUChar("bright", BRIGHTNESS_DEFAULT_IDX);
    prefs.end();
  }
  if (brightnessIdx >= BRIGHTNESS_COUNT) brightnessIdx = BRIGHTNESS_DEFAULT_IDX;
  brightnessSavedIdx = brightnessIdx;
}

// Only touches flash when the value actually changed (avoids NVS wear)
void saveBrightness() {
  if (brightnessIdx == brightnessSavedIdx) return;
  if (prefs.begin("pcmon", false)) {
    prefs.putUChar("bright", brightnessIdx);
    prefs.end();
    brightnessSavedIdx = brightnessIdx;
  }
}

void applyBrightness() {
  uint8_t pct = BRIGHTNESS_LEVELS[brightnessIdx];
  // Linear map so the on-screen percentage matches the PWM duty cycle
  uint8_t duty = (uint8_t)((pct * 255) / 100);
  analogWrite(PIN_LCD_BL, duty);
}

// =============================================================================
//  Button: debounce + tap / hold state machine
// =============================================================================
void updateButton() {
  uint32_t now = millis();
  bool raw = digitalRead(PIN_BTN_BOOT);

  // Track raw edges for the debounce window
  if (raw != btn.lastRead) {
    btn.lastRead     = raw;
    btn.lastChangeMs = now;
  }

  // Accept the new level once it has been stable long enough
  if (raw != btn.stable && (now - btn.lastChangeMs) >= BTN_DEBOUNCE_MS) {
    btn.stable = raw;
    if (btn.stable == LOW) {
      // Press started
      btn.pressStartMs = now;
      btn.holdFired    = false;
    } else {
      // Released: a tap only counts if the hold action did not already fire
      if (!btn.holdFired) onButtonTap();
    }
  }

  // Hold fires while the button is still down (instant feedback, no wait for release)
  if (btn.stable == LOW && !btn.holdFired && (now - btn.pressStartMs) >= BTN_HOLD_MS) {
    btn.holdFired = true;
    onButtonHold();
  }
}

// 0.0 .. 1.0 progress of the current hold - drives the on-screen hold indicator
float getHoldProgress() {
  if (btn.stable != LOW || btn.holdFired) return 0.0f;
  float p = (float)(millis() - btn.pressStartMs) / (float)BTN_HOLD_MS;
  return constrain(p, 0.0f, 1.0f);
}

void onButtonTap() {
  if (uiMode == MODE_DASHBOARD) {
    activePage = (activePage + 1) % 3;
  } else {
    // Step brightness 20 -> 40 -> 60 -> 80 -> 100 -> 20, applied live
    brightnessIdx = (brightnessIdx + 1) % BRIGHTNESS_COUNT;
    applyBrightness();
    settingsLastInput = millis();
  }
}

void onButtonHold() {
  if (uiMode == MODE_DASHBOARD) {
    uiMode = MODE_SETTINGS;
    settingsLastInput = millis();
  } else {
    closeSettings();
  }
}

void closeSettings() {
  saveBrightness();
  uiMode = MODE_DASHBOARD;
}

// =============================================================================
//  Main Loop
// =============================================================================
void loop() {
  uint32_t frameStartMicros = micros();

  processSerialInput();

  // Single-button input: TAP = page / brightness step, HOLD = enter / leave settings
  updateButton();

  // Auto save & close the settings screen when left untouched
  if (uiMode == MODE_SETTINGS && (millis() - settingsLastInput) > SETTINGS_TIMEOUT_MS) {
    closeSettings();
  }

  // Update ESP32 Board Metrics (Heap RAM & Internal Chip Temperature)
  static uint32_t lastEspUpdate = 0;
  if (millis() - lastEspUpdate > 200) {
    lastEspUpdate = millis();

    espData.chipTemp = readEspChipTemp();

    uint32_t totalH = ESP.getHeapSize();
    uint32_t freeH  = ESP.getFreeHeap();
    uint32_t usedH  = (totalH > freeH) ? (totalH - freeH) : 0;

    espData.totalHeapKb = totalH / 1024;
    espData.usedHeapKb  = usedH / 1024;
    espData.ramLoad     = (totalH > 0) ? ((float)usedH * 100.0f / totalH) : 0.0f;

    // Estimate ESP CPU load based on frame workload execution time ratio
    uint32_t workMicros = micros() - frameStartMicros;
    float workPercent = (workMicros / 33000.0f) * 100.0f;
    espData.cpuLoad = constrain(workPercent + 10.0f, 8.0f, 100.0f);
  }

  // Smooth lerp animation for gauge values (0.18 interpolation factor)
  animGauges.cpuLoad     += (sysData.cpuLoad    - animGauges.cpuLoad)     * 0.18f;
  animGauges.cpuTemp     += (sysData.cpuTemp    - animGauges.cpuTemp)     * 0.18f;
  animGauges.ramLoad     += (sysData.ramLoad    - animGauges.ramLoad)     * 0.18f;
  animGauges.gpuLoad     += (sysData.gpuLoad    - animGauges.gpuLoad)     * 0.18f;
  animGauges.gpuTemp     += (sysData.gpuTemp    - animGauges.gpuTemp)     * 0.18f;
  animGauges.vramLoad    += (sysData.vramLoad   - animGauges.vramLoad)    * 0.18f;
  animGauges.espCpuLoad  += (espData.cpuLoad    - animGauges.espCpuLoad)  * 0.18f;
  animGauges.espChipTemp += (espData.chipTemp   - animGauges.espChipTemp) * 0.18f;
  animGauges.espRamLoad  += (espData.ramLoad    - animGauges.espRamLoad)  * 0.18f;

  // Connection timeout & RGB LED status check
  if (millis() - sysData.lastPacketTime > 3000) {
    sysData.isConnected = false;
    setRgbLed(255, 50, 0); // Orange/Red warning
  } else {
    sysData.isConnected = true;

    int maxTemp = max((int)sysData.cpuTemp, max((int)sysData.gpuTemp, (int)espData.chipTemp));
    if (maxTemp >= 80) {
      // Emergency Red Blink
      if ((millis() / 250) % 2 == 0) setRgbLed(255, 0, 0);
      else                           setRgbLed(0, 0, 0);
    } else if (maxTemp >= 70) {
      setRgbLed(255, 140, 0); // Amber
    } else if (maxTemp >= 55) {
      setRgbLed(255, 220, 0); // Yellow
    } else {
      setRgbLed(0, 255, 0);   // Green
    }
  }

  // Render display frame (~30 FPS)
  static uint32_t lastRender = 0;
  if (millis() - lastRender > 33) {
    lastRender = millis();
    renderScreen();
  }
}

// =============================================================================
//  Serial Protocol Parser (Non-blocking character reader)
// =============================================================================
static String serialRxBuf = "";

void parsePacketLine(String line) {
  line.trim();
  if (line.length() == 0) return;

  int cpuVal = -1, ramVal = -1, tempVal = -1, gpuVal = -1, gpuTempVal = -1, vramVal = -1;

  int start = 0;
  while (start < line.length()) {
    int end = line.indexOf(';', start);
    if (end == -1) end = line.length();

    String token = line.substring(start, end);
    token.trim();

    int colon = token.indexOf(':');
    if (colon != -1) {
      String key = token.substring(0, colon);
      String valStr = token.substring(colon + 1);
      key.trim();
      valStr.trim();
      int v = valStr.toInt();

      if (key.equalsIgnoreCase("CPU"))        cpuVal = v;
      else if (key.equalsIgnoreCase("RAM"))   ramVal = v;
      else if (key.equalsIgnoreCase("TEMP"))  tempVal = v;
      else if (key.equalsIgnoreCase("GPU"))   gpuVal = v;
      else if (key.equalsIgnoreCase("GPUTEMP")) gpuTempVal = v;
      else if (key.equalsIgnoreCase("VRAM"))  vramVal = v;
    }

    start = end + 1;
  }

  if (cpuVal >= 0 && ramVal >= 0) {
    sysData.cpuLoad = constrain(cpuVal, 0, 100);
    sysData.ramLoad = constrain(ramVal, 0, 100);
    if (tempVal >= 0) sysData.cpuTemp = tempVal;
    if (gpuVal >= 0) sysData.gpuLoad = constrain(gpuVal, 0, 100);
    if (gpuTempVal >= 0) sysData.gpuTemp = gpuTempVal;
    if (vramVal >= 0) sysData.vramLoad = constrain(vramVal, 0, 100);

    sysData.lastPacketTime = millis();
    sysData.isConnected = true;
  }
}

void processSerialInput() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialRxBuf.length() > 0) {
        parsePacketLine(serialRxBuf);
        serialRxBuf = "";
      }
    } else {
      if (serialRxBuf.length() < 128) {
        serialRxBuf += c;
      }
    }
  }
}

// =============================================================================
//  Rendering Engine
// =============================================================================
void renderScreen() {
  canvas.fillScreen(COLOR_BG);

  drawHeader();

  if (uiMode == MODE_SETTINGS) {
    renderSettings();
  } else if (!sysData.isConnected) {
    // Waiting view
    canvas.setCursor(60, 65);
    canvas.setTextColor(COLOR_VRAM);
    canvas.setTextSize(2);
    canvas.print(F("WAITING FOR PC"));

    canvas.setCursor(45, 95);
    canvas.setTextColor(COLOR_TEXT_MUTED);
    canvas.setTextSize(1);
    canvas.print(F("Run pc_monitor_sender.py (COM12)"));

    static int animX = 0;
    animX = (animX + 4) % 240;
    canvas.drawRoundRect(40, 120, 240, 10, 3, COLOR_BORDER);
    canvas.fillRoundRect(40 + animX, 121, 30, 8, 2, COLOR_CPU);
  } else {
    int cy = 82;
    int rIn = 25;
    int rOut = 34;

    if (activePage == 0) {
      // ─── PAGE 1: PC CPU & RAM Gauges ──────────────────────────────────────
      uint16_t cpuLoadCol = getLoadColor(animGauges.cpuLoad);
      drawCircularGauge(54, cy, rIn, rOut, animGauges.cpuLoad, 100.0f, "CPU LOAD", "%", cpuLoadCol);

      uint16_t cpuTempCol = getTempColor((int)animGauges.cpuTemp);
      drawCircularGauge(160, cy, rIn, rOut, animGauges.cpuTemp, 100.0f, "CPU TEMP", "C", cpuTempCol);

      uint16_t ramLoadCol = getLoadColor(animGauges.ramLoad);
      drawCircularGauge(266, cy, rIn, rOut, animGauges.ramLoad, 100.0f, "RAM LOAD", "%", ramLoadCol);
    } else if (activePage == 1) {
      // ─── PAGE 2: PC GPU & VRAM Gauges ─────────────────────────────────────
      uint16_t gpuLoadCol = getLoadColor(animGauges.gpuLoad);
      drawCircularGauge(54, cy, rIn, rOut, animGauges.gpuLoad, 100.0f, "GPU LOAD", "%", gpuLoadCol);

      uint16_t gpuTempCol = getTempColor((int)animGauges.gpuTemp);
      drawCircularGauge(160, cy, rIn, rOut, animGauges.gpuTemp, 100.0f, "GPU TEMP", "C", gpuTempCol);

      uint16_t vramLoadCol = getLoadColor(animGauges.vramLoad);
      drawCircularGauge(266, cy, rIn, rOut, animGauges.vramLoad, 100.0f, "VRAM LOAD", "%", vramLoadCol);
    } else {
      // ─── PAGE 3: ESP32-C6 BOARD MONITOR ──────────────────────────────────
      // Gauge 1: ESP CPU Load
      uint16_t espCpuCol = getLoadColor(animGauges.espCpuLoad);
      drawCircularGauge(54, cy, rIn, rOut, animGauges.espCpuLoad, 100.0f, "ESP CPU", "%", espCpuCol);

      // Gauge 2: ESP Internal Silicon Temperature
      uint16_t espTempCol = getTempColor((int)animGauges.espChipTemp);
      drawCircularGauge(160, cy, rIn, rOut, animGauges.espChipTemp, 100.0f, "ESP TEMP", "C", espTempCol);

      // Gauge 3: ESP SRAM Usage
      uint16_t espRamCol = getLoadColor(animGauges.espRamLoad);
      drawCircularGauge(266, cy, rIn, rOut, animGauges.espRamLoad, 100.0f, "ESP RAM", "%", espRamCol);
    }
  }

  drawFooter();

  // Atomic push to display
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), SCREEN_W, SCREEN_H);
}

// ─── Display Settings Screen ─────────────────────────────────────────────────
// Single-button menu: TAP steps the brightness (applied live so the user sees
// the result immediately), HOLD saves and returns to the gauges.
void renderSettings() {
  uint8_t pct = BRIGHTNESS_LEVELS[brightnessIdx];

  // Section label
  canvas.setTextSize(1);
  canvas.setTextColor(COLOR_TEXT_MUTED);
  canvas.setCursor(160 - (18 * 6) / 2, 32);
  canvas.print(F("SCREEN BRIGHTNESS"));

  // Sun icon - rays grow with the brightness level
  drawSunIcon(62, 66, pct);

  // Large current value + percent sign
  char valBuf[5];
  snprintf(valBuf, sizeof(valBuf), "%u", pct);
  int digits = strlen(valBuf);
  int blockW = digits * 24 + 4 + 12;          // size-4 digits + gap + size-2 '%'
  int blockX = 190 - blockW / 2;

  canvas.setTextSize(4);
  canvas.setTextColor(COLOR_TEXT_MAIN);
  canvas.setCursor(blockX, 50);
  canvas.print(valBuf);

  canvas.setTextSize(2);
  canvas.setTextColor(COLOR_CPU);
  canvas.setCursor(blockX + digits * 24 + 4, 66);
  canvas.print(F("%"));

  // Segmented level bar (one block per step) with labels underneath
  const int barX = 30, barY = 96, barH = 18, gap = 6, segW = 47;
  for (uint8_t i = 0; i < BRIGHTNESS_COUNT; i++) {
    int x = barX + i * (segW + gap);

    if (i <= brightnessIdx) canvas.fillRoundRect(x, barY, segW, barH, 3, COLOR_CPU);
    else                    canvas.drawRoundRect(x, barY, segW, barH, 3, COLOR_BORDER);

    // Highlight the block that represents the active level
    if (i == brightnessIdx) canvas.drawRoundRect(x - 2, barY - 2, segW + 4, barH + 4, 4, COLOR_TEXT_MAIN);

    char lbl[5];
    snprintf(lbl, sizeof(lbl), "%u", BRIGHTNESS_LEVELS[i]);
    canvas.setTextSize(1);
    canvas.setTextColor(i == brightnessIdx ? COLOR_TEXT_MAIN : COLOR_TEXT_MUTED);
    canvas.setCursor(x + segW / 2 - (int)strlen(lbl) * 3, barY + barH + 5);
    canvas.print(lbl);
  }

  // Idle auto-close progress (shrinks from full width to zero)
  uint32_t elapsed = millis() - settingsLastInput;
  if (elapsed < SETTINGS_TIMEOUT_MS) {
    int w = SCREEN_W - (int)((uint32_t)SCREEN_W * elapsed / SETTINGS_TIMEOUT_MS);
    canvas.drawFastHLine(0, 150, w, COLOR_BORDER);
  }
}

// ─── Sun Icon (ray length scales with brightness) ────────────────────────────
void drawSunIcon(int cx, int cy, uint8_t pct) {
  canvas.fillCircle(cx, cy, 9, COLOR_VRAM);

  int rayLen = 3 + (pct * 9) / 100;
  for (int i = 0; i < 8; i++) {
    float a = i * 45.0f * (PI / 180.0f);
    int x0 = cx + (int)roundf(cosf(a) * 13.0f);
    int y0 = cy + (int)roundf(sinf(a) * 13.0f);
    int x1 = cx + (int)roundf(cosf(a) * (13.0f + rayLen));
    int y1 = cy + (int)roundf(sinf(a) * (13.0f + rayLen));
    canvas.drawLine(x0, y0, x1, y1, COLOR_VRAM);
  }
}

// ─── Header Bar ──────────────────────────────────────────────────────────────
void drawHeader() {
  canvas.fillRect(0, 0, SCREEN_W, 22, COLOR_CARD_BG);
  canvas.drawFastHLine(0, 22, SCREEN_W, COLOR_BORDER);

  if (uiMode == MODE_SETTINGS) {
    // Gear icon
    canvas.drawCircle(15, 10, 6, COLOR_VRAM);
    canvas.fillCircle(15, 10, 2, COLOR_VRAM);
    canvas.drawFastVLine(15, 1, 3, COLOR_VRAM);
    canvas.drawFastVLine(15, 16, 3, COLOR_VRAM);
    canvas.drawFastHLine(6, 10, 3, COLOR_VRAM);
    canvas.drawFastHLine(21, 10, 3, COLOR_VRAM);

    canvas.setCursor(28, 7);
    canvas.setTextColor(COLOR_TEXT_MAIN);
    canvas.setTextSize(1);
    canvas.print(F("DISPLAY SETTINGS"));

    canvas.setCursor(240, 7);
    canvas.setTextColor(COLOR_VRAM);
    canvas.print(F("1/1 SCREEN"));
    return;
  }

  // Monitor Icon
  canvas.drawRoundRect(8, 4, 14, 11, 2, COLOR_CPU);
  canvas.drawFastHLine(11, 15, 8, COLOR_CPU);
  canvas.drawFastVLine(14, 15, 3, COLOR_CPU);

  // Title
  canvas.setCursor(28, 7);
  canvas.setTextColor(COLOR_TEXT_MAIN);
  canvas.setTextSize(1);
  canvas.print(F("HARDWARE MONITOR"));

  // Connection Indicator & Page Counter (P1/3 [CPU], P2/3 [GPU], P3/3 [ESP])
  if (sysData.isConnected) {
    canvas.fillCircle(195, 10, 3, COLOR_GPU);
    canvas.setCursor(203, 7);
    canvas.setTextColor(COLOR_GPU);

    const char* pageNames[] = {"CPU", "GPU", "ESP"};
    canvas.printf("P%d/3 [%s]", activePage + 1, pageNames[activePage]);
  } else {
    canvas.fillCircle(195, 10, 3, COLOR_TEMP_HOT);
    canvas.setCursor(203, 7);
    canvas.setTextColor(COLOR_TEMP_HOT);
    canvas.print(F("OFFLINE"));
  }
}

// ─── Footer Bar ──────────────────────────────────────────────────────────────
void drawFooter() {
  int y = 153;
  canvas.fillRect(0, y, SCREEN_W, 19, COLOR_CARD_BG);
  canvas.drawFastHLine(0, y, SCREEN_W, COLOR_BORDER);

  float hold = getHoldProgress();

  canvas.setCursor(8, y + 5);
  canvas.setTextSize(1);

  if (hold > 0.10f) {
    // The user is mid-hold: tell them what letting go / holding on will do
    canvas.setTextColor(COLOR_VRAM);
    if (uiMode == MODE_SETTINGS) canvas.print(F("KEEP HOLDING TO SAVE & EXIT"));
    else                         canvas.print(F("KEEP HOLDING FOR SETTINGS"));
  } else {
    canvas.setTextColor(COLOR_TEXT_MUTED);
    if (uiMode == MODE_SETTINGS) canvas.print(F("TAP: Brightness  HOLD: Save & Exit"));
    else                         canvas.print(F("TAP: Page  HOLD: Settings"));
  }

  if (uiMode == MODE_SETTINGS) {
    // Seconds left before the screen closes itself
    uint32_t elapsed = millis() - settingsLastInput;
    int secsLeft = (elapsed >= SETTINGS_TIMEOUT_MS)
                     ? 0 : (int)((SETTINGS_TIMEOUT_MS - elapsed + 999) / 1000);
    canvas.setCursor(272, y + 5);
    canvas.setTextColor(COLOR_TEXT_MUTED);
    canvas.printf("%2ds", secsLeft);
  } else if (sysData.isConnected) {
    canvas.setCursor(205, y + 5);
    canvas.setTextColor(COLOR_TEXT_MAIN);
    if (activePage == 2) {
      canvas.printf("SRAM:%d/%dK", espData.usedHeapKb, espData.totalHeapKb);
    } else {
      canvas.printf("C:%dC G:%dC", sysData.cpuTemp, sysData.gpuTemp);
    }
  }

  // Hold progress bar along the very bottom edge
  if (hold > 0.0f) {
    canvas.fillRect(0, SCREEN_H - 3, (int)(SCREEN_W * hold), 3, COLOR_VRAM);
  }
}

// ─── Circular Gauge Component ────────────────────────────────────────────────
void drawCircularGauge(int cx, int cy, int rIn, int rOut, float currentVal, float maxVal, const char* title, const char* unit, uint16_t color) {
  // Sweep angle: -135° to +135° (270° total sweep)
  float startDeg = 135.0f;
  float totalSweep = 270.0f;
  float endDeg = startDeg + totalSweep;

  // 1. Draw Background Track Ring Arc
  drawRingArc(cx, cy, rIn, rOut, startDeg, endDeg, COLOR_BORDER);

  // 2. Draw Active Animated Arc
  float clampedVal = constrain(currentVal, 0.0f, maxVal);
  float valSweep = (clampedVal / maxVal) * totalSweep;
  if (valSweep > 1.0f) {
    drawRingArc(cx, cy, rIn, rOut, startDeg, startDeg + valSweep, color);
  }

  // 3. Centered Large Numeric Text
  canvas.setTextColor(COLOR_TEXT_MAIN);
  canvas.setTextSize(2);
  int valInt = (int)roundf(clampedVal);

  String strVal = String(valInt);
  int textW = strVal.length() * 12;
  canvas.setCursor(cx - (textW / 2), cy - 7);
  canvas.print(strVal);

  // 4. Centered Unit Symbol below value (% or C)
  canvas.setTextColor(color);
  canvas.setTextSize(1);
  canvas.setCursor(cx + (textW / 2) + 1, cy - 6);
  canvas.print(unit);

  // 5. Title Badge below Gauge
  canvas.setCursor(cx - 24, cy + rOut + 6);
  canvas.setTextColor(COLOR_TEXT_MUTED);
  canvas.setTextSize(1);
  canvas.print(title);
}

// ─── Ring Arc Drawing Engine ─────────────────────────────────────────────────
// Builds the ring segment out of solid filled triangles between consecutive
// angle samples (instead of a fan of thin radial lines). Adjacent triangles
// share exact vertices, so the ring is seamless - no rounding-induced gaps,
// regardless of radius or step size.
void drawRingArc(int cx, int cy, int rIn, int rOut, float startDeg, float endDeg, uint16_t color) {
  float sweep = endDeg - startDeg;
  if (sweep <= 0.0f) return;

  const float maxStepDeg = 6.0f; // fine enough to look round, coarse enough to stay cheap
  int steps = (int)ceilf(sweep / maxStepDeg);
  if (steps < 1) steps = 1;
  float stepDeg = sweep / steps;

  float rad = startDeg * (PI / 180.0f);
  int xi0 = cx + (int)roundf(rIn * cosf(rad));
  int yi0 = cy + (int)roundf(rIn * sinf(rad));
  int xo0 = cx + (int)roundf(rOut * cosf(rad));
  int yo0 = cy + (int)roundf(rOut * sinf(rad));

  for (int i = 1; i <= steps; i++) {
    float deg = startDeg + stepDeg * i;
    rad = deg * (PI / 180.0f);
    int xi1 = cx + (int)roundf(rIn * cosf(rad));
    int yi1 = cy + (int)roundf(rIn * sinf(rad));
    int xo1 = cx + (int)roundf(rOut * cosf(rad));
    int yo1 = cy + (int)roundf(rOut * sinf(rad));

    canvas.fillTriangle(xi0, yi0, xo0, yo0, xo1, yo1, color);
    canvas.fillTriangle(xi0, yi0, xo1, yo1, xi1, yi1, color);

    xi0 = xi1; yi0 = yi1;
    xo0 = xo1; yo0 = yo1;
  }
}

// ─── Helper: Get Color based on Load Percentage ─────────────────────────────
uint16_t getLoadColor(float val) {
  if (val >= 85.0f)      return COLOR_TEMP_HOT;  // Red (Heavy load)
  else if (val >= 60.0f) return COLOR_TEMP_WARM; // Yellow (Moderate load)
  else                   return COLOR_TEMP_OK;   // Bright Green (Light load)
}

// ─── Helper: Get Color based on Temperature ──────────────────────────────────
uint16_t getTempColor(int temp) {
  if (temp >= 75)      return COLOR_TEMP_HOT;  // Red
  else if (temp >= 55) return COLOR_TEMP_WARM; // Yellow
  else                  return COLOR_TEMP_OK;   // Green
}

// ─── Helper: WS2812 GRB Channel Order ────────────────────────────────────────
void setRgbLed(uint8_t r, uint8_t g, uint8_t b) {
  neopixelWrite(PIN_RGB_LED, g, r, b);
}
