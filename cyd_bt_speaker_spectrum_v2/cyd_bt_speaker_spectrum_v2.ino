// =============================================================================
// Bluetooth Speaker + Spectrum Analyser — ESP32-2432S028R (CYD)  — v2
// =============================================================================
// Pair a phone, play music: sound out of the speaker, 16 auto-ranging bars on
// the screen.
//
// v2 change: AnalogAudioStream is now configured with defaultConfig(TX_MODE)
// before begin(). Calling bare begin() leaves the stream uninitialised as an
// output, which is why v1 produced a perfect spectrum and complete silence.
//
// The audio chain on this board is NOT a GPIO straight to the socket:
//   GPIO 26 -> RC filter -> SC8002B 1W amplifier -> 2-pin speaker connector
// MultiOutput fans the A2DP stream to that DAC and to the FFT; neither
// consumes it, so both see every sample.
//
// SETTINGS
//   Tools -> Board            : ESP32 Dev Module
//   Tools -> Partition Scheme : Huge APP
//
// EXPECT IT TO BE QUIET. The ESP32's internal DAC is 8-bit and the amp is 1W
// into 8 ohm. Phone volume up. This is a novelty, not hi-fi.
//
// KNOWN LIMITATION: the ESP32's two DAC channels are GPIO 25 and 26. The amp
// is on 26, but 25 is the touchscreen clock. In stereo the DAC drives both, so
// audio is pushed onto the touch controller's clock line. Harmless while touch
// is unused, but sound and touch cannot coexist without extra work.
// =============================================================================

#include <TFT_eSPI.h>
#include "AudioTools.h"
#include "BluetoothA2DPSink.h"
#include "AudioTools/FFT/AudioRealFFT.h"

// --- Audio ------------------------------------------------------------------
const int SAMPLE_RATE = 44100;     // fixed by A2DP
const int FFT_LEN     = 1024;      // drop to 512 if it reboots on low memory

// --- Display ----------------------------------------------------------------
const int BANDS = 16;
const int BAR_W = 17;
const int GAP   = 2;
const int LEFT  = 8;
const int BASE  = 226;
const int MAXH  = 190;

const bool LEVEL_DEBUG = true;

TFT_eSPI tft = TFT_eSPI();

AudioRealFFT      fft;
AnalogAudioStream dac;             // internal DAC -> RC -> SC8002B -> speaker
MultiOutput       multi;
BluetoothA2DPSink a2dp_sink(multi);

int   barH[BANDS];
int   peakY[BANDS];
float bandMag[BANDS];

float runningMax = 1000.0f;
unsigned long lastDraw = 0;
bool  wasConnected = false;

// =============================================================================
// FFT callback
//
// Bin n covers n * SAMPLE_RATE / FFT_LEN Hz — 43 Hz per bin here. Bands are
// spaced logarithmically because hearing is: 100->200 Hz matters as much as
// 1000->2000.
// =============================================================================
void onFFT(AudioFFTBase& f) {
  for (int i = 0; i < BANDS; i++) bandMag[i] = 0;

  int   bins  = f.size();
  float binHz = (float)SAMPLE_RATE / FFT_LEN;

  const float F_LO = 60.0f, F_HI = 12000.0f;
  const float SPAN = log10(F_HI / F_LO);

  for (int b = 1; b < bins; b++) {
    float hz = b * binHz;
    if (hz < F_LO || hz > F_HI) continue;

    int band = (int)(log10(hz / F_LO) / SPAN * BANDS);
    if (band < 0) band = 0;
    if (band >= BANDS) band = BANDS - 1;

    float m = f.magnitude(b);
    if (m > bandMag[band]) bandMag[band] = m;
  }

  // Auto-range: track the loudest band, decay slowly. A fixed divisor never
  // works, because magnitudes vary hugely with volume and material.
  float frameMax = 0;
  for (int i = 0; i < BANDS; i++) if (bandMag[i] > frameMax) frameMax = bandMag[i];
  if (frameMax > runningMax) runningMax = frameMax;
  else                       runningMax *= 0.995f;      // adapt within a second or two
  if (runningMax < 50.0f) runningMax = 50.0f;           // floor: do not amplify silence

  if (LEVEL_DEBUG && millis() % 2000 < 30) {
    Serial.printf("frameMax %.0f  runningMax %.0f  low %.0f  mid %.0f  high %.0f\n",
                  frameMax, runningMax, bandMag[1], bandMag[8], bandMag[14]);
  }
}

// =============================================================================
// Drawing — only changed pixels, or SPI becomes the frame-rate ceiling
// =============================================================================
void drawBars() {
  for (int i = 0; i < BANDS; i++) {
    int want = (int)(bandMag[i] / runningMax * MAXH);
    if (want > MAXH) want = MAXH;
    if (want < 0)    want = 0;

    if (want < barH[i]) want = barH[i] - max(2, (barH[i] - want) / 3);  // smooth fall

    int x = LEFT + i * (BAR_W + GAP);

    if (want != barH[i]) {
      if (want < barH[i]) {
        tft.fillRect(x, BASE - barH[i], BAR_W, barH[i] - want, TFT_BLACK);
      } else {
        uint16_t c = (want > MAXH * 0.75) ? TFT_RED
                   : (want > MAXH * 0.45) ? TFT_YELLOW : TFT_GREEN;
        tft.fillRect(x, BASE - want, BAR_W, want - barH[i], c);
      }
      barH[i] = want;
    }

    // falling peak marker
    int p = BASE - barH[i] - 3;
    if (p < peakY[i]) {
      tft.drawFastHLine(x, peakY[i], BAR_W, TFT_BLACK);
      peakY[i] = p;
      tft.drawFastHLine(x, peakY[i], BAR_W, TFT_WHITE);
    } else if (peakY[i] < BASE - 3) {
      tft.drawFastHLine(x, peakY[i], BAR_W, TFT_BLACK);
      peakY[i]++;
      tft.drawFastHLine(x, peakY[i], BAR_W, TFT_WHITE);
    }
  }
}

void drawStatus(bool connected) {
  tft.fillRect(236, 6, 78, 16, TFT_BLACK);
  tft.setTextColor(connected ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(236, 10);
  tft.print(connected ? "CONNECTED" : "waiting...");
}

// =============================================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== CYD Bluetooth Speaker + Spectrum v2 ===");

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(8, 6);
  tft.print("SPECTRUM");
  tft.drawFastHLine(0, 28, 320, TFT_DARKGREY);
  tft.drawFastHLine(0, BASE + 2, 320, TFT_DARKGREY);
  drawStatus(false);

  for (int i = 0; i < BANDS; i++) { barH[i] = 0; peakY[i] = BASE - 3; }

  // --- FFT ---
  auto fcfg = fft.defaultConfig();
  fcfg.length          = FFT_LEN;
  fcfg.channels        = 2;
  fcfg.sample_rate     = SAMPLE_RATE;
  fcfg.bits_per_sample = 16;
  fcfg.callback        = &onFFT;
  fft.begin(fcfg);
  Serial.println("FFT started");

  // --- DAC out to the on-board amplifier ---
  // THIS is what v1 got wrong: begin() with no config leaves the stream
  // uninitialised as an output, so it silently produces nothing.
  auto dcfg = dac.defaultConfig(TX_MODE);
  dcfg.sample_rate     = SAMPLE_RATE;
  dcfg.channels        = 2;
  dcfg.bits_per_sample = 16;
  dac.begin(dcfg);
  Serial.println("DAC started (GPIO26 -> SC8002B -> speaker)");

  // --- fan the stream to both, AFTER both are begun ---
  multi.add(dac);
  multi.add(fft);

  a2dp_sink.start("CYD Spectrum");
  Serial.println("pair with 'CYD Spectrum', then select it as audio output");
  Serial.println("phone volume UP - the internal DAC is 8-bit and quiet");
}

void loop() {
  bool c = a2dp_sink.is_connected();
  if (c != wasConnected) { drawStatus(c); wasConnected = c; }

  if (millis() - lastDraw > 33) {          // ~30 fps
    drawBars();
    lastDraw = millis();
  }
}
