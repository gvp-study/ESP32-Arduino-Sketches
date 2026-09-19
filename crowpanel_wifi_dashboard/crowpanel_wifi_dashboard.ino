#include <esp_task_wdt.h>
#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include "Audio.h"

// =============================================================================
// Wi-Fi Credentials
// =============================================================================
const char* ssid     = "GVPSHA";
const char* password = "ajayajith";

// =============================================================================
// Station Presets
// =============================================================================
struct Station {
  const char* name;
  const char* url;
};

const Station stations[] = {
  // ── Others ───────────────────────────────────────────────────────────────
  {"Malayalam 98.6",    "https://stream.zeno.fm/512rbf1e3qzuv"},
  {"Aaha FM Radio",     "http://s2.radio.co/s3801784f1/listen"},
  {"Aural Oldies",      "http://stream.zeno.fm/anrf216cu68uv"},
  {"Chitra Hits",       "http://stream.zeno.fm/dubvcz3rkrhvv"},
  // ── Malayalam / Kerala (AIR) ──────────────────────────────────────────────
  // Direct CloudFront URLs — wavespb.com does 302 redirect which ESP32 can't follow
  {"AIR Malayalam",     "https://d1cvqgmbcpg5yn.cloudfront.net/6ff13de7ea9b53d7/6ff13de7ea9b53d7.m3u8"},
  {"AIR Trivandrum",    "https://d1cvqgmbcpg5yn.cloudfront.net/ad3a8436a329e2d6/ad3a8436a329e2d6.m3u8"},
  {"AIR Kochi",         "https://d1cvqgmbcpg5yn.cloudfront.net/70400e7510e87cdf/70400e7510e87cdf.m3u8"},
  {"FM Rainbow Kochi",  "https://d1tmej9eu7kw5c.cloudfront.net/7df6f2a8c3c4d33b/7df6f2a8c3c4d33b.m3u8"},
  {"AIR Kozhikode",     "https://d1cvqgmbcpg5yn.cloudfront.net/8321393de70015fc/8321393de70015fc.m3u8"},
  {"Kozhikode Real FM", "https://d1cvqgmbcpg5yn.cloudfront.net/b69c296065db7627/b69c296065db7627.m3u8"},
  {"Raagam",            "https://airhlspush.pc.cdn.bitgravity.com/httppush/hlspbaudioragam/hlspbaudioragam64kbps.m3u8"}
};

const int NUM_STATIONS = sizeof(stations) / sizeof(stations[0]);

// =============================================================================
// Official CrowPanel 7.0" LGFX Hardware Definition
// =============================================================================
class LGFX : public lgfx::LGFX_Device {
public:
  lgfx::Bus_RGB     _bus_instance;
  lgfx::Panel_RGB   _panel_instance;
  lgfx::Touch_GT911 _touch_instance;

  LGFX(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.panel = &_panel_instance;
      cfg.pin_d0  = GPIO_NUM_21;   // B0
      cfg.pin_d1  = GPIO_NUM_47;   // B1
      cfg.pin_d2  = GPIO_NUM_48;   // B2
      cfg.pin_d3  = GPIO_NUM_45;   // B3
      cfg.pin_d4  = GPIO_NUM_38;   // B4
      cfg.pin_d5  = GPIO_NUM_9;    // G0
      cfg.pin_d6  = GPIO_NUM_10;   // G1
      cfg.pin_d7  = GPIO_NUM_11;   // G2
      cfg.pin_d8  = GPIO_NUM_12;   // G3
      cfg.pin_d9  = GPIO_NUM_13;   // G4
      cfg.pin_d10 = GPIO_NUM_14;   // G5
      cfg.pin_d11 = GPIO_NUM_7;    // R0
      cfg.pin_d12 = GPIO_NUM_17;   // R1
      cfg.pin_d13 = GPIO_NUM_18;   // R2
      cfg.pin_d14 = GPIO_NUM_3;    // R3
      cfg.pin_d15 = GPIO_NUM_46;   // R4

      cfg.pin_henable = GPIO_NUM_42;
      cfg.pin_vsync   = GPIO_NUM_41;
      cfg.pin_hsync   = GPIO_NUM_40;
      cfg.pin_pclk    = GPIO_NUM_39;

      // Native 14MHz pixel clock with tuned porches
      cfg.freq_write = 14000000;

      // Vertical porches
      cfg.vsync_polarity    = 0;
      cfg.vsync_front_porch = 8;
      cfg.vsync_pulse_width = 8;
      cfg.vsync_back_porch  = 16;

      // Horizontal timing
      cfg.hsync_polarity    = 0;
      cfg.hsync_front_porch = 12;
      cfg.hsync_pulse_width = 8;
      cfg.hsync_back_porch  = 8;

      cfg.pclk_idle_high    = 1;
      cfg.pclk_active_neg   = 0;
      cfg.de_idle_high      = 0;

      _bus_instance.config(cfg);
    }

    {
      auto cfg = _panel_instance.config();
      cfg.memory_width  = 800;
      cfg.memory_height = 480;
      cfg.panel_width   = 800;
      cfg.panel_height  = 480;
      cfg.offset_x      = 0;
      cfg.offset_y      = 0;

      // Allocate internal DMA line buffers to insulate LCD stream from PSRAM bursts
      cfg.bus_shared = true;

      _panel_instance.config(cfg);
    }

    {
      auto cfg = _touch_instance.config();
      cfg.x_min      = 0;
      cfg.x_max      = 799;
      cfg.y_min      = 0;
      cfg.y_max      = 479;
      cfg.pin_int    = -1;
      cfg.pin_rst    = -1;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.i2c_port   = 0;
      cfg.pin_sda    = GPIO_NUM_15;
      cfg.pin_scl    = GPIO_NUM_16;
      cfg.freq       = 400000;
      cfg.i2c_addr   = 0x5D;
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }

    _panel_instance.setBus(&_bus_instance);
    setPanel(&_panel_instance);
  }
};

volatile int requestedStation = -1; // -1 means no pending change
volatile int requestedVolume  = -1;

LGFX gfx;
Audio audio;

// =============================================================================
// State & Layout Tracking
// =============================================================================
int currentStation   = 0;
int currentVolume    = 10; // 0 to 21
String currentTrackTitle = "Connecting...";
unsigned long lastTouchTime = 0;

// Viewport / Scrolling Configuration
const int VISIBLE_STATIONS = 5;  // Show 5 items at a time
int listTopIndex           = 0;  // First visible index in view

const int BTN_X   = 25;
const int BTN_W   = 240;
const int BTN_H   = 52;
const int BTN_GAP = 8;

// =============================================================================
// Helpers
// =============================================================================
String clampText(const String& str, unsigned int maxLen) {
  if (str.length() <= maxLen) {
    return str;
  }
  if (maxLen > 3) {
    return str.substring(0, maxLen - 3) + "...";
  }
  return str.substring(0, maxLen);
}

bool i2cScanForAddress(uint8_t address) {
  Wire.beginTransmission(address);
  return (Wire.endTransmission() == 0);
}

void sendI2CCommand(uint8_t command) {
  Wire.beginTransmission(0x30);
  Wire.write(command);
  uint8_t error = Wire.endTransmission();
  if (error == 0) {
    Serial.printf("[BOARD_MCU] Command 0x%02X sent successfully\n", command);
  } else {
    Serial.printf("[BOARD_MCU] Command 0x%02X failed, code: %d\n", command, error);
  }
}

// =============================================================================
// UI Drawing
// =============================================================================
void drawStationButtons() {
  // Draw the 5 visible station slots
  for (int row = 0; row < VISIBLE_STATIONS; row++) {
    int stIdx = listTopIndex + row;
    int y = 90 + row * (BTN_H + BTN_GAP);

    if (stIdx < NUM_STATIONS) {
      if (stIdx == currentStation) {
        gfx.fillRoundRect(BTN_X, y, BTN_W, BTN_H, 8, gfx.color565(0, 120, 215));
        gfx.drawRoundRect(BTN_X, y, BTN_W, BTN_H, 8, TFT_WHITE);
      } else {
        gfx.fillRoundRect(BTN_X, y, BTN_W, BTN_H, 8, gfx.color565(38, 38, 44));
        gfx.drawRoundRect(BTN_X, y, BTN_W, BTN_H, 8, gfx.color565(65, 65, 75));
      }
      gfx.setTextColor(TFT_WHITE);
      gfx.setTextSize(1.8);
      gfx.drawCenterString(stations[stIdx].name, BTN_X + (BTN_W / 2), y + 17);
    } else {
      // Clear trailing empty slots if stations count is small
      gfx.fillRect(BTN_X, y, BTN_W, BTN_H, gfx.color565(18, 18, 22));
    }
  }

  // Draw Viewport Scroll Up / Down Buttons below the station list
  int scrollBtnY = 90 + VISIBLE_STATIONS * (BTN_H + BTN_GAP) + 5; // Y: 395
  int halfW = (BTN_W - 10) / 2; // ~115px each

  // Scroll UP button
  uint16_t upColor = (listTopIndex > 0) ? gfx.color565(55, 55, 68) : gfx.color565(30, 30, 35);
  gfx.fillRoundRect(BTN_X, scrollBtnY, halfW, 50, 8, upColor);
  gfx.drawRoundRect(BTN_X, scrollBtnY, halfW, 50, 8, gfx.color565(80, 80, 95));
  gfx.setTextColor(listTopIndex > 0 ? TFT_WHITE : gfx.color565(90, 90, 100));
  gfx.setTextSize(2.2);
  gfx.drawCenterString("▲ UP", BTN_X + (halfW / 2), scrollBtnY + 16);

  // Scroll DOWN button
  bool canScrollDown = (listTopIndex + VISIBLE_STATIONS < NUM_STATIONS);
  uint16_t dnColor = canScrollDown ? gfx.color565(55, 55, 68) : gfx.color565(30, 30, 35);
  gfx.fillRoundRect(BTN_X + halfW + 10, scrollBtnY, halfW, 50, 8, dnColor);
  gfx.drawRoundRect(BTN_X + halfW + 10, scrollBtnY, halfW, 50, 8, gfx.color565(80, 80, 95));
  gfx.setTextColor(canScrollDown ? TFT_WHITE : gfx.color565(90, 90, 100));
  gfx.setTextSize(2.2);
  gfx.drawCenterString("▼ DN", BTN_X + halfW + 10 + (halfW / 2), scrollBtnY + 16);
}

void drawNowPlayingCard() {
  int cardX = 295;
  int cardY = 90;
  int cardW = 480;
  int cardH = 260;

  gfx.fillRoundRect(cardX, cardY, cardW, cardH, 10, gfx.color565(26, 26, 32));
  gfx.drawRoundRect(cardX, cardY, cardW, cardH, 10, gfx.color565(55, 55, 65));

  gfx.setTextColor(gfx.color565(130, 130, 140));
  gfx.setTextSize(1.8);
  gfx.drawString("CURRENT STATION", cardX + 25, cardY + 20);

  gfx.setTextColor(TFT_CYAN);
  gfx.setTextSize(2.6);
  gfx.drawString(stations[currentStation].name, cardX + 25, cardY + 50);

  gfx.setTextColor(gfx.color565(180, 180, 190));
  gfx.setTextSize(1.8);
  gfx.drawString("STREAM METADATA", cardX + 25, cardY + 105);

  gfx.setTextColor(TFT_WHITE);
  gfx.setTextSize(2);
  gfx.setCursor(cardX + 25, cardY + 135);
  gfx.printf("%s", currentTrackTitle.c_str());
}

void updateMetadataText() {
  int cardX = 295;
  int cardY = 90;
  gfx.fillRect(cardX + 20, cardY + 130, 440, 110, gfx.color565(26, 26, 32));
  gfx.setTextColor(TFT_WHITE);
  gfx.setTextSize(2);
  gfx.setCursor(cardX + 25, cardY + 135);
  gfx.printf("%s", currentTrackTitle.c_str());
}

void drawVolumeControls() {
  int volY = 385;

  // Vol - Button
  gfx.fillRoundRect(360, volY, 75, 60, 8, gfx.color565(48, 48, 56));
  gfx.drawRoundRect(360, volY, 75, 60, 8, gfx.color565(80, 80, 95));
  gfx.setTextColor(TFT_WHITE);
  gfx.setTextSize(3);
  gfx.drawCenterString("-", 397, volY + 18);

  // Volume Display Box
  gfx.fillRoundRect(455, volY, 160, 60, 8, gfx.color565(32, 32, 38));
  gfx.drawRoundRect(455, volY, 160, 60, 8, gfx.color565(60, 60, 70));
  gfx.setTextSize(1.6);
  gfx.setTextColor(gfx.color565(140, 140, 150));
  gfx.drawCenterString("VOLUME", 535, volY + 10);
  gfx.setTextSize(2.6);
  gfx.setTextColor(TFT_GREEN);
  gfx.drawCenterString(String(currentVolume).c_str(), 535, volY + 30);

  // Vol + Button
  gfx.fillRoundRect(635, volY, 75, 60, 8, gfx.color565(48, 48, 56));
  gfx.drawRoundRect(635, volY, 75, 60, 8, gfx.color565(80, 80, 95));
  gfx.setTextColor(TFT_WHITE);
  gfx.setTextSize(3);
  gfx.drawCenterString("+", 672, volY + 18);
}

void updateVolumeDisplay() {
  int volY = 385;
  gfx.fillRect(470, volY + 28, 130, 28, gfx.color565(32, 32, 38));
  gfx.setTextSize(2.6);
  gfx.setTextColor(TFT_GREEN);
  gfx.drawCenterString(String(currentVolume).c_str(), 535, volY + 30);
}

void drawFullUI() {
  gfx.fillScreen(gfx.color565(18, 18, 22));

  // Top Nav Bar
  gfx.fillRect(0, 0, 800, 60, gfx.color565(28, 28, 34));
  gfx.setTextColor(TFT_WHITE);
  gfx.setTextSize(2.2);
  gfx.drawString("CROWPANEL 7\" INTERNET RADIO", 25, 18);

  // WiFi Status
  gfx.setTextSize(1.6);
  if (WiFi.status() == WL_CONNECTED) {
    gfx.setTextColor(TFT_GREEN);
    gfx.drawString("WIFI ONLINE", 680, 22);
  } else {
    gfx.setTextColor(TFT_RED);
    gfx.drawString("WIFI OFFLINE", 670, 22);
  }

  drawStationButtons();
  drawNowPlayingCard();
  drawVolumeControls();
}

// =============================================================================
// Touch Interaction
// =============================================================================
void handleTouch() {
  uint16_t touchX, touchY;
  if (!gfx.getTouch(&touchX, &touchY)) return;

  if (millis() - lastTouchTime < 250) return; // Debounce
  lastTouchTime = millis();

  // 1. Station Button Clicks (Rows 0 to VISIBLE_STATIONS - 1)
  for (int row = 0; row < VISIBLE_STATIONS; row++) {
    int y = 90 + row * (BTN_H + BTN_GAP);
    if (touchX >= BTN_X && touchX <= (BTN_X + BTN_W) && touchY >= y && touchY <= (y + BTN_H)) {
      int selectedIdx = listTopIndex + row;
      if (selectedIdx < NUM_STATIONS && currentStation != selectedIdx) {
        currentStation = selectedIdx;
        currentTrackTitle = "Connecting...";

        drawStationButtons();
        drawNowPlayingCard();

        // Signal audioTask on Core 0 to perform the switch safely
        requestedStation = currentStation;
      }
      return;
    }
  }

  // 2. Viewport Scroll Buttons (Y: 395-445)
  int scrollBtnY = 90 + VISIBLE_STATIONS * (BTN_H + BTN_GAP) + 5;
  int halfW = (BTN_W - 10) / 2;

  // Scroll UP button
  if (touchX >= BTN_X && touchX <= (BTN_X + halfW) && touchY >= scrollBtnY && touchY <= (scrollBtnY + 50)) {
    if (listTopIndex > 0) {
      listTopIndex--;
      drawStationButtons();
    }
    return;
  }

  // Scroll DOWN button
  if (touchX >= (BTN_X + halfW + 10) && touchX <= (BTN_X + BTN_W) && touchY >= scrollBtnY && touchY <= (scrollBtnY + 50)) {
    if (listTopIndex + VISIBLE_STATIONS < NUM_STATIONS) {
      listTopIndex++;
      drawStationButtons();
    }
    return;
  }

  // 3. Volume Buttons
  int volY = 385;
  if (touchX >= 360 && touchX <= 435 && touchY >= volY && touchY <= (volY + 60)) {
    if (currentVolume > 0) {
      currentVolume--;
      requestedVolume = currentVolume; // Hand off to Core 0
      updateVolumeDisplay();
    }
    return;
  }
  if (touchX >= 635 && touchX <= 710 && touchY >= volY && touchY <= (volY + 60)) {
    if (currentVolume < 21) {
      currentVolume++;
      requestedVolume = currentVolume; // Hand off to Core 0
      updateVolumeDisplay();
    }
    return;
  }
}

// =============================================================================
// Core 0 Audio Task
// =============================================================================
TaskHandle_t audioTaskHandle = NULL;

void audioTask(void *pvParameters) {
  // Suppress watchdog on Core 0 for long streaming operations
  #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_task_wdt_delete(NULL);
  #else
    esp_task_wdt_delete(xTaskGetCurrentTaskHandle());
  #endif

  for (;;) {
    if (requestedStation >= 0) {
      int nextStation = requestedStation;
      requestedStation = -1;

      audio.stopSong();
      vTaskDelay(pdMS_TO_TICKS(100));

      audio.connecttohost(stations[nextStation].url);
    }

    if (requestedVolume >= 0) {
      audio.setVolume(requestedVolume);
      requestedVolume = -1;
    }

    audio.loop();
    vTaskDelay(pdMS_TO_TICKS(2)); // Grants LCD DMA priority on the shared bus
  }
}

// =============================================================================
// Audio Callback (ESP32-audioI2S v3.x Unified Event System)
// =============================================================================
void setupAudioCallbacks() {
  Audio::audio_info_callback = [](Audio::msg_t m) {
    if (!m.msg) return;

    Serial.printf("[%s] %s\n", m.s ? m.s : "INFO", m.msg);

    if (m.s != nullptr) {
      String evt = String(m.s);
      if (evt.equalsIgnoreCase("streamtitle") || 
          evt.equalsIgnoreCase("stationname") || 
          evt.equalsIgnoreCase("icy_name") ||
          evt.equalsIgnoreCase("name")) {
        
        if (strlen(m.msg) > 0) {
          currentTrackTitle = clampText(String(m.msg), 40);
          updateMetadataText();
        }
      }
    }
    
    if (currentTrackTitle == "Connecting..." && m.s != nullptr) {
      String evt = String(m.s);
      if (evt.equalsIgnoreCase("bitrate") || evt.equalsIgnoreCase("sample_rate")) {
        currentTrackTitle = "Playing";
        updateMetadataText();
      }
    }
  };
}

// =============================================================================
// Setup & Loop
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== CrowPanel 7\" Radio Boot ===");

  // 1. Initialize Auxiliary Coprocessor & Touch I2C Bus (GPIO 15, 16)
  Wire.begin(15, 16);
  delay(50);

  int retries = 0;
  while (retries < 10) {
    if (i2cScanForAddress(0x30) && i2cScanForAddress(0x5D)) {
      Serial.println("[OK] Detected Board Controller (0x30) and GT911 (0x5D)");
      break;
    }
    sendI2CCommand(250); // Wake display/touch
    pinMode(1, OUTPUT);
    digitalWrite(1, LOW);
    delay(120);
    pinMode(1, INPUT);
    delay(100);
    retries++;
  }

  // Power on speaker amplifier
  sendI2CCommand(248);

  // Turn on screen backlight (0 = max brightness)
  sendI2CCommand(0);
  delay(50);

  // 2. Establish Wi-Fi
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[OK] Wi-Fi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[ERROR] Wi-Fi failed to connect!");
  }

  // 3. Initialize Display & Touch
  gfx.init();
  gfx.initDMA();
  gfx.startWrite();
  drawFullUI();

  // 4. Initialize I2S Audio with working pins: BCLK=5, LRC=6, DOUT=4
  if (!audio.setPinout(5, 6, 4)) {
    Serial.println("[ERROR] Failed to configure I2S pins (5, 6, 4)");
  } else {
    Serial.println("[OK] I2S Pins configured");
  }

  audio.setConnectionTimeout(2500, 2500); // 2.5s connect timeout, 2.5s read timeout
  audio.setVolume(currentVolume);

  setupAudioCallbacks();

  // Hand off initial tuning to audioTask on Core 0 (has an 80KB stack for SSL)
  requestedStation = currentStation;

  xTaskCreatePinnedToCore(
    audioTask,         // Function to implement the task
    "audioTask",       // Name of the task
    20480,             // Stack size in words (80KB)
    NULL,              // Task input parameter
    1,                 // Priority 1
    &audioTaskHandle,  // Task handle
    0                  // Core 0
  );
}

void loop() {
  handleTouch();
  delay(10);
}