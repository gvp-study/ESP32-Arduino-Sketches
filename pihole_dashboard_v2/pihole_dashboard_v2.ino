// =============================================================================
// Pi-hole v6 Dashboard — ESP32-2432S028R (CYD)  — v2
// =============================================================================
// Field names below are CONFIRMED against a live 6.4.3 summary response, not
// guessed. Raw sample the layout was built from:
//   queries.total 21118, blocked 1288, percent_blocked 6.099,
//   unique_domains 1943, forwarded 2476, cached 15968
//   clients.active 3, clients.total 3
//   gravity.domains_being_blocked 80040, last_update 1789266304
//
// Layout (320x240 landscape):
//   Header        y 0..28
//   Hero: % blocked + cache hit rate   y 32..104
//   Bar: blocked / cached / forwarded  y 110..126
//   Four stats in a 2x2 grid           y 136..228
// =============================================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include "secrets.h"        // WIFI_SSID, WIFI_PASSWORD, PIHOLE_PASSWORD

const char* PIHOLE_HOST = "10.0.0.9";
const unsigned long REFRESH_MS = 30000UL;
const bool SHOW_RAW_JSON = false;

TFT_eSPI tft = TFT_eSPI();
String sid = "";

struct Stats {
  long  total, blocked, forwarded, cached, uniqueDomains;
  float pctBlocked;
  long  gravityDomains, gravityUpdated;
  long  clientsActive, clientsTotal;
  bool  valid;
};

Stats prev;
bool firstDraw = true;
unsigned long lastFetch = 0;

// =============================================================================
// Network
// =============================================================================

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries++ < 40) { delay(500); Serial.print("."); }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 110);
    tft.print("WiFi FAILED");
    while (true) delay(1000);
  }
  Serial.print("IP: "); Serial.println(WiFi.localIP());
}

bool authenticate() {
  HTTPClient http;
  http.begin(String("http://") + PIHOLE_HOST + "/api/auth");
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(String("{\"password\":\"") + PIHOLE_PASSWORD + "\"}");
  String body = http.getString();
  http.end();

  if (code != 200) { Serial.printf("auth -> HTTP %d\n", code); return false; }

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  sid = doc["session"]["sid"].as<String>();
  return sid.length() > 0;
}

bool fetchStats(Stats& s) {
  if (sid.length() == 0 && !authenticate()) return false;

  HTTPClient http;
  http.begin(String("http://") + PIHOLE_HOST + "/api/stats/summary");
  http.addHeader("X-FTL-SID", sid);
  int code = http.GET();
  String body = http.getString();
  http.end();

  if (code == 401) { sid = ""; return authenticate() ? fetchStats(s) : false; }
  if (code != 200) { Serial.printf("stats -> HTTP %d\n", code); return false; }
  if (SHOW_RAW_JSON) { Serial.println(body); }

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;

  JsonObject q = doc["queries"];
  s.total         = q["total"]           | 0;
  s.blocked       = q["blocked"]         | 0;
  s.forwarded     = q["forwarded"]       | 0;
  s.cached        = q["cached"]          | 0;
  s.uniqueDomains = q["unique_domains"]  | 0;
  s.pctBlocked    = q["percent_blocked"] | 0.0f;

  s.gravityDomains = doc["gravity"]["domains_being_blocked"] | 0;
  s.gravityUpdated = doc["gravity"]["last_update"]           | 0;
  s.clientsActive  = doc["clients"]["active"]                | 0;
  s.clientsTotal   = doc["clients"]["total"]                 | 0;
  s.valid = true;
  return true;
}

// =============================================================================
// Helpers
// =============================================================================

// 21118 -> "21.1k" so big counters fit the cell
void human(long v, char* out, size_t n) {
  if (v >= 1000000)     snprintf(out, n, "%.1fM", v / 1000000.0);
  else if (v >= 10000)  snprintf(out, n, "%.1fk", v / 1000.0);
  else                  snprintf(out, n, "%ld", v);
}

// Blocklist age. Stale gravity means `pihole -g` has not run.
void gravityAge(long updatedUnix, char* out, size_t n) {
  if (updatedUnix <= 0) { snprintf(out, n, "?"); return; }
  time_t now = time(nullptr);
  if (now < 1000000000) { snprintf(out, n, "--"); return; }   // NTP not set yet
  long days = (now - updatedUnix) / 86400L;
  snprintf(out, n, "%ldd", days);
}

float cacheRate(Stats& s) {
  return s.total > 0 ? (100.0f * s.cached / s.total) : 0.0f;
}

// =============================================================================
// Display
// =============================================================================

const int CELL_X[2] = { 8, 166 };
const int CELL_Y[2] = { 140, 188 };

void drawSkeleton() {
  tft.fillScreen(TFT_BLACK);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(8, 6);
  tft.print("PI-HOLE");
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(118, 12);
  tft.print(PIHOLE_HOST);
  tft.drawFastHLine(0, 28, 320, TFT_DARKGREY);

  // Hero labels
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(8,   34);  tft.print("BLOCKED");
  tft.setCursor(166, 34);  tft.print("CACHE HIT");

  tft.drawFastHLine(0, 132, 320, TFT_DARKGREY);

  // Grid labels
  tft.setCursor(CELL_X[0], CELL_Y[0]);      tft.print("QUERIES");
  tft.setCursor(CELL_X[1], CELL_Y[0]);      tft.print("UNIQUE DOMAINS");
  tft.setCursor(CELL_X[0], CELL_Y[1]);      tft.print("BLOCKLIST / AGE");
  tft.setCursor(CELL_X[1], CELL_Y[1]);      tft.print("CLIENTS");
}

// Proportional bar: blocked | cached | forwarded
void drawBar(Stats& s) {
  const int X = 8, Y = 110, W = 304, H = 14;
  tft.fillRect(X, Y, W, H, TFT_BLACK);
  if (s.total <= 0) return;

  int wB = (int)((float)s.blocked   / s.total * W);
  int wC = (int)((float)s.cached    / s.total * W);
  int wF = (int)((float)s.forwarded / s.total * W);

  int x = X;
  tft.fillRect(x, Y, wB, H, TFT_RED);     x += wB;
  tft.fillRect(x, Y, wC, H, TFT_GREEN);   x += wC;
  tft.fillRect(x, Y, wF, H, TFT_BLUE);    x += wF;
  if (x < X + W) tft.fillRect(x, Y, X + W - x, H, TFT_DARKGREY);  // in-progress
  tft.drawRect(X, Y, W, H, TFT_DARKGREY);
}

void drawValues(Stats& s) {
  char a[16], b[16];

  // Hero: percent blocked
  tft.fillRect(8, 46, 150, 40, TFT_BLACK);
  uint16_t pc = (s.pctBlocked > 20) ? TFT_GREEN
              : (s.pctBlocked > 5)  ? TFT_YELLOW : TFT_ORANGE;
  tft.setTextColor(pc, TFT_BLACK);
  tft.setTextSize(4);
  tft.setCursor(8, 46);
  snprintf(a, sizeof(a), "%.1f%%", s.pctBlocked);
  tft.print(a);

  // Hero: cache hit rate
  tft.fillRect(166, 46, 150, 40, TFT_BLACK);
  float cr = cacheRate(s);
  tft.setTextColor(cr > 50 ? TFT_GREEN : TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(4);
  tft.setCursor(166, 46);
  snprintf(a, sizeof(a), "%.0f%%", cr);
  tft.print(a);

  // Sub-line under the heroes: raw counts
  tft.fillRect(8, 90, 304, 12, TFT_BLACK);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(8, 90);
  human(s.blocked, a, sizeof(a));
  tft.printf("%s blocked", a);
  tft.setCursor(166, 90);
  human(s.forwarded, a, sizeof(a));
  tft.printf("%s forwarded", a);

  drawBar(s);

  // Grid values
  tft.setTextSize(2);

  tft.fillRect(CELL_X[0], CELL_Y[0] + 12, 150, 20, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(CELL_X[0], CELL_Y[0] + 12);
  human(s.total, a, sizeof(a));
  tft.print(a);

  tft.fillRect(CELL_X[1], CELL_Y[0] + 12, 146, 20, TFT_BLACK);
  tft.setCursor(CELL_X[1], CELL_Y[0] + 12);
  human(s.uniqueDomains, a, sizeof(a));
  tft.print(a);

  tft.fillRect(CELL_X[0], CELL_Y[1] + 12, 150, 20, TFT_BLACK);
  tft.setCursor(CELL_X[0], CELL_Y[1] + 12);
  human(s.gravityDomains, a, sizeof(a));
  gravityAge(s.gravityUpdated, b, sizeof(b));
  tft.printf("%s %s", a, b);

  tft.fillRect(CELL_X[1], CELL_Y[1] + 12, 146, 20, TFT_BLACK);
  tft.setCursor(CELL_X[1], CELL_Y[1] + 12);
  tft.printf("%ld/%ld", s.clientsActive, s.clientsTotal);
}

void setDot(uint16_t c) { tft.fillCircle(306, 14, 6, c); }

// =============================================================================

void doFetch() {
  setDot(TFT_CYAN);
  Stats s;
  memset(&s, 0, sizeof(s));

  if (fetchStats(s)) {
    if (firstDraw) { drawSkeleton(); firstDraw = false; }
    drawValues(s);
    prev = s;
    setDot(TFT_GREEN);
    Serial.printf("%.1f%% blocked | cache %.0f%% | %ld queries | %ld clients\n",
                  s.pctBlocked, cacheRate(s), s.total, s.clientsActive);
  } else {
    setDot(TFT_RED);
    Serial.println("fetch failed");
  }
  lastFetch = millis();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Pi-hole Dashboard v2 ===");

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(30, 110);
  tft.print("Connecting...");

  connectWiFi();

  // NTP, so blocklist age can be computed from gravity.last_update
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  doFetch();
}

void loop() {
  if (millis() - lastFetch >= REFRESH_MS) doFetch();
  delay(500);
}
