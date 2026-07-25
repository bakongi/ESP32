# ESP32

Sketches for the **Waveshare ESP32-C6-LCD-1.47** (ESP32-C6FH8, 8 MB flash, 1.47" ST7789 IPS 172×320, WS2812 RGB LED, microSD slot).

Board pinout, thermal/graphics notes and the Arduino CLI build commands are collected in [AGENTS.md](AGENTS.md).

## Projects

| Project | What it does |
| :--- | :--- |
| [PC_Hardware_Monitor_ESP32C6](PC_Hardware_Monitor_ESP32C6/) | 3-page real-time PC monitor (CPU / GPU / ESP32 stats) with animated gauges at 30 FPS, brightness menu stored in NVS, plus a native Windows tray app that streams sensor data over USB serial. See its [README](PC_Hardware_Monitor_ESP32C6/README.md). |
| [EVE_Eyes_ESP32C6](EVE_Eyes_ESP32C6/) | Emotive animated eyes engine — blinking, saccades and emotion presets rendered flicker-free through off-screen `GFXcanvas16` buffers. Port of [playfultechnology/esp32-eyes](https://github.com/playfultechnology/esp32-eyes). |
| [WiFi_Scanner_ESP32C6](WiFi_Scanner_ESP32C6/) | Scrollable Wi-Fi network list with RSSI bars and encryption type. BOOT button: short press scrolls, long press rescans. |

Each folder is a self-contained Arduino sketch (folder name matches the `.ino`), so it can be opened directly in the Arduino IDE.

## Building

Board settings: `ESP32C6 Dev Module`, 8 MB flash, 160 MHz, **USB CDC On Boot enabled**.

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32c6:CDCOnBoot=cdc <SketchFolder>
arduino-cli upload -p COM12 --fqbn esp32:esp32:esp32c6:CDCOnBoot=cdc <SketchFolder>
```

Required libraries: `Adafruit GFX`, `Adafruit ST7735/ST7789`, and the ESP32 Arduino core 3.x.

## License

[MIT](LICENSE)
