/**
 * @file anomaly.h
 * @brief Optional edge anomaly witness (WIRECLAW_ANOMALY builds only).
 *
 * "The edge learns its own normal": exponentially-weighted running
 * mean/variance per telemetry feature (free heap, chip temp, WiFi RSSI,
 * LoRa rx-rate) -> per-feature |z|-scores -> one anomaly score the brain
 * can poll. Deliberately NOT a neural net: for four slow scalars, running
 * statistics self-calibrate, adapt to slow drift, need no training
 * pipeline, and can NAME the deviating feature — a witness that cannot
 * explain itself is half a witness.
 *
 * Honesty rules:
 *   - Below the warm-up sample count the score is -1 ("learning"), never 0:
 *     an unwarmed baseline must not read as "all normal".
 *   - A feature that is unobservable this sample (NAN temp, WiFi down,
 *     no LoRa radio) is SKIPPED, never recorded as 0.
 *   - Witness only: this module never actuates, never transmits — the
 *     brain's rules decide what a deviation means.
 *
 * Every other env compiles these as no-ops — the same guarded-optional
 * pattern as the display and the listeners.
 */

#ifndef ANOMALY_H
#define ANOMALY_H

#include <Arduino.h>

#ifdef WIRECLAW_ANOMALY

/** Reset baselines and start sampling (call from setup()). */
void anomalyInit();

/** Periodic sampler — call from loop(); internally rate-limited (~60 s). */
void anomalyTick();

/** Format the witness into `out`. FIRST number is the anomaly score
 *  (max |z| across warmed-up features), or -1 while learning — so a
 *  source-side numeric extractor gets the score with a `gt` threshold and
 *  the learning phase can never breach it. */
void anomalyStats(char *out, int out_len);

#else /* !WIRECLAW_ANOMALY — no-ops; callers need no #ifdefs */

static inline void anomalyInit() {}
static inline void anomalyTick() {}
static inline void anomalyStats(char *out, int out_len) {
    snprintf(out, out_len, "Error: no anomaly witness on this device");
}

#endif /* WIRECLAW_ANOMALY */

#endif /* ANOMALY_H */
