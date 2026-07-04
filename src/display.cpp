/**
 * @file display.cpp
 * @brief SSD1306 status display (optional; WIRECLAW_OLED builds only).
 *
 * Page 1 — STATUS (128x64, ArialMT_Plain_10, 13 px pitch):
 *   y=0   device name              [tier] NATS marker (right-aligned)
 *   y=13  IP address               WiFi RSSI
 *   y=26  chip temp · free heap · uptime
 *   y=39  metric row 0  (remote, display_print)  } replaced by the alert
 *   y=52  metric row 1  (remote, display_print)  } banner while one is up
 *
 * WIRECLAW_OLED_PAGES builds add two LOCAL pages, cycled by the PRG/USER
 * button (WIRECLAW_OLED_BUTTON; auto-home to STATUS after 60 s idle):
 *   Page 2 — EARS: LoRa/BLE listener counters + an rx-per-bucket sparkline,
 *   rendered entirely from on-board state — keeps working through brain loss.
 *   Page 3 — SELF: min-free-heap + chip-temp sparklines, battery, uptime.
 *
 * Honesty rules (v1 rules kept, v2 rules added):
 *   - a metric row never written shows nothing; >30 min old gains "(old)";
 *   - the brain-tier glyph (display_tier) decays ON THE DEVICE'S OWN CLOCK:
 *     <15 min → 'F'/'L'/'R'; <30 min → glyph+'?'; older → "SOLO". A dead
 *     brain cannot leave a fresh-looking claim on the glass;
 *   - the alert banner (display_alert) clears ONLY on an explicit empty
 *     push; its age ticks locally, so losing NATS mid-alert leaves it up
 *     with the age still climbing (unresolved as far as anyone proved);
 *   - a sparkline with <2 closed buckets says "warming" — never a fake
 *     flat line; buckets close on observed monotonic millis, never
 *     wall-clock (RTC-less discipline);
 *   - a NAN chip temp is skipped, never recorded as 0.
 */

#ifdef WIRECLAW_OLED

#include "display.h"
#include <WiFi.h>
#include <Wire.h>
#include <math.h>
#include "SSD1306Wire.h"
#ifdef WIRECLAW_OLED_PAGES
#include "lora_ears.h"
#include "ble_scan.h"
#include "tools.h" /* batteryReadVolts */
#endif
#if !defined(CONFIG_IDF_TARGET_ESP32)
#include "driver/temperature_sensor.h"
extern temperature_sensor_handle_t g_temp_sensor;
#endif

#ifndef WIRECLAW_OLED_SDA
#define WIRECLAW_OLED_SDA 17
#endif
#ifndef WIRECLAW_OLED_SCL
#define WIRECLAW_OLED_SCL 18
#endif
#ifndef WIRECLAW_OLED_RST
#define WIRECLAW_OLED_RST 21
#endif
#ifndef WIRECLAW_OLED_VEXT
#define WIRECLAW_OLED_VEXT 36
#endif

#define DISPLAY_REDRAW_MS 1000UL
#define METRIC_STALE_MS (30UL * 60UL * 1000UL)

extern char cfg_device_name[32];

/* Chip temp, same read pattern as devices.cpp. Returns NAN when the sensor
 * is unavailable — the draw path shows "--", never a fake number. */
static float chipTempRead() {
#if !defined(CONFIG_IDF_TARGET_ESP32)
    if (g_temp_sensor) {
        float t = 0.0f;
        if (temperature_sensor_get_celsius(g_temp_sensor, &t) == ESP_OK)
            return t;
    }
#endif
    return NAN;
}

static SSD1306Wire s_display(0x3c, WIRECLAW_OLED_SDA, WIRECLAW_OLED_SCL);
static bool s_available = false;
static unsigned long s_last_draw = 0;

static char s_metric[DISPLAY_METRIC_ROWS][DISPLAY_METRIC_COLS];
static unsigned long s_metric_ts[DISPLAY_METRIC_ROWS];
static bool s_metric_set[DISPLAY_METRIC_ROWS];

/*============================================================================
 * v2 pages (WIRECLAW_OLED_PAGES)
 *============================================================================*/

#ifdef WIRECLAW_OLED_PAGES

#define DISPLAY_PAGES 3
#ifndef WIRECLAW_OLED_BUCKET_MINUTES
#define WIRECLAW_OLED_BUCKET_MINUTES 60 /* sparkline bucket width */
#endif
#define SPARK_BUCKETS 24
#define BUCKET_MS (WIRECLAW_OLED_BUCKET_MINUTES * 60UL * 1000UL)
#define TIER_FRESH_MS (15UL * 60UL * 1000UL)
#define TIER_STALE_MS (30UL * 60UL * 1000UL)
#define PAGE_HOME_MS (60UL * 1000UL)
#define JITTER_PERIOD_MS (10UL * 60UL * 1000UL)
#define BAT_SAMPLE_MS (5UL * 60UL * 1000UL)

static int s_page = 0;
static unsigned long s_page_touch_ms = 0;
static int s_yoff = 0; /* 1-px burn-in relief, toggles every 10 min */

static char s_tier = 0; /* 'F'/'L'/'R'; 0 = never pushed (drawn as absence) */
static unsigned long s_tier_ms = 0;

static bool s_alert_set = false;
static char s_alert_cls[DISPLAY_METRIC_COLS];
static long s_alert_age0_s = 0;
static unsigned long s_alert_ms = 0;

/* Ring of CLOSED buckets; head = next slot to overwrite. */
typedef struct {
    uint16_t v[SPARK_BUCKETS];
    uint8_t head;
    uint8_t filled;
} SparkRing;

static SparkRing s_rx_ring;   /* LoRa packets heard per bucket */
static SparkRing s_heap_ring; /* min free heap (kB) seen in bucket */
static SparkRing s_temp_ring; /* chip temp at bucket close, stored C+100 */
static unsigned long s_bucket_open_ms = 0; /* 0 = no bucket opened yet */
static unsigned long s_rx_at_open = 0;
static uint32_t s_heap_min_kb = 0;

static float s_bat_v = NAN; /* sampled only while SELF shows (divider draws) */
static unsigned long s_bat_ms = 0;

#ifdef WIRECLAW_OLED_BUTTON
static bool s_btn_last_up = true;
static unsigned long s_btn_edge_ms = 0;
#endif

#define YO(y) ((y) + s_yoff)

static void sparkPush(SparkRing *r, uint16_t v) {
    r->v[r->head] = v;
    r->head = (uint8_t)((r->head + 1) % SPARK_BUCKETS);
    if (r->filled < SPARK_BUCKETS) r->filled++;
}

static uint16_t sparkAt(const SparkRing *r, int i) {
    /* i = 0..filled-1, oldest first */
    return r->v[(r->head + SPARK_BUCKETS - r->filled + i) % SPARK_BUCKETS];
}

/* Min-max scaled polyline. <2 closed buckets: say "warming" — an honest
 * "not enough observed time yet", never a fake flat line. */
static void sparkDraw(const SparkRing *r, int x, int y, int w, int h) {
    if (r->filled < 2) {
        s_display.drawString(x, y + (h - 10) / 2, "warming");
        return;
    }
    uint16_t mn = 0xFFFF, mx = 0;
    for (int i = 0; i < r->filled; i++) {
        uint16_t v = sparkAt(r, i);
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    int span = (mx > mn) ? (int)(mx - mn) : 1;
    int px = -1, py = -1;
    for (int i = 0; i < r->filled; i++) {
        int cx = x + (i * (w - 1)) / (SPARK_BUCKETS - 1);
        int cy = y + (h - 1) - ((int)(sparkAt(r, i) - mn) * (h - 1)) / span;
        if (px >= 0)
            s_display.drawLine(px, py, cx, cy);
        else
            s_display.setPixel(cx, cy);
        px = cx;
        py = cy;
    }
}

/* Close a bucket when its observed time has elapsed. Monotonic millis only —
 * an NTP step cannot stretch or shrink a bucket. */
static void sparkSample(unsigned long now) {
    uint32_t heap_kb = ESP.getFreeHeap() / 1024;
    if (s_bucket_open_ms == 0) { /* first tick opens the first bucket */
        s_bucket_open_ms = now ? now : 1;
        s_rx_at_open = loraEarsHeardCount();
        s_heap_min_kb = heap_kb;
        return;
    }
    if (heap_kb < s_heap_min_kb) s_heap_min_kb = heap_kb;
    if (now - s_bucket_open_ms < BUCKET_MS) return;

    unsigned long heard = loraEarsHeardCount();
    unsigned long d = heard - s_rx_at_open;
    sparkPush(&s_rx_ring, d > 0xFFFF ? 0xFFFF : (uint16_t)d);
    sparkPush(&s_heap_ring,
              s_heap_min_kb > 0xFFFF ? 0xFFFF : (uint16_t)s_heap_min_kb);
    float t = chipTempRead();
    if (!isnan(t)) { /* missing sensor is skipped, never logged as 0 */
        long c = lroundf(t) + 100; /* store C+100 so sub-zero fits unsigned */
        if (c < 0) c = 0;
        if (c > 0xFFFF) c = 0xFFFF;
        sparkPush(&s_temp_ring, (uint16_t)c);
    }
    s_bucket_open_ms = now ? now : 1;
    s_rx_at_open = heard;
    s_heap_min_kb = ESP.getFreeHeap() / 1024;
}

/* "F" fresh · "F?" aging · "SOLO" stale · "" never-pushed (absence). */
static void tierText(char *out, size_t n, unsigned long now) {
    if (!s_tier) {
        if (n) out[0] = '\0';
        return;
    }
    unsigned long age = now - s_tier_ms;
    if (age < TIER_FRESH_MS)
        snprintf(out, n, "%c", s_tier);
    else if (age < TIER_STALE_MS)
        snprintf(out, n, "%c?", s_tier);
    else
        snprintf(out, n, "SOLO");
}

bool displaySetTier(char tier) {
    if (tier != 'F' && tier != 'L' && tier != 'R') return false;
    s_tier = tier;
    s_tier_ms = millis();
    s_last_draw = 0;
    return true;
}

bool displaySetAlert(const char *cls, long age_s) {
    if (!cls || !cls[0]) { /* explicit clear — the ONLY way a banner drops */
        s_alert_set = false;
        s_alert_cls[0] = '\0';
        s_last_draw = 0;
        return true;
    }
    snprintf(s_alert_cls, sizeof(s_alert_cls), "%s", cls);
    s_alert_age0_s = (age_s < 0) ? 0 : age_s;
    s_alert_ms = millis();
    s_alert_set = true;
    s_page = 0; /* an alert yanks the glass back to the status page */
    s_page_touch_ms = s_alert_ms;
    s_last_draw = 0;
    return true;
}

static void pollButton(unsigned long now) {
#ifdef WIRECLAW_OLED_BUTTON
    bool up = digitalRead(WIRECLAW_OLED_BUTTON) != 0;
    if (s_btn_last_up && !up && (now - s_btn_edge_ms) > 50UL) {
        s_btn_edge_ms = now;
        s_page = (s_page + 1) % DISPLAY_PAGES;
        s_page_touch_ms = now;
        s_last_draw = 0; /* page flips redraw immediately */
    }
    s_btn_last_up = up;
#endif
    if (s_page != 0 && (now - s_page_touch_ms) > PAGE_HOME_MS) {
        s_page = 0;
        s_last_draw = 0;
    }
}

/* Right side of row 0 on every page: tier glyph + NATS marker. */
static void drawHeaderRight(unsigned long now, bool nats_connected) {
    char tier[8], buf[16];
    tierText(tier, sizeof(tier), now);
    if (tier[0])
        snprintf(buf, sizeof(buf), "%s %s", tier,
                 nats_connected ? "N*" : "N-");
    else
        snprintf(buf, sizeof(buf), "%s", nats_connected ? "N*" : "N-");
    s_display.setTextAlignment(TEXT_ALIGN_RIGHT);
    s_display.drawString(128, YO(0), buf);
    s_display.setTextAlignment(TEXT_ALIGN_LEFT);
}

static void drawAlertBanner(unsigned long now) {
    char buf[32];
    long age = s_alert_age0_s + (long)((now - s_alert_ms) / 1000UL);
    s_display.fillRect(0, YO(39), 128, 25);
    s_display.setColor(BLACK);
    s_display.drawString(2, YO(39), s_alert_cls);
    if (age < 3600)
        snprintf(buf, sizeof(buf), "alert %ldm%02lds", age / 60, age % 60);
    else
        snprintf(buf, sizeof(buf), "alert %ldh%02ldm", age / 3600,
                 (age % 3600) / 60);
    s_display.drawString(2, YO(52), buf);
    s_display.setColor(WHITE);
}

static void drawEarsPage(unsigned long now, bool nats_connected) {
    char buf[40];
    s_display.drawString(0, YO(0), "EARS 2/3");
    drawHeaderRight(now, nats_connected);

    if (loraEarsAvailable()) {
        snprintf(buf, sizeof(buf), "rx %lu  heard %lds ago",
                 loraEarsHeardCount(), loraEarsHeardAgeS());
        s_display.drawString(0, YO(13), buf);
    } else {
        s_display.drawString(0, YO(13), "LoRa: not listening");
    }

    snprintf(buf, sizeof(buf), "rx/%dm", (int)WIRECLAW_OLED_BUCKET_MINUTES);
    s_display.drawString(0, YO(26), buf);
    sparkDraw(&s_rx_ring, 34, YO(26), 94, 24);

    long bage = bleAdvAgeS();
    if (bage >= 0)
        snprintf(buf, sizeof(buf), "tx %lu %s  ble %d/%lds",
                 loraMeshTxCount(),
                 loraMeshTxGuardRemainS() > 0 ? "wait" : "rdy",
                 bleUniqCount(), bage);
    else
        snprintf(buf, sizeof(buf), "tx %lu %s  ble n/a", loraMeshTxCount(),
                 loraMeshTxGuardRemainS() > 0 ? "wait" : "rdy");
    s_display.drawString(0, YO(52), buf);
}

static void drawSelfPage(unsigned long now, bool nats_connected) {
    char buf[40];
    s_display.drawString(0, YO(0), "SELF 3/3");
    drawHeaderRight(now, nats_connected);

    /* Battery is sampled only while this page shows — the switched divider
     * draws battery current, so an unwatched page must not poll it. */
    if (s_bat_ms == 0 || (now - s_bat_ms) > BAT_SAMPLE_MS) {
        s_bat_v = batteryReadVolts(NULL);
        s_bat_ms = now ? now : 1;
    }
    float t = chipTempRead();
    char tstr[8], bstr[12];
    if (isnan(t))
        snprintf(tstr, sizeof(tstr), "--C");
    else
        snprintf(tstr, sizeof(tstr), "%.0fC", t);
    if (isnan(s_bat_v))
        snprintf(bstr, sizeof(bstr), "bat n/a");
    else
        snprintf(bstr, sizeof(bstr), "bat %.2fV", s_bat_v);
    snprintf(buf, sizeof(buf), "%uk  %s  %s", ESP.getFreeHeap() / 1024,
             tstr, bstr);
    s_display.drawString(0, YO(13), buf);

    s_display.drawString(0, YO(26), "hp");
    sparkDraw(&s_heap_ring, 20, YO(26), 108, 12);
    s_display.drawString(0, YO(39), "tC");
    sparkDraw(&s_temp_ring, 20, YO(39), 108, 12);

    if (WiFi.status() == WL_CONNECTED)
        snprintf(buf, sizeof(buf), "up %lum  wifi %d", now / 60000UL,
                 (int)WiFi.RSSI());
    else
        snprintf(buf, sizeof(buf), "up %lum  wifi down", now / 60000UL);
    s_display.drawString(0, YO(52), buf);
}

#else /* WIRECLAW_OLED without WIRECLAW_OLED_PAGES */

#define YO(y) (y)

/* Pages not built: honest refusals so the tools report the truth. */
bool displaySetTier(char) { return false; }
bool displaySetAlert(const char *, long) { return false; }

#endif /* WIRECLAW_OLED_PAGES */

/*============================================================================
 * Core (v1) surface
 *============================================================================*/

void displayInit() {
    /* Vext rail on (active LOW) — powers the OLED; settle before reset */
    pinMode(WIRECLAW_OLED_VEXT, OUTPUT);
    digitalWrite(WIRECLAW_OLED_VEXT, LOW);
    delay(50);

    /* Panel reset pulse */
    pinMode(WIRECLAW_OLED_RST, OUTPUT);
    digitalWrite(WIRECLAW_OLED_RST, LOW);
    delay(20);
    digitalWrite(WIRECLAW_OLED_RST, HIGH);
    delay(50);

    /* Probe the panel with a real I2C ACK — the lib's init() only allocates
     * a buffer and cannot tell a missing device from a present one. Without
     * this, displayAvailable() would claim a panel on boards that lack one
     * (an error path mapped to a valid-looking value). */
    Wire.begin(WIRECLAW_OLED_SDA, WIRECLAW_OLED_SCL);
    Wire.beginTransmission(0x3c);
    if (Wire.endTransmission() != 0) {
        Serial.printf("Display: no SSD1306 ACK at 0x3c (sda=%d scl=%d) — "
                      "running headless\n",
                      WIRECLAW_OLED_SDA, WIRECLAW_OLED_SCL);
        s_available = false;
        return;
    }

    s_available = s_display.init();
    if (!s_available) {
        Serial.printf("Display: SSD1306 init FAILED (sda=%d scl=%d) — "
                      "running headless\n", WIRECLAW_OLED_SDA, WIRECLAW_OLED_SCL);
        return;
    }
#if defined(WIRECLAW_OLED_PAGES) && defined(WIRECLAW_OLED_BUTTON)
    /* Page button. Input-only on the strapping pin is safe post-boot. */
    pinMode(WIRECLAW_OLED_BUTTON, INPUT_PULLUP);
#endif
    s_display.flipScreenVertically();
    s_display.setFont(ArialMT_Plain_10);
    s_display.clear();
    s_display.drawString(0, 0, cfg_device_name[0] ? cfg_device_name : "WireClaw");
    s_display.drawString(0, 13, "booting...");
    s_display.display();
    Serial.printf("Display: SSD1306 128x64 up (sda=%d scl=%d rst=%d vext=%d)\n",
                  WIRECLAW_OLED_SDA, WIRECLAW_OLED_SCL,
                  WIRECLAW_OLED_RST, WIRECLAW_OLED_VEXT);
}

bool displayAvailable() { return s_available; }

void displayShowSetupMode() {
    if (!s_available) return;
    s_display.clear();
    s_display.drawString(0, 0, "SETUP MODE");
    s_display.drawString(0, 13, "join WiFi:");
    s_display.drawString(0, 26, "WireClaw-Setup");
    s_display.drawString(0, 39, "portal: 192.168.4.1");
    s_display.display();
}

bool displayPrintRow(int row, const char *text) {
    if (row < 0 || row >= DISPLAY_METRIC_ROWS) return false;
    if (!text || !text[0]) {
        s_metric_set[row] = false;
        s_metric[row][0] = '\0';
        return true;
    }
    snprintf(s_metric[row], DISPLAY_METRIC_COLS, "%s", text);
    s_metric_ts[row] = millis();
    s_metric_set[row] = true;
    s_last_draw = 0; /* redraw on the next tick, not up to 1 s later */
    return true;
}

void displayTick(bool nats_connected) {
    if (!s_available) return;
    unsigned long now = millis();
#ifdef WIRECLAW_OLED_PAGES
    /* Every call: button feel and bucket closure must not wait for redraw. */
    pollButton(now);
    sparkSample(now);
#endif
    if (now - s_last_draw < DISPLAY_REDRAW_MS) return;
    s_last_draw = now;

    char buf[40];
    s_display.clear();

#ifdef WIRECLAW_OLED_PAGES
    s_yoff = ((now / JITTER_PERIOD_MS) & 1UL) ? 1 : 0;
    if (s_page == 1) {
        drawEarsPage(now, nats_connected);
        s_display.display();
        return;
    }
    if (s_page == 2) {
        drawSelfPage(now, nats_connected);
        s_display.display();
        return;
    }
#endif

    /* ---- STATUS page ---- */

    /* line 0: name + (tier +) NATS state */
    s_display.drawString(0, YO(0), cfg_device_name);
#ifdef WIRECLAW_OLED_PAGES
    drawHeaderRight(now, nats_connected);
#else
    s_display.setTextAlignment(TEXT_ALIGN_RIGHT);
    s_display.drawString(128, YO(0), nats_connected ? "N*" : "N-");
    s_display.setTextAlignment(TEXT_ALIGN_LEFT);
#endif

    /* line 1: IP + RSSI (or honest disconnect) */
    if (WiFi.status() == WL_CONNECTED) {
        s_display.drawString(0, YO(13), WiFi.localIP().toString());
        snprintf(buf, sizeof(buf), "%d", (int)WiFi.RSSI());
        s_display.setTextAlignment(TEXT_ALIGN_RIGHT);
        s_display.drawString(128, YO(13), buf);
        s_display.setTextAlignment(TEXT_ALIGN_LEFT);
    } else {
        s_display.drawString(0, YO(13), "WiFi: down");
    }

    /* line 2: temp / heap / uptime */
    float t = chipTempRead();
    if (isnan(t)) {
        snprintf(buf, sizeof(buf), "--C  %uk  up %lum",
                 ESP.getFreeHeap() / 1024, now / 60000UL);
    } else {
        snprintf(buf, sizeof(buf), "%.0fC  %uk  up %lum",
                 t, ESP.getFreeHeap() / 1024, now / 60000UL);
    }
    s_display.drawString(0, YO(26), buf);

#ifdef WIRECLAW_OLED_PAGES
    /* An active alert owns the metric-row region until explicitly cleared. */
    if (s_alert_set) {
        drawAlertBanner(now);
        s_display.display();
        return;
    }
#endif

    /* metric rows */
    for (int i = 0; i < DISPLAY_METRIC_ROWS; i++) {
        if (!s_metric_set[i]) continue;
        int y = YO(39 + i * 13);
        if (now - s_metric_ts[i] > METRIC_STALE_MS) {
            snprintf(buf, sizeof(buf), "%.17s(old)", s_metric[i]);
            s_display.drawString(0, y, buf);
        } else {
            s_display.drawString(0, y, s_metric[i]);
        }
    }

    s_display.display();
}

#endif /* WIRECLAW_OLED */
