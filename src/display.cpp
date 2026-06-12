/**
 * @file display.cpp
 * @brief SSD1306 status display for the Heltec WiFi LoRa 32 V4 (dude-claw fork).
 *
 * Layout (128x64, ArialMT_Plain_10, 13 px pitch):
 *   y=0   device name              NATS marker (right-aligned: "N*" up / "N-" down)
 *   y=13  IP address               WiFi RSSI
 *   y=26  chip temp · free heap · uptime
 *   y=39  metric row 0  (remote, display_print)
 *   y=52  metric row 1  (remote, display_print)
 *
 * Honesty rules: a metric row not written yet shows nothing (absence, not a
 * fake value); a row older than 30 min gains a "(old)" suffix so a dead
 * pusher can't masquerade as live data.
 */

#ifdef DUDECLAW_OLED

#include "display.h"
#include <WiFi.h>
#include <Wire.h>
#include <math.h>
#include "SSD1306Wire.h"
#if !defined(CONFIG_IDF_TARGET_ESP32)
#include "driver/temperature_sensor.h"
extern temperature_sensor_handle_t g_temp_sensor;
#endif

#ifndef DUDECLAW_OLED_SDA
#define DUDECLAW_OLED_SDA 17
#endif
#ifndef DUDECLAW_OLED_SCL
#define DUDECLAW_OLED_SCL 18
#endif
#ifndef DUDECLAW_OLED_RST
#define DUDECLAW_OLED_RST 21
#endif
#ifndef DUDECLAW_VEXT_PIN
#define DUDECLAW_VEXT_PIN 36
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

static SSD1306Wire s_display(0x3c, DUDECLAW_OLED_SDA, DUDECLAW_OLED_SCL);
static bool s_available = false;
static unsigned long s_last_draw = 0;

static char s_metric[DISPLAY_METRIC_ROWS][DISPLAY_METRIC_COLS];
static unsigned long s_metric_ts[DISPLAY_METRIC_ROWS];
static bool s_metric_set[DISPLAY_METRIC_ROWS];

void displayInit() {
    /* Vext rail on (active LOW) — powers the OLED; settle before reset */
    pinMode(DUDECLAW_VEXT_PIN, OUTPUT);
    digitalWrite(DUDECLAW_VEXT_PIN, LOW);
    delay(50);

    /* Panel reset pulse */
    pinMode(DUDECLAW_OLED_RST, OUTPUT);
    digitalWrite(DUDECLAW_OLED_RST, LOW);
    delay(20);
    digitalWrite(DUDECLAW_OLED_RST, HIGH);
    delay(50);

    /* Probe the panel with a real I2C ACK — the lib's init() only allocates
     * a buffer and cannot tell a missing device from a present one. Without
     * this, displayAvailable() would claim a panel on boards that lack one
     * (an error path mapped to a valid-looking value). */
    Wire.begin(DUDECLAW_OLED_SDA, DUDECLAW_OLED_SCL);
    Wire.beginTransmission(0x3c);
    if (Wire.endTransmission() != 0) {
        Serial.printf("Display: no SSD1306 ACK at 0x3c (sda=%d scl=%d) — "
                      "running headless\n",
                      DUDECLAW_OLED_SDA, DUDECLAW_OLED_SCL);
        s_available = false;
        return;
    }

    s_available = s_display.init();
    if (!s_available) {
        Serial.printf("Display: SSD1306 init FAILED (sda=%d scl=%d) — "
                      "running headless\n", DUDECLAW_OLED_SDA, DUDECLAW_OLED_SCL);
        return;
    }
    s_display.flipScreenVertically();
    s_display.setFont(ArialMT_Plain_10);
    s_display.clear();
    s_display.drawString(0, 0, "dude-claw");
    s_display.drawString(0, 13, "WireClaw boot...");
    s_display.display();
    Serial.printf("Display: SSD1306 128x64 up (sda=%d scl=%d rst=%d vext=%d)\n",
                  DUDECLAW_OLED_SDA, DUDECLAW_OLED_SCL,
                  DUDECLAW_OLED_RST, DUDECLAW_VEXT_PIN);
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
    if (now - s_last_draw < DISPLAY_REDRAW_MS) return;
    s_last_draw = now;

    char buf[32];
    s_display.clear();

    /* line 0: name + NATS state */
    s_display.drawString(0, 0, cfg_device_name);
    s_display.setTextAlignment(TEXT_ALIGN_RIGHT);
    s_display.drawString(128, 0, nats_connected ? "N*" : "N-");
    s_display.setTextAlignment(TEXT_ALIGN_LEFT);

    /* line 1: IP + RSSI (or honest disconnect) */
    if (WiFi.status() == WL_CONNECTED) {
        s_display.drawString(0, 13, WiFi.localIP().toString());
        snprintf(buf, sizeof(buf), "%d", (int)WiFi.RSSI());
        s_display.setTextAlignment(TEXT_ALIGN_RIGHT);
        s_display.drawString(128, 13, buf);
        s_display.setTextAlignment(TEXT_ALIGN_LEFT);
    } else {
        s_display.drawString(0, 13, "WiFi: down");
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
    s_display.drawString(0, 26, buf);

    /* metric rows */
    for (int i = 0; i < DISPLAY_METRIC_ROWS; i++) {
        if (!s_metric_set[i]) continue;
        int y = 39 + i * 13;
        if (now - s_metric_ts[i] > METRIC_STALE_MS) {
            snprintf(buf, sizeof(buf), "%.17s(old)", s_metric[i]);
            s_display.drawString(0, y, buf);
        } else {
            s_display.drawString(0, y, s_metric[i]);
        }
    }

    s_display.display();
}

#endif /* DUDECLAW_OLED */
