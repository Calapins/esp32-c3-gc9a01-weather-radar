#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <PNGdec.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ================= CONFIGURATION =================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Target Location (Decimal degrees)
const float MY_LAT     = 0.0000;        // e.g. 40.7128
const float MY_LON     = 0.0000;        // e.g. -74.0060
const int   RADAR_ZOOM = 7;             // 6 = ~150 mi radius, 7 = ~75 mi radius, 8 = ~35 mi radius

const unsigned long REFRESH_INTERVAL_MS = 60000; // Check every 60 seconds
// =================================================

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01  _panel_instance;
  lgfx::Bus_SPI       _bus_instance;
public:
  LGFX() {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host   = SPI2_HOST;
      cfg.spi_mode   = 0;
      cfg.freq_write = 20000000;
      cfg.pin_sclk   = 4;        // SCL
      cfg.pin_mosi   = 3;        // SDA
      cfg.pin_miso   = -1;
      cfg.pin_dc     = 10;       // DC
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs          = 1;   // CS
      cfg.pin_rst         = 0;   // RST
      cfg.pin_busy        = -1;
      cfg.panel_width     = 240;
      cfg.panel_height    = 240;
      cfg.offset_x        = 0;
      cfg.offset_y        = 0;
      cfg.invert          = true; // True blacks on IPS panel
      cfg.rgb_order       = false;
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};

LGFX lcd;
LGFX_Sprite canvas(&lcd);
PNG png;

bool hasCanvas = false;
bool wasConnected = false;
unsigned long lastRadarCheck = 0;
String currentRadarPath = "";

// Coordinate Conversion (Web-Mercator to Screen Pixels at Zoom 7)
void latLonToPixel(float lat, float lon, int &x, int &y) {
  float world_x = (lon + 180.0) / 360.0 * 32768.0;
  float lat_rad = lat * 0.0174532925;
  float world_y = (1.0 - log(tan(0.785398163 + lat_rad / 2.0)) / 3.1415926535) * 16384.0;

  float center_x = (MY_LON + 180.0) / 360.0 * 32768.0;
  float c_lat_rad = MY_LAT * 0.0174532925;
  float center_y = (1.0 - log(tan(0.785398163 + c_lat_rad / 2.0)) / 3.1415926535) * 16384.0;

  x = (int)(120 + (world_x - center_x));
  y = (int)(120 + (world_y - center_y));
}

// Line-by-line PNG stream decode directly into memory buffer
int pngDrawCallback(PNGDRAW *pDraw) {
  uint16_t lineBuffer[256];
  png.getLineAsRGB565(pDraw, lineBuffer, PNG_RGB565_LITTLE_ENDIAN, 0x000000);

  int y = pDraw->y - 8;
  if (y >= 0 && y < 240) {
    if (hasCanvas) {
      canvas.pushImage(0, y, 240, 1, &lineBuffer[8], (uint16_t)0x0000);
    } else {
      lcd.pushImage(0, y, 240, 1, &lineBuffer[8], (uint16_t)0x0000);
    }
  }
  return 1;
}

// Universal Tactical Reticle, Range Rings, and Compass Overlay
void drawTacticalOverlay(lgfx::LGFXBase* dst) {
  uint16_t green = dst->color565(0, 255, 60);  // Vivid Radar Green

  int cx = 120;
  int cy = 120;

  // Range rings (~25 mi, 50 mi, 75 mi at Zoom 7)
  dst->drawCircle(cx, cy, 38, green);
  dst->drawCircle(cx, cy, 76, green);
  dst->drawCircle(cx, cy, 114, green);

  // Crosshairs
  dst->drawFastHLine(8, cy, 224, green);
  dst->drawFastVLine(cx, 8, 224, green);

  // Cardinal Headings
  dst->setTextColor(TFT_WHITE);
  dst->setTextDatum(textdatum_t::middle_center);
  dst->drawString("N", cx, 14);
  dst->drawString("S", cx, 226);
  dst->drawString("W", 14, cy);
  dst->drawString("E", 226, cy);

  // Center Station Marker
  dst->fillCircle(cx, cy, 3, TFT_CYAN);
  dst->drawCircle(cx, cy, 4, TFT_BLACK);
}

void updateRadar() {
  Serial.println("\n[Radar] Checking RainViewer for frame updates...");
  String radarPath = "";

  {
    WiFiClientSecure secureClient;
    secureClient.setInsecure();
    HTTPClient http;
    http.begin(secureClient, "https://api.rainviewer.com/public/weather-maps.json");

    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, http.getStream());
      if (!err) {
        JsonArray past = doc["radar"]["past"].as<JsonArray>();
        if (past.size() > 0) {
          radarPath = past[past.size() - 1]["path"].as<String>();
          Serial.printf("[Radar] Latest server frame: %s\n", radarPath.c_str());
        }
      }
    }
    http.end();
  }

  if (radarPath.length() == 0) {
    Serial.println("[Radar] Timestamp fetch failed.");
    return;
  }

  // Timestamp Cache Check: Skip download and redraw if the radar frame hasn't changed
  if (radarPath == currentRadarPath) {
    Serial.println("[Radar] Frame unchanged. Skipping redraw.");
    return;
  }

  // Fetch tile over standard HTTP (port 80)
  char url[256];
  snprintf(url, sizeof(url), "http://tilecache.rainviewer.com%s/256/%d/%.4f/%.4f/2/1_1.png",
           radarPath.c_str(), RADAR_ZOOM, MY_LAT, MY_LON);

  Serial.printf("[Radar] New frame detected. Downloading: %s\n", url);

  WiFiClient plainClient;
  HTTPClient http;
  http.begin(plainClient, url);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    int totalBytes = http.getSize();
    if (totalBytes > 0 && totalBytes < 50000) {
      uint8_t* pngBuffer = (uint8_t*)malloc(totalBytes);
      if (pngBuffer) {
        WiFiClient* stream = http.getStreamPtr();
        int bytesRead = 0;
        unsigned long timeout = millis();

        while (http.connected() && (bytesRead < totalBytes) && (millis() - timeout < 5000)) {
          size_t availableBytes = stream->available();
          if (availableBytes) {
            int read = stream->readBytes(&pngBuffer[bytesRead], availableBytes);
            bytesRead += read;
            timeout = millis();
          }
          delay(1);
        }

        lgfx::LGFXBase* dst = hasCanvas ? (lgfx::LGFXBase*)&canvas : (lgfx::LGFXBase*)&lcd;

        dst->fillScreen(TFT_BLACK);

        int rc = png.openRAM(pngBuffer, bytesRead, pngDrawCallback);
        if (rc == PNG_SUCCESS) {
          dst->startWrite();
          png.decode(NULL, 0);
          png.close();
          dst->endWrite();
        }
        free(pngBuffer);

        drawTacticalOverlay(dst);

        // Blit finished buffer to physical display in ~40ms (Flicker-free)
        if (hasCanvas) {
          canvas.pushSprite(0, 0);
        }

        currentRadarPath = radarPath;
        Serial.println("[Radar] Screen redraw complete.");
      }
    }
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- GC9A01 Weather Radar Starting ---");

  lcd.init();
  lcd.setRotation(0);

  // Allocate 8-bit (RGB332) sprite: 240x240x1 = 57.6 KB (Fits internal SRAM)
  canvas.setColorDepth(8);
  hasCanvas = (canvas.createSprite(240, 240) != nullptr);
  if (hasCanvas) {
    Serial.println("[Setup] Double-buffer sprite initialized (57.6 KB).");
  } else {
    Serial.println("[Setup] Sprite allocation failed. Using direct draw.");
  }

  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(lcd.color565(0, 255, 60));
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.drawString("Connecting Wi-Fi...", 120, 120);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 80) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[Wi-Fi] Connected! IP: %s | RSSI: %d dBm\n", 
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    wasConnected = true;
    lcd.fillScreen(TFT_BLACK);
    lcd.drawString("Fetching Radar...", 120, 120);
    updateRadar();
    lastRadarCheck = millis();
  } else {
    Serial.println("\n[Wi-Fi] Handshake pending; continuing in background loop.");
    lcd.fillScreen(TFT_BLACK);
    lcd.setTextColor(TFT_YELLOW);
    lcd.drawString("Mesh Syncing...", 120, 120);
  }
}

void loop() {
  bool isConnected = (WiFi.status() == WL_CONNECTED);

  if (isConnected && !wasConnected) {
    Serial.printf("\n[Wi-Fi] Connected in background! IP: %s\n", WiFi.localIP().toString().c_str());
    wasConnected = true;
    lcd.fillScreen(TFT_BLACK);
    lcd.drawString("Fetching Radar...", 120, 120);
    updateRadar();
    lastRadarCheck = millis();
  } else if (!isConnected) {
    wasConnected = false;
    delay(500);
    return;
  }

  if (millis() - lastRadarCheck >= REFRESH_INTERVAL_MS) {
    lastRadarCheck = millis();
    updateRadar();
  }
  delay(100);
}