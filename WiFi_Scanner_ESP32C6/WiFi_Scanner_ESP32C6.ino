/*
 * ═══════════════════════════════════════════════════════════════════════════════
 *   WiFi Scanner with Scrollable List for Waveshare ESP32-C6-LCD-1.47
 *   Board: Waveshare ESP32-C6-LCD-1.47 (ST7789 172×320 IPS)
 *   Controls: BOOT Button (GPIO 9)
 *     - Short Press: Scroll down list
 *     - Long Press / Double Click: Rescan WiFi networks
 * ═══════════════════════════════════════════════════════════════════════════════
 */

#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// ─── Hardware Pin Definitions ────────────────────────────────────────────────
#define PIN_LCD_MOSI 6
#define PIN_LCD_SCLK 7
#define PIN_LCD_MISO 5
#define PIN_LCD_CS   14
#define PIN_LCD_DC   15
#define PIN_LCD_RST  21
#define PIN_LCD_BL   22
#define PIN_BTN_BOOT 9     // Physical BOOT button (Active LOW)
#define PIN_RGB_LED  8     // WS2812 Addressable RGB LED

// ─── Screen Dimensions & Layout ──────────────────────────────────────────────
#define SCREEN_W     320
#define SCREEN_H     172

#define VISIBLE_ITEMS 4    // Number of WiFi network items visible on screen at once
#define ITEM_HEIGHT   30   // Height of each list item in pixels
#define LIST_TOP_Y    26   // Y start position of the list
#define ITEM_SPACING  31   // Vertical offset between list items

// ─── Color Palette (RGB565 format) ───────────────────────────────────────────
#define COLOR_BG         0x0824   // Dark Slate Navy (#080B18)
#define COLOR_CARD_BG    0x18C8   // Slightly lighter card background (#181D30)
#define COLOR_CARD_SEL   0x2213   // Highlighted card background (#222A45)
#define COLOR_HEADER_BG  0x10A6   // Dark top header bar (#101424)
#define COLOR_FOOTER_BG  0x10A6   // Dark bottom footer bar (#101424)
#define COLOR_BORDER     0x2D90   // Subtle card border (#2C3550)
#define COLOR_BORDER_SEL 0x04BF   // Active accent border (#0097FF)
#define COLOR_TEXT_MAIN  0xFFFF   // White text
#define COLOR_TEXT_MUTED 0x9CE7   // Light gray/blue text (#9AA3BA)
#define COLOR_ACCENT     0x05DF   // Vibrant Cyan/Blue Accent (#00BCFF)
#define COLOR_ACCENT_ALT 0x9B1F   // Soft Magenta/Purple (#9B51E0)

// Signal strength colors
#define COLOR_SIG_EXCELLENT 0x07E0 // Bright Green (-50 dBm or better)
#define COLOR_SIG_GOOD      0x07DF // Cyan/Green (-65 to -51 dBm)
#define COLOR_SIG_FAIR      0xFFE0 // Yellow (-75 to -66 dBm)
#define COLOR_SIG_POOR      0xF800 // Red (-80 dBm or worse)

// ─── WiFi Network Item Struct ────────────────────────────────────────────────
struct NetworkInfo {
  String ssid;
  int32_t rssi;
  uint8_t channel;
  wifi_auth_mode_t authmode;
  bool isSecure;
};

// ─── Display & Canvas ────────────────────────────────────────────────────────
Adafruit_ST7789 tft = Adafruit_ST7789(&SPI, PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST);
GFXcanvas16 canvas(SCREEN_W, SCREEN_H);

// ─── Application State ───────────────────────────────────────────────────────
NetworkInfo networks[32];
int totalNetworks = 0;
int topIndex = 0;           // Index of the first visible item in list
bool isScanning = false;
uint32_t lastScanTime = 0;
const uint32_t AUTO_RESCAN_INTERVAL = 30000; // Auto rescan every 30 seconds

// ─── Button State & Debounce ─────────────────────────────────────────────────
uint32_t btnPressTime = 0;
bool lastBtnState = HIGH;
bool isBtnPressed = false;
bool longPressHandled = false;

// ─── Prototypes ──────────────────────────────────────────────────────────────
void startScan();
void processScanResult();
void handleButton();
void renderScreen();
void drawHeader();
void drawFooter();
void drawNetworkItem(int displayRow, int netIndex);
void drawSignalBars(int x, int y, int rssi);
String getAuthModeString(wifi_auth_mode_t mode);
uint16_t getRssiColor(int rssi);

// Helper function for WS2812 RGB LED (corrects GRB channel order on board)
void setRgbLed(uint8_t r, uint8_t g, uint8_t b) {
  neopixelWrite(PIN_RGB_LED, g, r, b);
}

// =============================================================================
//  Setup
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println(F("=========================================="));
  Serial.println(F(" ESP32-C6 WiFi Scanner Initializing..."));
  Serial.println(F("=========================================="));

  // Initialize BOOT button
  pinMode(PIN_BTN_BOOT, INPUT_PULLUP);

  // Initialize Backlight PWM (Thermal Management: ~75% brightness)
  pinMode(PIN_LCD_BL, OUTPUT);
  analogWrite(PIN_LCD_BL, 190);

  // Initialize SPI & TFT Display
  SPI.begin(PIN_LCD_SCLK, PIN_LCD_MISO, PIN_LCD_MOSI, PIN_LCD_CS);
  SPI.setFrequency(27000000); // 27 MHz SPI frequency

  tft.init(172, 320);
  tft.setRotation(1); // Landscape mode 320x172
  tft.fillScreen(COLOR_BG);

  // Initialize WiFi mode
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  // Initial RGB status LED: Blue (Scanning)
  setRgbLed(0, 0, 255);

  // Start first asynchronous WiFi scan
  startScan();
}

// =============================================================================
//  Main Loop
// =============================================================================
void loop() {
  handleButton();

  // Blink blue LED while scanning is in progress
  if (isScanning) {
    static uint32_t lastLedBlink = 0;
    static bool ledState = false;
    if (millis() - lastLedBlink > 250) {
      lastLedBlink = millis();
      ledState = !ledState;
      if (ledState) {
        setRgbLed(0, 0, 255); // Pure Blue
      } else {
        setRgbLed(0, 0, 0);   // Off
      }
    }
  }

  // Check async scan completion status
  if (isScanning) {
    int16_t scanResult = WiFi.scanComplete();
    if (scanResult >= 0) {
      processScanResult();
      isScanning = false;
      renderScreen();
    } else if (scanResult == WIFI_SCAN_FAILED) {
      Serial.println(F("WiFi Scan Failed! Retrying..."));
      isScanning = false;
      totalNetworks = 0;
      setRgbLed(255, 0, 0); // Red on failure
      renderScreen();
    }
  }

  // Periodic automatic background rescan
  if (!isScanning && (millis() - lastScanTime > AUTO_RESCAN_INTERVAL)) {
    startScan();
  }

  // Render animation frames (spinner while scanning, or standard view)
  static uint32_t lastRenderTime = 0;
  if (millis() - lastRenderTime > 50) { // ~20 FPS display update cycle
    lastRenderTime = millis();
    renderScreen();
  }
}

// =============================================================================
//  WiFi Scanning
// =============================================================================
void startScan() {
  isScanning = true;
  lastScanTime = millis();
  Serial.println(F("Starting WiFi Scan..."));
  WiFi.scanNetworks(true, true); // async = true, show_hidden = true
  renderScreen();
}

void processScanResult() {
  int count = WiFi.scanComplete();
  Serial.printf("WiFi Scan Complete. Found %d networks.\n", count);
  
  totalNetworks = min(count, 32);
  for (int i = 0; i < totalNetworks; i++) {
    networks[i].ssid = WiFi.SSID(i);
    if (networks[i].ssid.length() == 0) {
      networks[i].ssid = "<Hidden Network>";
    }
    networks[i].rssi = WiFi.RSSI(i);
    networks[i].channel = WiFi.channel(i);
    networks[i].authmode = WiFi.encryptionType(i);
    networks[i].isSecure = (networks[i].authmode != WIFI_AUTH_OPEN);
  }

  WiFi.scanDelete();

  // Reset top index if out of range
  if (topIndex >= totalNetworks) {
    topIndex = 0;
  }

  // LED indication based on network count:
  // > 2 networks: Green
  // 1-2 networks: Yellow
  // 0 networks: Red
  if (totalNetworks > 2) {
    setRgbLed(0, 255, 0);    // Pure Green
  } else if (totalNetworks > 0) {
    setRgbLed(255, 200, 0);  // Yellow
  } else {
    setRgbLed(255, 0, 0);    // Pure Red
  }
}

// =============================================================================
//  Button Input Handling
// =============================================================================
void handleButton() {
  bool currentReading = digitalRead(PIN_BTN_BOOT); // Active LOW

  if (currentReading == LOW && lastBtnState == HIGH) {
    // Button pressed down
    btnPressTime = millis();
    isBtnPressed = true;
    longPressHandled = false;
  } else if (currentReading == LOW && isBtnPressed) {
    // Button held down -> check for Long Press (> 800ms)
    if (!longPressHandled && (millis() - btnPressTime > 800)) {
      longPressHandled = true;
      Serial.println(F("Button Long Press -> Force Rescan!"));
      topIndex = 0;
      startScan();
    }
  } else if (currentReading == HIGH && lastBtnState == LOW) {
    // Button released
    if (isBtnPressed && !longPressHandled) {
      // Short click -> Scroll list down
      if (totalNetworks > 0) {
        topIndex++;
        if (topIndex >= totalNetworks) {
          topIndex = 0; // Wrap around back to top
        }
        Serial.printf("Button Clicked -> Scroll to index %d / %d\n", topIndex + 1, totalNetworks);
        renderScreen();
      }
    }
    isBtnPressed = false;
  }

  lastBtnState = currentReading;
}

// =============================================================================
//  Rendering Engine (Zero-Flicker Double Buffering)
// =============================================================================
void renderScreen() {
  canvas.fillScreen(COLOR_BG);

  drawHeader();

  if (isScanning && totalNetworks == 0) {
    // Show Scanning indicator box
    canvas.setCursor(95, 75);
    canvas.setTextColor(COLOR_ACCENT);
    canvas.setTextSize(2);
    canvas.print(F("Scanning..."));

    canvas.setCursor(70, 105);
    canvas.setTextColor(COLOR_TEXT_MUTED);
    canvas.setTextSize(1);
    canvas.print(F("Searching for WiFi networks"));

    // Animated loading spinner dot
    static uint8_t spinnerFrame = 0;
    spinnerFrame = (spinnerFrame + 1) % 8;
    int cx = 160, cy = 135;
    for (int i = 0; i < 8; i++) {
      float angle = i * (PI / 4.0f);
      int px = cx + (int)(cos(angle) * 10);
      int py = cy + (int)(sin(angle) * 10);
      uint16_t col = (i == spinnerFrame) ? COLOR_ACCENT : COLOR_BORDER;
      canvas.fillCircle(px, py, (i == spinnerFrame) ? 3 : 1, col);
    }
  } else if (totalNetworks == 0) {
    // No networks found view
    canvas.setCursor(75, 75);
    canvas.setTextColor(COLOR_SIG_POOR);
    canvas.setTextSize(2);
    canvas.print(F("No Networks"));

    canvas.setCursor(55, 105);
    canvas.setTextColor(COLOR_TEXT_MUTED);
    canvas.setTextSize(1);
    canvas.print(F("Press BOOT button to retry"));
  } else {
    // Draw Network List items
    int visibleCount = min(VISIBLE_ITEMS, totalNetworks - topIndex);
    for (int i = 0; i < VISIBLE_ITEMS; i++) {
      int netIdx = topIndex + i;
      if (netIdx < totalNetworks) {
        drawNetworkItem(i, netIdx);
      }
    }

    // Draw Right Scrollbar if list overflows visible area
    if (totalNetworks > VISIBLE_ITEMS) {
      int barX = 313;
      int barY = LIST_TOP_Y;
      int barH = VISIBLE_ITEMS * ITEM_SPACING - 1;
      
      // Scrollbar track
      canvas.drawFastVLine(barX + 1, barY, barH, COLOR_BORDER);

      // Scrollbar thumb
      int thumbH = max(12, (barH * VISIBLE_ITEMS) / totalNetworks);
      int thumbY = barY + ((barH - thumbH) * topIndex) / (totalNetworks - VISIBLE_ITEMS);
      canvas.fillRoundRect(barX, thumbY, 3, thumbH, 1, COLOR_ACCENT);
    }
  }

  drawFooter();

  // Push off-screen canvas to physical display in 1 atomic SPI pass
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), SCREEN_W, SCREEN_H);
}

// ─── Header Bar Component ────────────────────────────────────────────────────
void drawHeader() {
  canvas.fillRect(0, 0, SCREEN_W, 22, COLOR_HEADER_BG);
  canvas.drawFastHLine(0, 22, SCREEN_W, COLOR_BORDER);

  // WiFi Antenna Icon
  canvas.drawPixel(10, 8, COLOR_ACCENT);
  canvas.drawCircle(10, 11, 4, COLOR_ACCENT);
  canvas.drawCircle(10, 11, 7, COLOR_ACCENT);
  canvas.fillRect(4, 12, 13, 8, COLOR_HEADER_BG); // Mask lower half
  canvas.fillCircle(10, 11, 2, COLOR_ACCENT);

  // Title
  canvas.setCursor(24, 7);
  canvas.setTextColor(COLOR_TEXT_MAIN);
  canvas.setTextSize(1);
  canvas.print(F("WiFi Scanner"));

  // Subtitle / Status on right
  canvas.setCursor(180, 7);
  if (isScanning) {
    canvas.setTextColor(COLOR_ACCENT);
    canvas.print(F("Scanning..."));
  } else {
    canvas.setTextColor(COLOR_TEXT_MUTED);
    canvas.printf("Found: %d nets", totalNetworks);
  }
}

// ─── Footer Bar Component ────────────────────────────────────────────────────
void drawFooter() {
  int y = 153;
  canvas.fillRect(0, y, SCREEN_W, 19, COLOR_FOOTER_BG);
  canvas.drawFastHLine(0, y, SCREEN_W, COLOR_BORDER);

  // Instructions
  canvas.setCursor(8, y + 5);
  canvas.setTextColor(COLOR_TEXT_MUTED);
  canvas.setTextSize(1);
  canvas.print(F("BOOT: Scroll | Hold: Rescan"));

  // Position Counter (e.g. 1-4/12)
  if (totalNetworks > 0) {
    canvas.setCursor(250, y + 5);
    canvas.setTextColor(COLOR_ACCENT);
    canvas.printf("%d-%d / %d", topIndex + 1, min(topIndex + VISIBLE_ITEMS, totalNetworks), totalNetworks);
  }
}

// ─── List Item Card Component ────────────────────────────────────────────────
void drawNetworkItem(int displayRow, int netIndex) {
  int x = 6;
  int y = LIST_TOP_Y + (displayRow * ITEM_SPACING);
  int w = (totalNetworks > VISIBLE_ITEMS) ? 302 : 308;
  int h = ITEM_HEIGHT;

  bool isTopItem = (displayRow == 0); // Highlight top item as active cursor

  uint16_t bgCol = isTopItem ? COLOR_CARD_SEL : COLOR_CARD_BG;
  uint16_t borderCol = isTopItem ? COLOR_BORDER_SEL : COLOR_BORDER;

  // Card background & rounded border
  canvas.fillRoundRect(x, y, w, h, 4, bgCol);
  canvas.drawRoundRect(x, y, w, h, 4, borderCol);

  // Signal Strength Indicator Icon (left side)
  drawSignalBars(x + 8, y + 7, networks[netIndex].rssi);

  // Network Index Number
  canvas.setCursor(x + 32, y + 11);
  canvas.setTextColor(COLOR_TEXT_MUTED);
  canvas.setTextSize(1);
  canvas.printf("%2d.", netIndex + 1);

  // SSID Title (truncated if too long to fit card)
  canvas.setCursor(x + 54, y + 11);
  canvas.setTextColor(isTopItem ? COLOR_TEXT_MAIN : COLOR_TEXT_MAIN);
  canvas.setTextSize(1);

  String dispSSID = networks[netIndex].ssid;
  if (dispSSID.length() > 18) {
    dispSSID = dispSSID.substring(0, 16) + "..";
  }
  canvas.print(dispSSID);

  // Channel Badge
  int chX = x + 195;
  canvas.setCursor(chX, y + 11);
  canvas.setTextColor(COLOR_TEXT_MUTED);
  canvas.printf("Ch%d", networks[netIndex].channel);

  // Security Badge (Right side)
  int secX = x + 235;
  String authStr = getAuthModeString(networks[netIndex].authmode);
  uint16_t badgeCol = networks[netIndex].isSecure ? COLOR_ACCENT_ALT : COLOR_SIG_EXCELLENT;

  canvas.fillRoundRect(secX, y + 7, 58, 16, 3, badgeCol);
  canvas.setCursor(secX + 4, y + 11);
  canvas.setTextColor(COLOR_TEXT_MAIN);
  canvas.print(authStr);
}

// ─── Signal Strength Bars Graphic ──────────────────────────────────────────
void drawSignalBars(int x, int y, int rssi) {
  uint16_t col = getRssiColor(rssi);

  int activeBars = 1;
  if (rssi >= -55)      activeBars = 4;
  else if (rssi >= -68) activeBars = 3;
  else if (rssi >= -78) activeBars = 2;

  // Draw 4 vertical signal bars of increasing height
  for (int b = 0; b < 4; b++) {
    int barH = 3 + (b * 3); // 3px, 6px, 9px, 12px
    int barY = y + (12 - barH);
    int barX = x + (b * 4);
    uint16_t barCol = (b < activeBars) ? col : COLOR_BORDER;
    canvas.fillRect(barX, barY, 3, barH, barCol);
  }
}

// ─── Helper: Get RSSI Signal Color ───────────────────────────────────────────
uint16_t getRssiColor(int rssi) {
  if (rssi >= -55)      return COLOR_SIG_EXCELLENT;
  else if (rssi >= -68) return COLOR_SIG_GOOD;
  else if (rssi >= -78) return COLOR_SIG_FAIR;
  else                  return COLOR_SIG_POOR;
}

// ─── Helper: Authentication Mode String ──────────────────────────────────────
String getAuthModeString(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "EAP";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/3";
    case WIFI_AUTH_WAPI_PSK:        return "WAPI";
    default:                        return "SEC";
  }
}
