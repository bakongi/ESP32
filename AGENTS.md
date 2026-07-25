# ESP32-C6 Board Specification & Hardware Guide

## Board Overview
- **Board Model:** Waveshare ESP32-C6-LCD-1.47
- **Detected Serial Port:** `COM12`

---

## Hardware Specifications

| Property | Value / Detail |
| :--- | :--- |
| **Microcontroller Chip** | ESP32-C6FH8 (QFN32, Revision v0.2) |
| **Processor Architecture** | 32-bit RISC-V Single-Core (up to 160 MHz) + Low-Power (LP) Core |
| **Flash Memory** | 8 MB (Embedded Flash, ID: `0x20` / `0x4017`) |
| **Crystal Frequency** | 40 MHz |
| **Wireless Interfaces** | Wi-Fi 6 (802.11ax 2.4 GHz), Bluetooth 5 (LE), IEEE 802.15.4 (Zigbee / Thread / Matter) |
| **USB Interface** | Built-in USB-Serial / JTAG Controller (CDC) |
| **MAC Address** | `AC:EB:E6:1D:D5:68` *(Extended: `AC:EB:E6:FF:FE:1D:D5:68`)* |

---

## Onboard Peripherals & GPIO Pinout

### 1. LCD Display (1.47" IPS, 172x320, ST7789 Controller)
- **SPI MOSI:** GPIO 6
- **SPI SCLK:** GPIO 7
- **SPI MISO:** GPIO 5 (or GPIO 13)
- **CS (Chip Select):** GPIO 14
- **DC (Data/Command):** GPIO 15
- **RST (Reset):** GPIO 21
- **BL (Backlight Control):** GPIO 22 *(Supports PWM via `analogWrite(22, 190)` (~75% brightness) to reduce thermal heating by ~30%)*
- **Recommended SPI Frequency:** **27 MHz** (`27000000` Hz) — official Waveshare ST7789 ribbon clock spec.

### 2. Onboard Buttons
- **BOOT Button:** **GPIO 9** (Active LOW, `INPUT_PULLUP`). *(Pulls strapping pin GPIO 9 to GND when pressed)*.
- **RESET Button:** Dedicated hardware Chip Reset pin (`CHIP_PU` / `EN`).

### 3. MicroSD Card Slot (TF Card)
- **CS (Chip Select):** GPIO 4
- **SPI MISO:** GPIO 5
- **SPI MOSI:** GPIO 6
- **SPI SCLK:** GPIO 7

### 4. RGB LED (WS2812 / Addressable LED)
- **DATA Pin:** GPIO 8
- **Hardware Color Order:** **GRB** (Green-Red-Blue). When using `neopixelWrite(PIN_RGB_LED, ...)` on ESP32 Arduino Core 3.x, parameters must be mapped to GRB order: `neopixelWrite(PIN_RGB_LED, green, red, blue)`.

---

## Software & Graphics Best Practices

### 1. Zero-Flicker Animations (Double Buffering)
- **Problem:** Clearing the entire screen with `fillScreen(COLOR_BLACK)` every frame causes severe visible flickering/strobe over SPI.
- **Solution:** Render graphic components into an off-screen `GFXcanvas16` sprite buffer in RAM (e.g. 72x104 px) and push to display in **1 atomic pass** via `tft.drawRGBBitmap(x, y, canvas.getBuffer(), w, h)`.

### 2. Thermal Management
- **Problem:** Compact board layout gets warm (38°C–48°C) due to LDO 3.3V regulator, 160MHz CPU, and 100% LCD backlight LEDs.
- **Solution:** Use PWM dimming on GPIO 22 (`analogWrite(PIN_LCD_BL, 190)` ~75% brightness) to significantly reduce heat while maintaining bright colors.

### 3. Onboard WS2812 RGB LED Color Mapping (GRB Channel Order)
- **Problem:** Direct calls like `neopixelWrite(PIN_RGB_LED, r, g, b)` produce incorrect colors (e.g. Green displays as Red, Blue displays as Magenta/Purple) because physical WS2812 chips expect **GRB** byte order.
- **Solution:** Wrap RGB calls in a helper function `setRgbLed(r, g, b)`:
  ```cpp
  void setRgbLed(uint8_t r, uint8_t g, uint8_t b) {
    neopixelWrite(PIN_RGB_LED, g, r, b); // Swaps Green and Red channels for GRB hardware
  }
  ```

---

## Development Environment & Tooling Location

- **Arduino IDE Core Tools (esptool.exe):**
  `C:\Users\Constantine\AppData\Local\Arduino15\packages\esp32\tools\esptool_py\5.3.1\esptool.exe`

- **CLI Builder (arduino-cli.exe):**
  `C:\Users\Constantine\AppData\Local\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe`

### Recommended Arduino IDE Configuration
- **Board:** `ESP32C6 Dev Module`
- **Flash Size:** `8MB (64Mb)`
- **CPU Frequency:** `160MHz`
- **USB CDC On Boot:** `Enabled`

### CLI Build & Upload Commands
```powershell
# Compile Sketch (Enable USB CDC On Boot)
& "C:\Users\Constantine\AppData\Local\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" compile --fqbn esp32:esp32:esp32c6:CDCOnBoot=cdc "J:\ESP32\PC_Hardware_Monitor_ESP32C6"

# Flash to Board via COM12
& "C:\Users\Constantine\AppData\Local\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" upload -p COM12 --fqbn esp32:esp32:esp32c6:CDCOnBoot=cdc "J:\ESP32\PC_Hardware_Monitor_ESP32C6"
```
