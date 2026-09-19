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
  {"Radio Suno 91.7",   "https://playerservices.streamtheworld.com/api/livestream-redirect/SUNO917.mp3"},
  {"Aaha FM Radio",     "http://s2.radio.co/s3801784f1/listen"},
  {"Aural Oldies",      "http://stream.zeno.fm/anrf216cu68uv"},
  {"Chitra Hits",       "http://stream.zeno.fm/dubvcz3rkrhvv"},
  {"Malayalam 98.6",    "https://stream.zeno.fm/512rbf1e3qzuv"}
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

      // Native 16MHz clock keeps the ST7262 internal PLL locked
      cfg.freq_write = 16000000;

// Horizontal timings adjusted to 860 total clocks for solid line latching:
      cfg.hsync_polarity    = 0;
      cfg.hsync_front_porch = 20;   // Increased from 8 to give the line counter breathing room
      cfg.hsync_pulse_width = 8;    // Widened from 4 for an unambiguous sync edge
      cfg.hsync_back_porch  = 32;   // Adjusted back porch


      // Exact ST7262 factory vertical timings (498 lines total)
      cfg.vsync_polarity    = 0;
      cfg.vsync_front_porch = 4;
      cfg.vsync_pulse_width = 4;
      cfg.vsync_back_porch  = 10;

      cfg.pclk_idle_high  = 1;
      cfg.pclk_active_neg = 0;  // Rising-edge data latch prevents sync phase jitter
      cfg.de_idle_high    = 0;

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
LGFX gfx;
Audio audio;

// =============================================================================
// State & Layout Tracking
// =============================================================================
int currentStation = 0;
int currentVolume  = 15; // 0 to 21
String currentTrackTitle = "Connecting...";
unsigned long lastTouchTime = 0;

const int BTN_X = 25;
const int BTN_W = 240;
const int BTN_H = 55;
const int BTN_GAP = 12;

// =============================================================================
// Helper Functions for Hardware Power Management
// =============================================================================
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
  for (int i = 0; i < NUM_STATIONS; i++) {
    int y = 90 + i * (BTN_H + BTN_GAP);
    if (i == currentStation) {
      gfx.fillRoundRect(BTN_X, y, BTN_W, BTN_H, 8, gfx.color565(0, 120, 215));
      gfx.drawRoundRect(BTN_X, y, BTN_W, BTN_H, 8, TFT_WHITE);
    } else {
      gfx.fillRoundRect(BTN_X, y, BTN_W, BTN_H, 8, gfx.color565(38, 38, 44));
      gfx.drawRoundRect(BTN_X, y, BTN_W, BTN_H, 8, gfx.color565(65, 65, 75));
    }
    gfx.setTextColor(TFT_WHITE);
    gfx.setTextSize(2);
    gfx.drawCenterString(stations[i].name, BTN_X + (BTN_W / 2), y + 18);
  }
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

// 1. Station Buttons
  for (int i = 0; i < NUM_STATIONS; i++) {
    int y = 90 + i * (BTN_H + BTN_GAP);
    if (touchX >= BTN_X && touchX <= (BTN_X + BTN_W) && touchY >= y && touchY <= (y + BTN_H)) {
      if (currentStation != i) {
        currentStation = i;
        currentTrackTitle = "Connecting...";

        drawStationButtons();
        drawNowPlayingCard();

        // Signal audioTask on Core 0 to perform the switch safely
        requestedStation = currentStation;
      }
      return;
    }
  }

  // 2. Volume Buttons
  int volY = 385;
  if (touchX >= 360 && touchX <= 435 && touchY >= volY && touchY <= (volY + 60)) {
    if (currentVolume > 0) {
      currentVolume--;
      audio.setVolume(currentVolume);
      updateVolumeDisplay();
    }
    return;
  }
  if (touchX >= 635 && touchX <= 710 && touchY >= volY && touchY <= (volY + 60)) {
    if (currentVolume < 21) {
      currentVolume++;
      audio.setVolume(currentVolume);
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
  for (;;) {
    // Handle station change safely inside the audio thread
    if (requestedStation >= 0) {
      int nextStation = requestedStation;
      requestedStation = -1;
      audio.stopSong();
      vTaskDelay(pdMS_TO_TICKS(50)); // Allow sockets and buffers to flush
      audio.connecttohost(stations[nextStation].url);
    }

    audio.loop();
    taskYIELD();
  }
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

  // Power on speaker power amplifier
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

  audio.setVolume(currentVolume);

  if (WiFi.status() == WL_CONNECTED) {
    audio.connecttohost(stations[currentStation].url);
  }

  // Pin audio playback and decoding to Core 0 (Priority 1: below Wi-Fi driver)
  xTaskCreatePinnedToCore(
    audioTask,         // Function to implement the task
    "audioTask",       // Name of the task
    8192,              // Stack size in words
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

// =============================================================================
// Audio Callbacks
// =============================================================================
void audio_showstreamtitle(const char* info) {
  if (info && strlen(info) > 0) {
    currentTrackTitle = String(info);
    updateMetadataText();
  }
}

void audio_info(const char* info) {
  Serial.printf("[AUDIO] %s\n", info);
}