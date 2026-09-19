// =============================================================================
// Bluetooth Speaker + Spectrum Analyser — CYD with MAX98357A  (v3)
// =============================================================================
// v3 replaces the board's internal 8-bit DAC with an external MAX98357A I2S
// amplifier. The on-board path (GPIO 26 -> RC filter -> SC8002B) exists to
// make beeps, not music: its DAC sine output measured far quieter than a
// square wave on the same hardware, which is the whole reason music through it
// was a whisper. The MAX98357A does its own conversion at full resolution and
// drives 3W, so this is a different category of sound rather than a tweak.
//
// WIRING
//   SPI peripheral connector      Extended input connector
//     IO27 -> BCLK                  3.3V -> VIN
//     IO18 -> LRC                   GND  -> GND
//     IO23 -> DIN
//     IO19 unused                 GAIN and SD left floating:
//                                 9 dB gain, L+R summed to mono
//
// NOTE: IO18/19/23 are the microSD bus. The card slot is unavailable while
// the amplifier is connected. Nothing else contends — the display sits on
// HSPI pins 12-15.
//
// SETTINGS
//   Board: ESP32 Dev Module   PSRAM: Disabled   Flash: 4MB
//   Partition Scheme: Huge APP   (Bluetooth Classic is a large stack)
// =============================================================================

#include <TFT_eSPI.h>
#include "AudioTools.h"
#include "BluetoothA2DPSink.h"
#include "AudioTools/FFT/AudioRealFFT.h"

// --- Amplifier pins ----------------------------------------------------------
#define AMP_BCLK 27
#define AMP_LRC  18
#define AMP_DIN  23

// --- Audio -------------------------------------------------------------------
const int SAMPLE_RATE = 44100;     // fixed by A2DP
const int FFT_LEN     = 1024;      // drop to 512 if it reboots on low memory

// --- Display -----------------------------------------------------------------
const int BANDS = 16;
const int BAR_W = 17;
const int GAP   = 2;
const int LEFT  = 8;
const int BASE  = 226;
const int MAXH  = 190;

const bool LEVEL_DEBUG = true;

TFT_eSPI tft = TFT_eSPI();

AudioRealFFT      fft;
I2SStream         amp;             // MAX98357A, replaces AnalogAudioStream
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

  // Auto-range against a decaying maximum, so quiet and loud material both
  // fill the screen. A fixed divisor never works across different music.
  float frameMax = 0;
  for (int i = 0; i < BANDS; i++) if (bandMag[i] > frameMax) frameMax = bandMag[i];
  if (frameMax > runningMax) runningMax = frameMax;
  else                       runningMax *= 0.995f;
  if (runningMax < 50.0f) runningMax = 50.0f;

  if (LEVEL_DEBUG && millis() % 2000 < 30) {
    Serial.printf("frameMax %.0f  runningMax %.0f  low %.0f  mid %.0f  high %.0f\n",
                  frameMax, runningMax, bandMag[1], bandMag[8], bandMag[14]);
  }
}

// Colour by frequency rather than height: a smooth blue-through-red sweep
// across the 16 bands. Each bar keeps its colour, so the palette tells you
// WHERE in the spectrum you are looking instead of restating the height you
// can already see.
uint16_t bandColour(int i) {
  float t = (float)i / (BANDS - 1);        // 0 at the bass end, 1 at treble
  uint8_t r, g, b;
  if (t < 0.5f) {                          // blue -> cyan -> green
    float u = t * 2.0f;
    r = 0;
    g = (uint8_t)(255 * u);
    b = (uint8_t)(255 * (1.0f - u));
  } else {                                 // green -> yellow -> red
    float u = (t - 0.5f) * 2.0f;
    r = (uint8_t)(255 * u);
    g = (uint8_t)(255 * (1.0f - u * 0.6f));
    b = 0;
  }
  return tft.color565(r, g, b);
}
// =============================================================================
// Drawing — only changed pixels, or SPI becomes the frame-rate ceiling
// =============================================================================
void drawBars() {
  for (int i = 0; i < BANDS; i++) {
    int want = (int)(bandMag[i] / runningMax * MAXH);
    if (want > MAXH) want = MAXH;
    if (want < 0)    want = 0;

    if (want < barH[i]) want = barH[i] - max(2, (barH[i] - want) / 3);

    int x = LEFT + i * (BAR_W + GAP);

    if (want != barH[i]) {
      if (want < barH[i]) {
        tft.fillRect(x, BASE - barH[i], BAR_W, barH[i] - want, TFT_BLACK);
      } else {
        // uint16_t c = (want > MAXH * 0.75) ? TFT_RED
        //            : (want > MAXH * 0.45) ? TFT_YELLOW : TFT_GREEN;
        uint16_t c = bandColour(i);
        tft.fillRect(x, BASE - want, BAR_W, want - barH[i], c);
      }
      barH[i] = want;
    }

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
  Serial.println("\n=== CYD Spectrum + MAX98357A ===");

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

  // --- MAX98357A over I2S ---
  auto acfg = amp.defaultConfig(TX_MODE);
  acfg.sample_rate     = SAMPLE_RATE;
  acfg.channels        = 2;
  acfg.bits_per_sample = 16;
  acfg.pin_bck         = AMP_BCLK;
  acfg.pin_ws          = AMP_LRC;
  acfg.pin_data        = AMP_DIN;
  amp.begin(acfg);
  Serial.printf("I2S amp started: BCLK %d, LRC %d, DIN %d\n",
                AMP_BCLK, AMP_LRC, AMP_DIN);

  // --- fan the stream to both, after both are begun ---
  multi.add(amp);
  multi.add(fft);

  a2dp_sink.start("CYD Spectrum");
  Serial.println("pair with 'CYD Spectrum', then select it as audio output");
  Serial.println("start the phone at LOW volume - 3W into a small speaker is loud");
}

void loop() {
  bool c = a2dp_sink.is_connected();
  if (c != wasConnected) { drawStatus(c); wasConnected = c; }

  if (millis() - lastDraw > 33) {          // ~30 fps
    drawBars();
    lastDraw = millis();
  }
}
