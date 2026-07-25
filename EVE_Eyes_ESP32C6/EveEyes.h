#ifndef EVE_EYES_H
#define EVE_EYES_H

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// =============================================================================
// Hardware Pins — Waveshare ESP32-C6-LCD-1.47
// =============================================================================
#define PIN_LCD_MOSI 6
#define PIN_LCD_SCLK 7
#define PIN_LCD_MISO 5
#define PIN_LCD_CS   14
#define PIN_LCD_DC   15
#define PIN_LCD_RST  21
#define PIN_LCD_BL   22
#define PIN_BTN_BOOT 9    // Physical BOOT button (Active LOW)
#define PIN_RGB_LED  8    // WS2812 Addressable RGB LED

// =============================================================================
// Display & Canvas Configuration (320 x 172 ST7789)
// =============================================================================
#define SCREEN_W      320
#define SCREEN_H      172

#define EYE_CANVAS_W  120
#define EYE_CANVAS_H  120

#define CANVAS_L_X    25
#define CANVAS_L_Y    26
#define CANVAS_R_X    175
#define CANVAS_R_Y    26

// =============================================================================
// Colors
// =============================================================================
#define COL_BLACK      0x0000
#define COL_WHITE      0xFFFF
#define COL_EVE_BLUE   0x3DDF    // Bright Cyan-Blue (#38B6FF / #00CCFF)
#define COL_EVE_GLOW   0x11E9    // Deep subtle background glow halo

// =============================================================================
// Math Helpers
// =============================================================================
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// =============================================================================
// Eye Configuration Structure
// =============================================================================
struct EyeConfig {
  float Width;
  float Height;
  float Radius_Top;
  float Radius_Bottom;
  float Slope_Top;
  float Slope_Bottom;
  float OffsetX;
  float OffsetY;
};

inline EyeConfig lerpEyeConfig(const EyeConfig &a, const EyeConfig &b, float t) {
  EyeConfig c;
  c.Width         = lerpf(a.Width, b.Width, t);
  c.Height        = lerpf(a.Height, b.Height, t);
  c.Radius_Top    = lerpf(a.Radius_Top, b.Radius_Top, t);
  c.Radius_Bottom = lerpf(a.Radius_Bottom, b.Radius_Bottom, t);
  c.Slope_Top     = lerpf(a.Slope_Top, b.Slope_Top, t);
  c.Slope_Bottom  = lerpf(a.Slope_Bottom, b.Slope_Bottom, t);
  c.OffsetX       = lerpf(a.OffsetX, b.OffsetX, t);
  c.OffsetY       = lerpf(a.OffsetY, b.OffsetY, t);
  return c;
}

// =============================================================================
// Emotions Enum
// =============================================================================
enum eEmotion {
  EMO_NORMAL = 0,
  EMO_HAPPY,
  EMO_GLEE,
  EMO_SAD,
  EMO_WORRIED,
  EMO_FOCUSED,
  EMO_ANNOYED,
  EMO_SURPRISED,
  EMO_SKEPTIC,
  EMO_FRUSTRATED,
  EMO_UNIMPRESSED,
  EMO_SLEEPY,
  EMO_SUSPICIOUS,
  EMO_SQUINT,
  EMO_ANGRY,
  EMO_FURIOUS,
  EMO_SCARED,
  EMO_AWE,
  EMO_COUNT
};

// Preset Configs
static const EyeConfig Preset_Normal = {
  .Width = 74, .Height = 74, .Radius_Top = 24, .Radius_Bottom = 24,
  .Slope_Top = 0.0f, .Slope_Bottom = 0.0f, .OffsetX = 0.0f, .OffsetY = 0.0f
};

static const EyeConfig Preset_Happy = {
  .Width = 74, .Height = 36, .Radius_Top = 24, .Radius_Bottom = 4,
  .Slope_Top = 0.0f, .Slope_Bottom = 0.0f, .OffsetX = 0.0f, .OffsetY = -5.0f
};

static const EyeConfig Preset_Glee = {
  .Width = 74, .Height = 26, .Radius_Top = 22, .Radius_Bottom = 2,
  .Slope_Top = 0.0f, .Slope_Bottom = 0.0f, .OffsetX = 0.0f, .OffsetY = -8.0f
};

static const EyeConfig Preset_Sad = {
  .Width = 74, .Height = 46, .Radius_Top = 6, .Radius_Bottom = 24,
  .Slope_Top = -0.35f, .Slope_Bottom = 0.0f, .OffsetX = 0.0f, .OffsetY = 5.0f
};

static const EyeConfig Preset_Worried = {
  .Width = 74, .Height = 56, .Radius_Top = 16, .Radius_Bottom = 24,
  .Slope_Top = -0.25f, .Slope_Bottom = 0.0f, .OffsetX = 0.0f, .OffsetY = 0.0f
};

static const EyeConfig Preset_Focused = {
  .Width = 74, .Height = 36, .Radius_Top = 10, .Radius_Bottom = 6,
  .Slope_Top = 0.25f, .Slope_Bottom = 0.0f, .OffsetX = 0.0f, .OffsetY = 0.0f
};

static const EyeConfig Preset_Annoyed = {
  .Width = 74, .Height = 32, .Radius_Top = 4, .Radius_Bottom = 20,
  .Slope_Top = 0.0f, .Slope_Bottom = 0.0f, .OffsetX = 0.0f, .OffsetY = 0.0f
};

static const EyeConfig Preset_Surprised = {
  .Width = 86, .Height = 86, .Radius_Top = 36, .Radius_Bottom = 36,
  .Slope_Top = 0.0f, .Slope_Bottom = 0.0f, .OffsetX = 0.0f, .OffsetY = 0.0f
};

static const EyeConfig Preset_Skeptic = {
  .Width = 74, .Height = 46, .Radius_Top = 8, .Radius_Bottom = 22,
  .Slope_Top = 0.25f, .Slope_Bottom = 0.0f, .OffsetX = 0.0f, .OffsetY = -4.0f
};

static const EyeConfig Preset_Frustrated = {
  .Width = 74, .Height = 30, .Radius_Top = 4, .Radius_Bottom = 18,
  .Slope_Top = 0.0f, .Slope_Bottom = 0.0f, .OffsetX = 5.0f, .OffsetY = -6.0f
};

static const EyeConfig Preset_Unimpressed = {
  .Width = 74, .Height = 40, .Radius_Top = 6, .Radius_Bottom = 24,
  .Slope_Top = 0.0f, .Slope_Bottom = 0.0f, .OffsetX = 5.0f, .OffsetY = 0.0f
};

static const EyeConfig Preset_Sleepy = {
  .Width = 74, .Height = 28, .Radius_Top = 8, .Radius_Bottom = 8,
  .Slope_Top = -0.35f, .Slope_Bottom = -0.35f, .OffsetX = 0.0f, .OffsetY = -4.0f
};

static const EyeConfig Preset_Suspicious = {
  .Width = 74, .Height = 44, .Radius_Top = 18, .Radius_Bottom = 8,
  .Slope_Top = 0.0f, .Slope_Bottom = 0.0f, .OffsetX = 0.0f, .OffsetY = 0.0f
};

static const EyeConfig Preset_Squint = {
  .Width = 62, .Height = 38, .Radius_Top = 14, .Radius_Bottom = 14,
  .Slope_Top = 0.0f, .Slope_Bottom = 0.0f, .OffsetX = -10.0f, .OffsetY = -4.0f
};

static const EyeConfig Preset_Angry = {
  .Width = 74, .Height = 42, .Radius_Top = 4, .Radius_Bottom = 22,
  .Slope_Top = 0.35f, .Slope_Bottom = 0.0f, .OffsetX = -4.0f, .OffsetY = 0.0f
};

static const EyeConfig Preset_Furious = {
  .Width = 74, .Height = 54, .Radius_Top = 4, .Radius_Bottom = 16,
  .Slope_Top = 0.45f, .Slope_Bottom = 0.0f, .OffsetX = -4.0f, .OffsetY = 0.0f
};

static const EyeConfig Preset_Scared = {
  .Width = 74, .Height = 74, .Radius_Top = 24, .Radius_Bottom = 14,
  .Slope_Top = -0.15f, .Slope_Bottom = 0.0f, .OffsetX = -5.0f, .OffsetY = 0.0f
};

static const EyeConfig Preset_Awe = {
  .Width = 82, .Height = 68, .Radius_Top = 26, .Radius_Bottom = 26,
  .Slope_Top = -0.15f, .Slope_Bottom = 0.15f, .OffsetX = 4.0f, .OffsetY = 0.0f
};

// =============================================================================
// Button Handler (GPIO 9 BOOT button)
// =============================================================================
class ButtonHandler {
private:
  uint8_t pin;
  bool lastState;
  uint32_t lastDB;
  uint32_t pressT;
  uint32_t lastClickT;
  uint8_t clicks;
public:
  ButtonHandler(uint8_t p)
    : pin(p), lastState(HIGH), lastDB(0), pressT(0), lastClickT(0), clicks(0) {}

  void begin() {
    pinMode(pin, INPUT_PULLUP);
  }

  uint8_t update() {
    bool r = digitalRead(pin);   // LOW when pressed
    uint32_t now = millis();
    uint8_t res = 0;

    if (r != lastState) {
      lastDB = now;
    }

    if ((now - lastDB) > 30) {
      static bool stable = HIGH;
      if (r != stable) {
        stable = r;
        if (stable == LOW) {
          pressT = now;
        } else if ((now - pressT) < 1000) {
          clicks++;
          lastClickT = now;
        }
      }
    }
    lastState = r;

    if (clicks > 0 && (now - lastClickT) > 300) {
      res = clicks;
      clicks = 0;
    }
    return res;
  }
};

#endif
