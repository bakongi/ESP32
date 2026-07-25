/*
 * ═══════════════════════════════════════════════════════════════════════════════
 *   Emotive Animated Eyes Engine — esp32-eyes Port with RGB LED & Dynamic Motion
 *   Based on: https://github.com/playfultechnology/esp32-eyes
 *   Board: Waveshare ESP32-C6-LCD-1.47 (ST7789 172×320)
 * ═══════════════════════════════════════════════════════════════════════════════
 */

#include "EveEyes.h"

// ─── Hardware ────────────────────────────────────────────────────────────────
Adafruit_ST7789 tft = Adafruit_ST7789(&SPI, PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST);
GFXcanvas16 canvasL(EYE_CANVAS_W, EYE_CANVAS_H);
GFXcanvas16 canvasR(EYE_CANVAS_W, EYE_CANVAS_H);
ButtonHandler button(PIN_BTN_BOOT);   // Physical BOOT button on GPIO 9

// ─── Animation State ─────────────────────────────────────────────────────────
EyeConfig curConfigL = Preset_Normal;
EyeConfig curConfigR = Preset_Normal;
EyeConfig tarConfigL = Preset_Normal;
EyeConfig tarConfigR = Preset_Normal;

// Behavior & emotion state
eEmotion currentEmotion = EMO_NORMAL;
uint32_t emotionStartTime = 0;
bool randomBehavior = true;
uint32_t nextBehaviorTime = 4000;

// Dynamic active animation offsets
float animOffsetX = 0.0f;
float animOffsetY = 0.0f;

// Look assistant
float lookTargetX = 0.0f;
float lookTargetY = 0.0f;
uint32_t nextLookTime = 2500;

// Blink assistant
enum BlinkState { BLINK_IDLE, BLINK_CLOSING, BLINK_OPENING };
BlinkState blinkState = BLINK_IDLE;
float blinkProgress = 1.0f;     // 1.0 = open, 0.0 = closed
uint32_t nextBlinkTime = 3000;

// Frame timing
uint32_t lastFrameTime = 0;

// =============================================================================
//  RGB LED Controller (WS2812 on GPIO 8)
// =============================================================================
void updateRgbLed() {
  switch (currentEmotion) {
    case EMO_HAPPY:
    case EMO_GLEE:
      neopixelWrite(PIN_RGB_LED, 0, 220, 255);      // Vibrant Cyan-Blue
      break;

    case EMO_ANGRY:
    case EMO_FURIOUS:
      neopixelWrite(PIN_RGB_LED, 255, 0, 30);       // Intense Red
      break;

    case EMO_SURPRISED:
    case EMO_AWE:
      neopixelWrite(PIN_RGB_LED, 200, 240, 255);    // Bright White-Blue
      break;

    case EMO_SAD:
    case EMO_WORRIED:
      neopixelWrite(PIN_RGB_LED, 0, 30, 180);       // Deep Blue
      break;

    case EMO_SLEEPY:
      neopixelWrite(PIN_RGB_LED, 0, 15, 50);        // Dim Soft Blue
      break;

    default:
      neopixelWrite(PIN_RGB_LED, 0, 140, 255);      // EVE Default Cyan
      break;
  }
}

// =============================================================================
//  Preset Lookup
// =============================================================================
const EyeConfig* getPreset(eEmotion emo) {
  switch (emo) {
    case EMO_NORMAL:      return &Preset_Normal;
    case EMO_HAPPY:       return &Preset_Happy;
    case EMO_GLEE:        return &Preset_Glee;
    case EMO_SAD:         return &Preset_Sad;
    case EMO_WORRIED:     return &Preset_Worried;
    case EMO_FOCUSED:     return &Preset_Focused;
    case EMO_ANNOYED:     return &Preset_Annoyed;
    case EMO_SURPRISED:   return &Preset_Surprised;
    case EMO_SKEPTIC:     return &Preset_Skeptic;
    case EMO_FRUSTRATED:  return &Preset_Frustrated;
    case EMO_UNIMPRESSED: return &Preset_Unimpressed;
    case EMO_SLEEPY:      return &Preset_Sleepy;
    case EMO_SUSPICIOUS:  return &Preset_Suspicious;
    case EMO_SQUINT:      return &Preset_Squint;
    case EMO_ANGRY:       return &Preset_Angry;
    case EMO_FURIOUS:     return &Preset_Furious;
    case EMO_SCARED:      return &Preset_Scared;
    case EMO_AWE:         return &Preset_Awe;
    default:              return &Preset_Normal;
  }
}

void setEmotion(eEmotion emo, bool triggerBlink = true) {
  currentEmotion = emo;
  emotionStartTime = millis();
  const EyeConfig* p = getPreset(emo);
  tarConfigL = *p;
  tarConfigR = *p;

  if (emo == EMO_SKEPTIC) {
    tarConfigR = Preset_Annoyed;
  } else if (emo == EMO_SQUINT) {
    tarConfigR = Preset_Normal;
    tarConfigR.Height = 40;
  }

  if (triggerBlink) {
    blinkState = BLINK_CLOSING;
    blinkProgress = 0.3f;
  }

  updateRgbLed();
}

// =============================================================================
//  Pixel-Perfect Parametric Eye Rasterizer
// =============================================================================
static inline bool isInsideEye(float x, float y, float cx, float cy,
                               float w, float h, float st, float sb,
                               float rt, float rb) {
  float dx = x - cx;
  float dy = y - cy;

  if (dx < -w / 2.0f || dx > w / 2.0f) return false;

  float top_y = cy - h / 2.0f + dx * st;
  float bot_y = cy + h / 2.0f + dx * sb;

  if (y < top_y || y > bot_y) return false;

  float halfW = w / 2.0f;

  if (rt > 1.0f && dx < -halfW + rt) {
    float cornerX = cx - halfW + rt;
    float cornerY = top_y + rt;
    if (y < cornerY) {
      float cdx = x - cornerX, cdy = y - cornerY;
      if (cdx * cdx + cdy * cdy > rt * rt) return false;
    }
  }

  if (rt > 1.0f && dx > halfW - rt) {
    float cornerX = cx + halfW - rt;
    float cornerY = top_y + rt;
    if (y < cornerY) {
      float cdx = x - cornerX, cdy = y - cornerY;
      if (cdx * cdx + cdy * cdy > rt * rt) return false;
    }
  }

  if (rb > 1.0f && dx < -halfW + rb) {
    float cornerX = cx - halfW + rb;
    float cornerY = bot_y - rb;
    if (y > cornerY) {
      float cdx = x - cornerX, cdy = y - cornerY;
      if (cdx * cdx + cdy * cdy > rb * rb) return false;
    }
  }

  if (rb > 1.0f && dx > halfW - rb) {
    float cornerX = cx + halfW - rb;
    float cornerY = bot_y - rb;
    if (y > cornerY) {
      float cdx = x - cornerX, cdy = y - cornerY;
      if (cdx * cdx + cdy * cdy > rb * rb) return false;
    }
  }

  return true;
}

void renderEyeShape(GFXcanvas16 &c, EyeConfig cfg, bool isRightEye, float blinkFactor) {
  c.fillScreen(COL_BLACK);

  if (isRightEye) {
    cfg.Slope_Top    = -cfg.Slope_Top;
    cfg.Slope_Bottom = -cfg.Slope_Bottom;
    cfg.OffsetX      = -cfg.OffsetX;
  }

  float cx = (EYE_CANVAS_W / 2.0f) + cfg.OffsetX + lookTargetX + animOffsetX;
  float cy = (EYE_CANVAS_H / 2.0f) + cfg.OffsetY + lookTargetY + animOffsetY;

  float h  = max(2.0f, cfg.Height * blinkFactor);
  float w  = max(10.0f, cfg.Width);
  float st = cfg.Slope_Top;
  float sb = cfg.Slope_Bottom;

  float maxR = min(w / 2.0f, h / 2.0f);
  float rt   = clampf(cfg.Radius_Top * blinkFactor, 1.0f, maxR);
  float rb   = clampf(cfg.Radius_Bottom * blinkFactor, 1.0f, maxR);

  if (h < 5.0f) {
    c.fillRoundRect((int)(cx - w / 2.0f), (int)(cy - 2.0f), (int)w, 4, 2, COL_EVE_BLUE);
    return;
  }

  for (int y = 0; y < EYE_CANVAS_H; y++) {
    int x1 = -1, x2 = -1;
    float fy = (float)y + 0.5f;

    for (int x = 0; x < EYE_CANVAS_W; x++) {
      float fx = (float)x + 0.5f;
      if (isInsideEye(fx, fy, cx, cy, w, h, st, sb, rt, rb)) {
        if (x1 == -1) x1 = x;
        x2 = x;
      }
    }

    if (x1 != -1) {
      int gx1 = max(0, x1 - 2);
      int gx2 = min(EYE_CANVAS_W - 1, x2 + 2);
      c.drawFastHLine(gx1, y, gx2 - gx1 + 1, COL_EVE_GLOW);
      c.drawFastHLine(x1, y, x2 - x1 + 1, COL_EVE_BLUE);
    }
  }
}

// =============================================================================
//  Active Motion Animations per Emotion
// =============================================================================
void updateActiveEmotionAnimation() {
  uint32_t elapsed = millis() - emotionStartTime;
  float t = elapsed / 1000.0f;

  switch (currentEmotion) {
    case EMO_HAPPY:
    case EMO_GLEE:
      animOffsetY = sinf(t * 6.0f) * 6.0f;          // Joyful bouncing
      animOffsetX = sinf(t * 1.5f) * 3.0f;
      break;

    case EMO_ANGRY:
    case EMO_FURIOUS:
      animOffsetX = random(-3, 4);                   // Tremor / shake
      animOffsetY = random(-2, 3);
      break;

    case EMO_SURPRISED:
    case EMO_AWE:
      animOffsetY = -sinf(t * 4.0f) * 3.0f;
      animOffsetX = 0.0f;
      break;

    case EMO_SAD:
    case EMO_WORRIED:
      animOffsetY = 4.0f + sinf(t * 1.5f) * 2.0f;    // Drooping / looking down
      animOffsetX = sinf(t * 0.8f) * 2.0f;
      break;

    case EMO_SLEEPY:
      animOffsetY = 3.0f;
      animOffsetX = 0.0f;
      break;

    default:
      animOffsetX = 0.0f;
      animOffsetY = 0.0f;
      break;
  }

  // Auto-reset to NORMAL after 5 seconds of button emotion
  if (!randomBehavior && currentEmotion != EMO_NORMAL && elapsed > 5000) {
    setEmotion(EMO_NORMAL, true);
    randomBehavior = true;
    nextBehaviorTime = millis() + 4000;
  }
}

// =============================================================================
//  Behavior Assistants
// =============================================================================
void updateBlink() {
  uint32_t now = millis();
  switch (blinkState) {
    case BLINK_IDLE:
      if (now > nextBlinkTime) {
        blinkState = BLINK_CLOSING;
      }
      break;
    case BLINK_CLOSING:
      blinkProgress -= 0.25f;
      if (blinkProgress <= 0.0f) {
        blinkProgress = 0.0f;
        blinkState = BLINK_OPENING;
      }
      break;
    case BLINK_OPENING:
      blinkProgress += 0.25f;
      if (blinkProgress >= 1.0f) {
        blinkProgress = 1.0f;
        blinkState = BLINK_IDLE;
        nextBlinkTime = now + random(2200, 6000);
      }
      break;
  }
}

void updateLook() {
  uint32_t now = millis();
  if (now > nextLookTime) {
    lookTargetX = random(-14, 15);
    lookTargetY = random(-10, 11);
    nextLookTime = now + random(1500, 4500);
  }
}

void updateRandomBehavior() {
  if (!randomBehavior) return;
  uint32_t now = millis();
  if (now > nextBehaviorTime) {
    eEmotion randEmo = (eEmotion)random(0, EMO_COUNT);
    setEmotion(randEmo, true);
    nextBehaviorTime = now + random(4500, 9000);
  }
}

// =============================================================================
//  Setup & Loop
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("esp32-eyes Engine Initializing..."));

  pinMode(PIN_LCD_BL, OUTPUT);
  analogWrite(PIN_LCD_BL, 190);

  SPI.begin(PIN_LCD_SCLK, PIN_LCD_MISO, PIN_LCD_MOSI, PIN_LCD_CS);
  SPI.setFrequency(27000000);

  tft.init(172, 320);
  tft.setRotation(1);
  tft.fillScreen(COL_BLACK);

  button.begin();
  setEmotion(EMO_NORMAL, false);

  Serial.println(F("esp32-eyes Engine Ready!"));
}

void loop() {
  // ── Button Input (BOOT button on GPIO 9) ─────────────────────────────────
  uint8_t clicks = button.update();
  if (clicks > 0) {
    randomBehavior = false;
    eEmotion targetEmo;
    switch (clicks) {
      case 1: targetEmo = EMO_HAPPY;      break;
      case 2: targetEmo = EMO_SURPRISED;  break;
      case 3: targetEmo = EMO_ANGRY;      break;
      case 4: targetEmo = EMO_SAD;        break;
      case 5: targetEmo = EMO_GLEE;       break;
      case 6: targetEmo = EMO_SKEPTIC;    break;
      case 7: targetEmo = EMO_WORRIED;    break;
      case 8: targetEmo = EMO_SLEEPY;     break;
      default: targetEmo = EMO_NORMAL; randomBehavior = true; break;
    }
    Serial.printf("BOOT Button Pressed! Clicks: %d -> Emotion: %d\n", clicks, targetEmo);
    setEmotion(targetEmo, true);
  }

  // ── Frame Gate (30 FPS) ──────────────────────────────────────────────────
  uint32_t now = millis();
  if (now - lastFrameTime < 33) return;
  lastFrameTime = now;

  // ── Behavior & Active Motion Update ──────────────────────────────────────
  updateBlink();
  updateLook();
  updateRandomBehavior();
  updateActiveEmotionAnimation();

  // ── Lerp Interpolation ───────────────────────────────────────────────────
  float lerpSpeed = 0.20f;
  curConfigL = lerpEyeConfig(curConfigL, tarConfigL, lerpSpeed);
  curConfigR = lerpEyeConfig(curConfigR, tarConfigR, lerpSpeed);

  // ── Render ───────────────────────────────────────────────────────────────
  renderEyeShape(canvasL, curConfigL, false, blinkProgress);
  renderEyeShape(canvasR, curConfigR, true,  blinkProgress);

  // ── Atomic DMA Push (Zero Flicker) ───────────────────────────────────────
  tft.drawRGBBitmap(CANVAS_L_X, CANVAS_L_Y, canvasL.getBuffer(), EYE_CANVAS_W, EYE_CANVAS_H);
  tft.drawRGBBitmap(CANVAS_R_X, CANVAS_R_Y, canvasR.getBuffer(), EYE_CANVAS_W, EYE_CANVAS_H);
}
