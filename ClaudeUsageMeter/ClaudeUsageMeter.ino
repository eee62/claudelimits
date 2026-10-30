/*
  Claude Usage Meter
  Seeed Studio XIAO ESP32-S3  +  Waveshare 4.2" e-Paper Module V2 (400x300)

  Shows your Claude Pro limits (5-hour session and weekly) with reset countdowns,
  pace projection and a 24-hour history, plus a clock and local weather.

  Usage numbers come from Claude Code: its status line passes every update to a
  one-line curl command that posts it to this meter over your Wi-Fi.
  After flashing, open http://claude-meter.local in a browser for the setting.

  ---- Arduino IDE ----
  Boards Manager : "esp32" by Espressif Systems -> board "XIAO_ESP32S3"
  Library Manager: "GxEPD2" by Jean-Marc Zingg, "ArduinoJson" by Benoit Blanchon

  ---- Wiring (e-paper -> XIAO) ----
  VCC -> 3V3    GND -> GND    DIN -> D10    CLK -> D8
  CS  -> D3     DC  -> D2     RST -> D1     BUSY -> D0
  Slide the BS switch on the back of the e-paper module to 0 (4-line SPI).
  Plug in the Wi-Fi antenna that came with the XIAO.

  BOOT button: toggle dashboard / setup screen (also does a full, ghost-free refresh).
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <time.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

// ====== CONFIG ======
const char* WIFI_SSID     = "YOUR_WIFI_NAME";      // must be a 2.4 GHz network
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* HOSTNAME      = "claude-meter";        // -> http://claude-meter.local
const char* UNITS         = "auto";                // "auto" (US: F + 12h clock), "metric" or "imperial"
const float LATITUDE      = 0;                     // weather location; leave 0,0 to detect from your IP
const float LONGITUDE     = 0;
const uint8_t ROTATION    = 0;                     // 0 = landscape, 2 = landscape upside down
const int FULL_REFRESH_EVERY = 60;                 // partial refreshes between full anti-ghosting refreshes
const uint32_t WEATHER_EVERY_MS = 15UL * 60 * 1000;

// ====== PINS ======
const int PIN_BUSY = D0, PIN_RST = D1, PIN_DC = D2, PIN_CS = D3;
const int PIN_SCK = D8, PIN_MOSI = D10;
const int PIN_BUTTON = 0;                          // on-board BOOT button

GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT>
    display(GxEPD2_420_GDEY042T81(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));

// ====== STATE ======
const uint32_t SESSION_SECS = 5UL * 3600;
const uint32_t WEEK_SECS    = 7UL * 86400;
const int HIST_N = 96;                             // 24 h of 15-minute samples
const uint8_t NO_DATA = 255;

struct UsageWindow {
  float pct = -1;                                  // 0..100 as reported by Claude Code, -1 = none yet
  uint32_t resetsAt = 0;                           // unix seconds
};

struct Meter {
  UsageWindow session, weekly;
  uint32_t lastSync = 0;                           // unix time of the last usage update
  bool wifiOk = false;
  char ip[16] = "0.0.0.0";
  int rssi = 0;
  int32_t utcOffset = 0;                           // seconds, from the location/weather lookup
  bool imperial = false;
  char city[32] = "";
  bool wxValid = false;
  float temp = 0, tMax = 0, tMin = 0;
  int wxCode = 0;
  uint8_t hist[HIST_N];                            // session usage %, oldest first
  uint8_t page = 0;                                // 0 = dashboard, 1 = setup
};
Meter M;

bool timeValid(time_t t) { return t > 1700000000; }

// What to show now: a window whose reset time has passed is back to 0%.
float effectivePct(const UsageWindow& w, time_t now) {
  if (w.pct < 0) return -1;
  if (timeValid(now) && now >= (time_t)w.resetsAt) return 0;
  return w.pct;
}

struct tm localTime(time_t t) {
  t += M.utcOffset;
  struct tm r;
  gmtime_r(&t, &r);
  return r;
}

const char* DAYS[]   = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

void fmtClock(char* out, size_t n, time_t t) {
  struct tm lt = localTime(t);
  if (M.imperial) snprintf(out, n, "%d:%02d %s", (lt.tm_hour + 11) % 12 + 1, lt.tm_min, lt.tm_hour < 12 ? "AM" : "PM");
  else snprintf(out, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
}

void fmtDate(char* out, size_t n, time_t t) {
  struct tm lt = localTime(t);
  if (M.imperial) snprintf(out, n, "%s %s %d", DAYS[lt.tm_wday], MONTHS[lt.tm_mon], lt.tm_mday);
  else snprintf(out, n, "%s %d %s", DAYS[lt.tm_wday], lt.tm_mday, MONTHS[lt.tm_mon]);
}

void fmtDuration(char* out, size_t n, long secs) {
  long m = secs > 0 ? (secs + 59) / 60 : 0;
  if (m >= 1440) snprintf(out, n, "%ldd %ldh", m / 1440, (m % 1440) / 60);
  else if (m >= 60) snprintf(out, n, "%ldh %02ldm", m / 60, m % 60);
  else snprintf(out, n, "%ldm", m);
}

void fmtAgo(char* out, size_t n, long secs) {
  if (secs < 60) snprintf(out, n, "just now");
  else if (secs < 3600) snprintf(out, n, "%ldm ago", secs / 60);
  else if (secs < 86400) snprintf(out, n, "%ldh ago", secs / 3600);
  else snprintf(out, n, "%ldd ago", secs / 86400);
}

// "17:30" if within a day, otherwise "Thu 17:30"
void fmtResetAt(char* out, size_t n, time_t at, time_t now) {
  char clock[12];
  fmtClock(clock, sizeof clock, at);
  if (at - now < 86400) snprintf(out, n, "Resets %s", clock);
  else snprintf(out, n, "Resets %s %s", DAYS[localTime(at).tm_wday], clock);
}

// Record the session usage once per 15 minutes for the 24 h chart.
void sampleHistory(time_t now) {
  static uint32_t lastSlot = 0;
  if (!timeValid(now)) return;
  uint32_t slot = now / 900;
  if (slot == lastSlot) return;
  uint32_t steps = lastSlot ? min<uint32_t>(slot - lastSlot, HIST_N) : 1;
  lastSlot = slot;
  float p = effectivePct(M.session, now);
  uint8_t v = p < 0 ? NO_DATA : (uint8_t)lroundf(min(p, 100.0f));
  for (uint32_t i = 0; i < steps; i++) {
    memmove(M.hist, M.hist + 1, HIST_N - 1);
    M.hist[HIST_N - 1] = v;
  }
}

// ====== DRAWING ======
const uint16_t INK = GxEPD_BLACK, PAPER = GxEPD_WHITE;
enum Align { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

int16_t textWidth(Adafruit_GFX& g, const char* s, const GFXfont* f) {
  int16_t x1, y1;
  uint16_t w, h;
  g.setFont(f);
  g.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return x1 + w;
}

// Prints s with its baseline at y (top-left at y for the built-in font, f = nullptr).
// Returns the x where the text ends.
int16_t text(Adafruit_GFX& g, int16_t x, int16_t y, const char* s, const GFXfont* f,
             Align a = ALIGN_LEFT, uint16_t color = INK) {
  int16_t w = textWidth(g, s, f);
  if (a == ALIGN_CENTER) x -= w / 2;
  else if (a == ALIGN_RIGHT) x -= w;
  g.setTextColor(color);
  g.setCursor(x, y);
  g.print(s);
  return x + w;
}

void drawHeader(Adafruit_GFX& g, time_t now) {
  g.fillRect(0, 0, g.width(), 30, INK);
  text(g, 10, 22, "Claude Pro", &FreeSansBold12pt7b, ALIGN_LEFT, PAPER);
  char clock[12] = "--:--", date[16] = "";
  if (timeValid(now)) {
    fmtClock(clock, sizeof clock, now);
    fmtDate(date, sizeof date, now);
  }
  int16_t clockW = textWidth(g, clock, &FreeSansBold12pt7b);
  text(g, g.width() - 10, 22, clock, &FreeSansBold12pt7b, ALIGN_RIGHT, PAPER);
  text(g, g.width() - 22 - clockW, 21, date, &FreeSans9pt7b, ALIGN_RIGHT, PAPER);
}

// One limit: big percentage, bar with a pace marker, reset time and projection.
void drawUsage(Adafruit_GFX& g, int16_t y, const char* title, const UsageWindow& w,
               uint32_t windowSecs, time_t now) {
  const int16_t barX = 132, barW = g.width() - 10 - barX, barY = y + 29, barH = 22;
  float pct = effectivePct(w, now);
  bool hasData = pct >= 0;
  bool open = hasData && timeValid(now) && now < (time_t)w.resetsAt;
  long left = open ? (long)(w.resetsAt - now) : 0;
  char buf[64], d1[16], d2[16];

  text(g, 10, y + 14, title, &FreeSansBold9pt7b);
  if (open) {
    fmtResetAt(buf, sizeof buf, w.resetsAt, now);
    text(g, g.width() - 10, y + 14, buf, &FreeSans9pt7b, ALIGN_RIGHT);
  }

  if (hasData) snprintf(buf, sizeof buf, "%d", (int)lroundf(min(pct, 100.0f)));
  else snprintf(buf, sizeof buf, "--");
  int16_t x = text(g, 8, y + 62, buf, &FreeSansBold24pt7b);
  text(g, x + 3, y + 62, "%", &FreeSansBold12pt7b);

  g.drawRect(barX, barY, barW, barH, INK);
  g.drawRect(barX + 1, barY + 1, barW - 2, barH - 2, INK);
  if (hasData) {
    int16_t fill = (barW - 6) * min(pct, 100.0f) / 100;
    if (fill > 0) g.fillRect(barX + 3, barY + 3, fill, barH - 6, INK);
  }
  // Pace marker: where usage would be if spread evenly over the window.
  if (open) {
    float elapsed = constrain(1.0f - (float)left / windowSecs, 0.0f, 1.0f);
    int16_t mx = barX + 3 + (barW - 6) * elapsed;
    g.fillTriangle(mx - 5, barY - 7, mx + 5, barY - 7, mx, barY - 2, INK);
  }

  int16_t ty = y + 71;
  if (!hasData) {
    text(g, barX, ty, "Waiting for Claude Code", &FreeSans9pt7b);
  } else if (!open) {
    text(g, barX, ty, "Reset - full limit available", &FreeSans9pt7b);
  } else if (pct >= 100) {
    fmtDuration(d1, sizeof d1, left);
    const char* badge = "LIMIT REACHED";
    int16_t tw = textWidth(g, badge, &FreeSansBold9pt7b);
    g.fillRect(barX, ty - 14, tw + 8, 19, INK);
    text(g, barX + 4, ty, badge, &FreeSansBold9pt7b, ALIGN_LEFT, PAPER);
    snprintf(buf, sizeof buf, "%s left", d1);
    text(g, barX + tw + 16, ty, buf, &FreeSans9pt7b);
  } else {
    fmtDuration(d1, sizeof d1, left);
    float used = windowSecs - left;
    if (used < windowSecs * 0.1f || pct < 1) {
      snprintf(buf, sizeof buf, "%s left", d1);
    } else {
      float rate = pct / used;                       // % per second so far
      float projected = pct + rate * left;
      if (projected >= 100) {
        fmtDuration(d2, sizeof d2, (long)((100 - pct) / rate));
        snprintf(buf, sizeof buf, "%s left - limit in ~%s", d1, d2);
      } else {
        snprintf(buf, sizeof buf, "%s left - on pace for %d%%", d1, (int)lroundf(projected));
      }
    }
    text(g, barX, ty, buf, &FreeSans9pt7b);
  }
}

void drawHistory(Adafruit_GFX& g, int16_t x, int16_t y, int16_t w, int16_t h) {
  text(g, x, y, "SESSION USAGE, LAST 24 HOURS", nullptr);
  int16_t top = y + 13, base = y + h - 10;
  for (int16_t i = x; i < x + w; i += 4) g.drawPixel(i, top, INK);   // 100% line
  for (int i = 0; i < HIST_N; i++) {
    if (M.hist[i] == NO_DATA) continue;
    int16_t x0 = x + (int32_t)i * w / HIST_N, x1 = x + (int32_t)(i + 1) * w / HIST_N;
    int16_t bh = (int32_t)M.hist[i] * (base - top) / 100;
    if (bh > 0) g.fillRect(x0, base - bh, x1 - x0, bh, INK);
  }
  g.drawFastHLine(x, base, w, INK);
  const char* labels[] = {"-24h", "-18h", "-12h", "-6h", "now"};
  for (int k = 0; k <= 4; k++) {
    int16_t tx = x + k * (w - 1) / 4;
    g.drawFastVLine(tx, base, 3, INK);
    text(g, tx, base + 4, labels[k], nullptr, k == 0 ? ALIGN_LEFT : k == 4 ? ALIGN_RIGHT : ALIGN_CENTER);
  }
}

void drawCloud(Adafruit_GFX& g, int16_t x, int16_t y, uint16_t c) {   // ~34x20 at (x, y)
  g.fillCircle(x + 9, y + 12, 8, c);
  g.fillCircle(x + 18, y + 8, 10, c);
  g.fillCircle(x + 27, y + 12, 7, c);
  g.fillRoundRect(x + 1, y + 11, 33, 10, 4, c);
}

void drawSun(Adafruit_GFX& g, int16_t cx, int16_t cy, int16_t r) {
  g.fillCircle(cx, cy, r, INK);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4;
    g.drawLine(cx + cosf(a) * (r + 3), cy + sinf(a) * (r + 3), cx + cosf(a) * (r + 7), cy + sinf(a) * (r + 7), INK);
  }
}

// 40x40 weather icon from a WMO weather code.
void drawWeatherIcon(Adafruit_GFX& g, int16_t x, int16_t y, int code) {
  if (code == 0) { drawSun(g, x + 20, y + 20, 9); return; }
  if (code <= 2) {
    drawSun(g, x + 14, y + 13, 7);
    drawCloud(g, x + 3, y + 15, PAPER);   // white halo separates cloud from sun
    drawCloud(g, x + 5, y + 17, INK);
    return;
  }
  if (code == 45 || code == 48) {
    for (int i = 0; i < 4; i++) g.fillRoundRect(x + 4 + (i % 2) * 4, y + 8 + i * 7, 28, 3, 1, INK);
    return;
  }
  drawCloud(g, x + 3, y + 4, INK);
  if (code == 3) return;
  bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
  if (code >= 95) {
    g.fillTriangle(x + 20, y + 26, x + 14, y + 34, x + 20, y + 33, INK);
    g.fillTriangle(x + 18, y + 32, x + 23, y + 31, x + 16, y + 40, INK);
  } else if (snow) {
    for (int i = 0; i < 3; i++) g.fillCircle(x + 10 + i * 8, y + 31 + (i % 2) * 4, 2, INK);
  } else {
    for (int i = 0; i < 3; i++) {
      g.drawLine(x + 12 + i * 8, y + 28, x + 9 + i * 8, y + 36, INK);
      g.drawLine(x + 13 + i * 8, y + 28, x + 10 + i * 8, y + 36, INK);
    }
  }
}

const char* weatherText(int code) {
  if (code == 0) return "Clear";
  if (code == 1) return "Mostly clear";
  if (code == 2) return "Partly cloudy";
  if (code == 3) return "Overcast";
  if (code == 45 || code == 48) return "Fog";
  if (code <= 57) return "Drizzle";
  if (code <= 67) return "Rain";
  if (code <= 77) return "Snow";
  if (code <= 82) return "Showers";
  if (code <= 86) return "Snow showers";
  return "Thunderstorm";
}

void drawWeather(Adafruit_GFX& g, int16_t x, int16_t y) {
  if (!M.wxValid) {
    text(g, x, y + 24, M.wifiOk ? "Loading weather..." : "No Wi-Fi", nullptr);
    return;
  }
  drawWeatherIcon(g, x, y, M.wxCode);
  char buf[32];
  snprintf(buf, sizeof buf, "%d", (int)lroundf(M.temp));
  int16_t ex = text(g, x + 46, y + 30, buf, &FreeSansBold18pt7b);
  g.drawCircle(ex + 5, y + 8, 3, INK);                // degree sign
  g.drawCircle(ex + 5, y + 8, 2, INK);
  text(g, ex + 10, y + 30, M.imperial ? "F" : "C", &FreeSansBold12pt7b);
  text(g, x + 46, y + 36, weatherText(M.wxCode), nullptr);
  snprintf(buf, sizeof buf, "H %d  L %d", (int)lroundf(M.tMax), (int)lroundf(M.tMin));
  text(g, x + 46, y + 46, buf, nullptr);
  if (M.city[0]) {
    char city[18];
    strlcpy(city, M.city, sizeof city);                 // trimmed to fit
    text(g, x, y + 56, city, nullptr);
  }
}

void drawFooter(Adafruit_GFX& g, time_t now) {
  int16_t y = g.height() - 14;
  g.drawFastHLine(0, y - 3, g.width(), INK);
  char buf[64], ago[16];
  if (!M.lastSync) snprintf(buf, sizeof buf, "Not synced yet - press BOOT for setup");
  else if (!timeValid(now)) snprintf(buf, sizeof buf, "Synced");
  else { fmtAgo(ago, sizeof ago, now - M.lastSync); snprintf(buf, sizeof buf, "Synced %s", ago); }
  text(g, 10, y + 2, buf, nullptr);
  if (M.wifiOk) snprintf(buf, sizeof buf, "%s.local  %s", HOSTNAME, M.ip);
  else snprintf(buf, sizeof buf, "Wi-Fi not connected");
  text(g, g.width() - 10, y + 2, buf, nullptr, ALIGN_RIGHT);
}

void drawDashboard(Adafruit_GFX& g, time_t now) {
  drawHeader(g, now);
  drawUsage(g, 38, "5-HOUR SESSION", M.session, SESSION_SECS, now);
  g.drawFastHLine(10, 118, g.width() - 20, INK);
  drawUsage(g, 124, "WEEKLY LIMIT", M.weekly, WEEK_SECS, now);
  g.drawFastHLine(10, 204, g.width() - 20, INK);
  drawHistory(g, 10, 212, 228, 66);
  g.drawFastVLine(252, 212, 62, INK);
  drawWeather(g, 264, 214);
  drawFooter(g, now);
}

void drawSetup(Adafruit_GFX& g, time_t now) {
  drawHeader(g, now);
  char buf[96], ago[16];
  text(g, 10, 56, "Connect Claude Code", &FreeSansBold12pt7b);
  text(g, 10, 82, "1. On your computer, open this address:", &FreeSans9pt7b);
  if (M.wifiOk) snprintf(buf, sizeof buf, "http://%s.local", HOSTNAME);
  else snprintf(buf, sizeof buf, "(waiting for Wi-Fi)");
  text(g, 28, 108, buf, &FreeSansBold12pt7b);
  if (M.wifiOk) {
    snprintf(buf, sizeof buf, "or http://%s", M.ip);
    text(g, 28, 128, buf, &FreeSans9pt7b);
  }
  text(g, 10, 154, "2. Copy the status line setting it shows into", &FreeSans9pt7b);
  text(g, 28, 174, "~/.claude/settings.json", &FreeSansBold9pt7b);
  text(g, 10, 198, "3. Send a message in Claude Code.", &FreeSans9pt7b);

  g.drawFastHLine(10, 212, g.width() - 20, INK);
  if (M.wifiOk) snprintf(buf, sizeof buf, "Wi-Fi:      %s (%d dBm)", WIFI_SSID, M.rssi);
  else snprintf(buf, sizeof buf, "Wi-Fi:      NOT CONNECTED to \"%s\" (2.4 GHz only)", WIFI_SSID);
  text(g, 10, 222, buf, nullptr);
  if (M.lastSync && timeValid(now)) { fmtAgo(ago, sizeof ago, now - M.lastSync); snprintf(buf, sizeof buf, "Last sync:  %s", ago); }
  else snprintf(buf, sizeof buf, "Last sync:  never");
  text(g, 10, 236, buf, nullptr);
  text(g, 10, 250, "Clock:      ", nullptr);
  text(g, 82, 250, timeValid(now) ? "synced" : "waiting for internet time", nullptr);
  text(g, 10, 264, "Marker on bars = even pace through the window", nullptr);
  text(g, g.width() / 2, g.height() - 14, "Press BOOT to return to the dashboard", nullptr, ALIGN_CENTER);
}

void drawPage(Adafruit_GFX& g, time_t now) {
  g.fillScreen(PAPER);
  g.setTextWrap(false);
  g.setTextSize(1);
  if (M.page == 1) drawSetup(g, now);
  else drawDashboard(g, now);
}

// ====== NETWORK ======
WebServer server(80);
Preferences prefs;
float geoLat = LATITUDE, geoLon = LONGITUDE;
bool redrawNow = true, dataDirty = false, fullNext = false;
uint32_t pageSinceMs = 0;

String pushCommand(const char* host) {
  return String("curl -s -m 3 -H 'Content-Type: application/json' --data-binary @- http://") + host + "/push";
}

// One line for Claude Code's status line (curl prints the reply).
String statusText(time_t now) {
  float s = effectivePct(M.session, now), w = effectivePct(M.weekly, now);
  if (s < 0 && w < 0) return "Meter: waiting for usage data";
  char out[96], left[24] = "";
  if (s >= 0 && timeValid(now) && now < (time_t)M.session.resetsAt) {
    char d[16];
    fmtDuration(d, sizeof d, M.session.resetsAt - now);
    snprintf(left, sizeof left, " (%s left)", d);
  }
  snprintf(out, sizeof out, "5h %d%%%s | week %d%%", (int)lroundf(max(s, 0.0f)), left, (int)lroundf(max(w, 0.0f)));
  return out;
}

bool applyWindow(UsageWindow& w, JsonVariantConst j) {
  if (j["used_percentage"].isNull()) return false;   // absent until the session's first reply
  float pct = j["used_percentage"].as<float>();
  uint32_t resetsAt = j["resets_at"].as<uint32_t>();
  bool changed = lroundf(pct) != lroundf(w.pct) || resetsAt != w.resetsAt;
  w.pct = pct;
  w.resetsAt = resetsAt;
  return changed;
}

void saveUsage() {
  prefs.putFloat("s_pct", M.session.pct);
  prefs.putUInt("s_rst", M.session.resetsAt);
  prefs.putFloat("w_pct", M.weekly.pct);
  prefs.putUInt("w_rst", M.weekly.resetsAt);
  prefs.putUInt("sync", M.lastSync);
}

void loadUsage() {
  M.session.pct = prefs.getFloat("s_pct", -1);
  M.session.resetsAt = prefs.getUInt("s_rst", 0);
  M.weekly.pct = prefs.getFloat("w_pct", -1);
  M.weekly.resetsAt = prefs.getUInt("w_rst", 0);
  M.lastSync = prefs.getUInt("sync", 0);
}

// POST /push  body = the JSON Claude Code gives its status line.
void handlePush() {
  JsonDocument filter;
  filter["rate_limits"]["five_hour"] = true;
  filter["rate_limits"]["seven_day"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"), DeserializationOption::Filter(filter))) {
    server.send(400, "text/plain", "Meter: could not read status line JSON");
    return;
  }
  time_t now = time(nullptr);
  JsonVariantConst rl = doc["rate_limits"];
  bool changed = applyWindow(M.session, rl["five_hour"]);
  changed |= applyWindow(M.weekly, rl["seven_day"]);
  if (!rl["five_hour"].isNull() || !rl["seven_day"].isNull()) {
    bool first = M.lastSync == 0;
    if (timeValid(now)) M.lastSync = now;
    if (changed || first) saveUsage();
    if (first) { M.page = 0; redrawNow = true; fullNext = true; }
    if (changed) dataDirty = true;
  }
  server.send(200, "text/plain", statusText(now));
}

void handleRoot() {
  time_t now = time(nullptr);
  String local = String(HOSTNAME) + ".local";
  String html =
      "<!doctype html><html><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'><title>Claude Usage Meter</title>"
      "<style>body{font:16px system-ui,sans-serif;max-width:680px;margin:24px auto;padding:0 16px;"
      "color:#111;background:#fff}pre{background:#f1f1f1;padding:12px;white-space:pre-wrap;"
      "word-break:break-all}</style></head><body><h1>Claude Usage Meter</h1><p><b>";
  html += statusText(now);
  html += "</b></p><h2>Connect Claude Code</h2>"
          "<p>Put this in <code>~/.claude/settings.json</code> on the computer where you use Claude Code. "
          "If the file already has settings, add only the <code>\"statusLine\"</code> part inside its braces.</p><pre>";
  html += "{\n  \"statusLine\": {\n    \"type\": \"command\",\n    \"command\": \"" + pushCommand(local.c_str()) + "\"\n  }\n}";
  html += "</pre><p>Restart Claude Code and send a message: the meter updates and Claude Code's status line shows "
          "the same numbers.</p><ul><li>Windows: write <code>curl.exe</code> instead of <code>curl</code>.</li>"
          "<li>Status line stays empty? Replace <code>" + local + "</code> with <code>" + M.ip + "</code>.</li></ul>"
          "</body></html>";
  server.send(200, "text/html", html);
}

bool getJson(const char* url, JsonDocument& doc) {
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  if (!http.begin(url)) return false;
  bool ok = http.GET() == 200 && !deserializeJson(doc, http.getString());
  http.end();
  return ok;
}

// City, coordinates, time zone offset and country (for units), from the public IP.
bool fetchLocation() {
  JsonDocument doc;
  if (!getJson("http://ip-api.com/json/?fields=status,countryCode,city,lat,lon,offset", doc)) return false;
  if (doc["status"] != "success") return false;
  if (LATITUDE == 0 && LONGITUDE == 0) {
    geoLat = doc["lat"];
    geoLon = doc["lon"];
    strlcpy(M.city, doc["city"] | "", sizeof M.city);
  }
  M.utcOffset = doc["offset"] | 0;
  if (strcmp(UNITS, "auto") == 0) M.imperial = doc["countryCode"] == "US";
  return true;
}

bool fetchWeather() {
  char url[300];
  snprintf(url, sizeof url,
           "http://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f"
           "&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min"
           "&timezone=auto&forecast_days=1%s",
           geoLat, geoLon, M.imperial ? "&temperature_unit=fahrenheit" : "");
  JsonDocument doc;
  if (!getJson(url, doc) || doc["current"].isNull()) return false;
  M.temp = doc["current"]["temperature_2m"];
  M.wxCode = doc["current"]["weather_code"];
  M.tMax = doc["daily"]["temperature_2m_max"][0];
  M.tMin = doc["daily"]["temperature_2m_min"][0];
  M.utcOffset = doc["utc_offset_seconds"] | M.utcOffset;   // follows daylight saving changes
  M.wxValid = true;
  return true;
}

void maintainNetwork() {
  static bool mdnsUp = false, geoOk = false;
  static uint32_t lastGeoMs = 0, lastWxMs = 0;
  bool ok = WiFi.status() == WL_CONNECTED;
  if (ok != M.wifiOk) {
    M.wifiOk = ok;
    strlcpy(M.ip, WiFi.localIP().toString().c_str(), sizeof M.ip);
    redrawNow = true;
    if (ok && !mdnsUp) {
      mdnsUp = MDNS.begin(HOSTNAME);
      MDNS.addService("http", "tcp", 80);
    }
  }
  if (!ok) return;
  bool haveCoords = geoLat != 0 || geoLon != 0;
  if (!geoOk && (lastGeoMs == 0 || millis() - lastGeoMs > 60000)) {
    lastGeoMs = millis();
    geoOk = fetchLocation();
    haveCoords = geoLat != 0 || geoLon != 0;
  }
  uint32_t wait = M.wxValid ? WEATHER_EVERY_MS : 60000;
  if (haveCoords && (lastWxMs == 0 || millis() - lastWxMs > wait)) {
    lastWxMs = millis();
    if (fetchWeather()) dataDirty = true;
  }
}

void handleButton() {
  static bool last = HIGH;
  static uint32_t changedMs = 0;
  bool b = digitalRead(PIN_BUTTON);
  if (b == last || millis() - changedMs < 50) return;
  last = b;
  changedMs = millis();
  if (b == LOW) {
    M.page ^= 1;
    pageSinceMs = millis();
    redrawNow = true;
    fullNext = true;
  }
}

// ====== MAIN ======
void redraw(time_t now) {
  static int partials = FULL_REFRESH_EVERY;   // first refresh is full
  M.rssi = WiFi.RSSI();
  bool full = fullNext || partials >= FULL_REFRESH_EVERY;
  if (full) display.setFullWindow();
  else display.setPartialWindow(0, 0, display.width(), display.height());
  display.firstPage();
  do {
    drawPage(display, now);
  } while (display.nextPage());
  display.powerOff();
  partials = full ? 0 : partials + 1;
  fullNext = false;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  memset(M.hist, NO_DATA, sizeof M.hist);
  if (strcmp(UNITS, "imperial") == 0) M.imperial = true;

  prefs.begin("meter");
  loadUsage();
  M.page = M.lastSync ? 0 : 1;   // show setup until the first sync

  SPI.begin(PIN_SCK, -1, PIN_MOSI, -1);
  display.init(0, true, 2, false);   // 2 ms reset pulse for Waveshare boards
  display.setRotation(ROTATION);

  WiFi.setHostname(HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);              // always powered: keep the web server responsive
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  server.on("/", HTTP_GET, handleRoot);
  server.on("/push", HTTP_POST, handlePush);
  server.begin();

  // Give Wi-Fi and the clock a moment so the first screen is complete.
  for (int i = 0; i < 60 && !timeValid(time(nullptr)); i++) delay(250);
  Serial.printf("Wi-Fi %s, IP %s\n", WiFi.status() == WL_CONNECTED ? "connected" : "not connected",
                WiFi.localIP().toString().c_str());
}

void loop() {
  static long drawnMinute = -1;
  static uint32_t lastDrawMs = 0;
  server.handleClient();
  handleButton();
  maintainNetwork();

  time_t now = time(nullptr);
  sampleHistory(now);
  if (M.page == 1 && M.lastSync && millis() - pageSinceMs > 120000) {   // leave setup after 2 min
    M.page = 0;
    redrawNow = true;
  }
  long minute = now / 60;
  if (redrawNow || minute != drawnMinute || (dataDirty && millis() - lastDrawMs > 10000)) {
    redraw(now);
    drawnMinute = minute;
    lastDrawMs = millis();
    redrawNow = dataDirty = false;
  }
}
