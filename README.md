# esp32-c3-gc9a01-weather-radar
Standalone live Doppler weather radar on an ESP32-C3 Super Mini and 1.28-inch round GC9A01 display.
# ESP32-C3 GC9A01 Doppler Weather Radar

A standalone, circular live Doppler weather radar running on an ESP32-C3 Super Mini and a 1.28" GC9A01 round SPI LCD (240x240). Fetches live radar tiles from the RainViewer API, applies tactical overlay range rings, and plots local interstate corridors.

## Pinout Wiring

| GC9A01 Display Pin | ESP32-C3 Super Mini Pin |
| :--- | :--- |
| **VCC** | 3.3V |
| **GND** | GND |
| **SCL (SCLK)** | GPIO 4 |
| **SDA (MOSI)** | GPIO 3 |
| **DC** | GPIO 10 |
| **CS** | GPIO 1 |
| **RST** | GPIO 0 |

## Arduino IDE Configuration
* **Board:** ESP32C3 Dev Module
* **USB CDC On Boot:** Enabled
* **Flash Frequency:** 80MHz
* **Partition Scheme:** Default 4MB with spiffs

## Required Libraries
* LovyanGFX
* PNGdec
* ArduinoJson
