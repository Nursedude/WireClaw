/**
 * @file anomaly.cpp
 * @brief Edge anomaly witness — EW mean/variance z-scores (see anomaly.h).
 *
 * Update rule (exponentially weighted, West 1979 form):
 *   diff = x - mean;  incr = ALPHA * diff
 *   mean += incr;     var = (1 - ALPHA) * (var + diff * incr)
 * ALPHA = 1/1440 at ~1-minute samples gives a ~1-day time constant: the
 * baseline follows slow drift (seasonal temp, WiFi environment) while a
 * fast deviation scores high before the baseline absorbs it.
 *
 * Per-feature stddev floors stop a near-constant feature (flat heap) from
 * turning measurement noise into huge z-scores.
 */

#ifdef WIRECLAW_ANOMALY

#include "anomaly.h"
#include <WiFi.h>
#include <math.h>
#include "lora_ears.h"
#if !defined(CONFIG_IDF_TARGET_ESP32)
#include "driver/temperature_sensor.h"
extern temperature_sensor_handle_t g_temp_sensor;
#endif

#ifndef WIRECLAW_ANOMALY_SAMPLE_MS
#define WIRECLAW_ANOMALY_SAMPLE_MS 60000UL
#endif
/* Samples before a feature's z-score participates (~1 h at 1/min). */
#ifndef WIRECLAW_ANOMALY_WARMUP
#define WIRECLAW_ANOMALY_WARMUP 60
#endif
#define ANOMALY_ALPHA (1.0f / 1440.0f)

typedef struct {
    const char *name;
    float mean;
    float var;
    uint32_t n;        /* samples absorbed */
    float last;        /* last raw value */
    float z;           /* last |z| (valid when n >= warmup) */
    bool seen;         /* observed at least once since boot */
} Feature;

static Feature s_feat[] = {
    {"heap_kb", 0, 0, 0, 0, 0, false},
    {"temp_c",  0, 0, 0, 0, 0, false},
    {"rssi",    0, 0, 0, 0, 0, false},
    {"rx_min",  0, 0, 0, 0, 0, false},
};
#define F_HEAP 0
#define F_TEMP 1
#define F_RSSI 2
#define F_RX   3
#define N_FEAT 4

/* stddev floors: below these, deviation is measurement noise, not signal */
static const float FLOORS[N_FEAT] = {1.0f, 0.5f, 2.0f, 0.5f};

static unsigned long s_last_sample = 0;
static unsigned long s_rx_at_last = 0;
static bool s_rx_primed = false;

static void update(int i, float x) {
    Feature *f = &s_feat[i];
    f->last = x;
    f->seen = true;
    if (f->n == 0) {
        f->mean = x;
        f->var = 0;
        f->n = 1;
        return;
    }
    /* score against the CURRENT baseline before absorbing the sample —
     * otherwise the anomaly dilutes the very reference it deviates from */
    float sd = sqrtf(f->var);
    if (sd < FLOORS[i]) sd = FLOORS[i];
    f->z = fabsf(x - f->mean) / sd;
    float diff = x - f->mean;
    float incr = ANOMALY_ALPHA * diff;
    f->mean += incr;
    f->var = (1.0f - ANOMALY_ALPHA) * (f->var + diff * incr);
    f->n++;
}

static float chipTemp() {
#if !defined(CONFIG_IDF_TARGET_ESP32)
    if (g_temp_sensor) {
        float t = 0.0f;
        if (temperature_sensor_get_celsius(g_temp_sensor, &t) == ESP_OK)
            return t;
    }
#endif
    return NAN;
}

void anomalyInit() {
    for (int i = 0; i < N_FEAT; i++) {
        s_feat[i].mean = 0; s_feat[i].var = 0; s_feat[i].n = 0;
        s_feat[i].last = 0; s_feat[i].z = 0; s_feat[i].seen = false;
    }
    s_last_sample = 0;
    s_rx_primed = false;
    Serial.printf("Anomaly: witness up (sample %lus, warmup %d)\n",
                  WIRECLAW_ANOMALY_SAMPLE_MS / 1000UL,
                  (int)WIRECLAW_ANOMALY_WARMUP);
}

void anomalyTick() {
    unsigned long now = millis();
    if (s_last_sample && now - s_last_sample < WIRECLAW_ANOMALY_SAMPLE_MS)
        return;
    s_last_sample = now ? now : 1;

    update(F_HEAP, (float)(ESP.getFreeHeap() / 1024));

    float t = chipTemp();
    if (!isnan(t)) update(F_TEMP, t); /* unobservable -> skipped, never 0 */

    if (WiFi.status() == WL_CONNECTED)
        update(F_RSSI, (float)WiFi.RSSI());

    if (loraEarsAvailable()) {
        unsigned long heard = loraEarsHeardCount();
        if (s_rx_primed)
            update(F_RX, (float)(heard - s_rx_at_last));
        s_rx_at_last = heard;
        s_rx_primed = true;
    }
}

void anomalyStats(char *out, int out_len) {
    /* Warmed-up = every feature that has EVER been observed reached the
     * warm-up count. Features never observed on this build (no radio, no
     * sensor) are exempt — absence of a surface is not an unwarmed one. */
    uint32_t min_n = 0xFFFFFFFF;
    bool any = false;
    for (int i = 0; i < N_FEAT; i++) {
        if (!s_feat[i].seen) continue;
        any = true;
        if (s_feat[i].n < min_n) min_n = s_feat[i].n;
    }
    if (!any) {
        snprintf(out, out_len, "anomaly_score: -1 (learning, no samples yet)");
        return;
    }
    if (min_n < WIRECLAW_ANOMALY_WARMUP) {
        snprintf(out, out_len,
                 "anomaly_score: -1 (learning %lu/%d samples)",
                 (unsigned long)min_n, (int)WIRECLAW_ANOMALY_WARMUP);
        return;
    }
    float top = 0;
    int top_i = 0;
    for (int i = 0; i < N_FEAT; i++) {
        if (s_feat[i].seen && s_feat[i].z > top) {
            top = s_feat[i].z;
            top_i = i;
        }
    }
    int w = snprintf(out, out_len,
                     "anomaly_score: %.2f (top %s: %.1f vs mean %.1f",
                     (double)top, s_feat[top_i].name,
                     (double)s_feat[top_i].last, (double)s_feat[top_i].mean);
    for (int i = 0; i < N_FEAT && w < out_len - 1; i++) {
        if (!s_feat[i].seen) continue;
        w += snprintf(out + w, out_len - w, "; %s z=%.2f",
                      s_feat[i].name, (double)s_feat[i].z);
    }
    if (w < out_len - 1)
        snprintf(out + w, out_len - w, "; n=%lu)", (unsigned long)min_n);
}

#endif /* WIRECLAW_ANOMALY */
