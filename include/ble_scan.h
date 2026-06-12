/**
 * @file ble_scan.h
 * @brief Optional passive BLE advertisement listener ("BLE ears").
 *
 * Built only when WIRECLAW_BLE is defined (e.g. the esp32-s3-heltec-v4 env).
 * Every other env compiles no-ops — the same guarded-optional pattern as the
 * display, battery sense, and LoRa ears.
 *
 * PASSIVE and observer-only: never scan-requests, never connects, never
 * advertises — pure listening, like the LoRa ears. WiFi and BLE share the
 * ESP32's single 2.4 GHz radio (the hardware coexistence arbiter
 * time-slices them); the scan duty cycle is deliberately modest
 * (window < interval) because the WiFi/NATS link is load-bearing and
 * always wins.
 *
 * This build is the COEXISTENCE VALIDATION step: aggregate counters only,
 * no per-device tracking yet. Once a soak proves the NATS link, heap, and
 * WiFi RSSI stay healthy with the scan running, v1 adds named-target
 * presence (ble_seen_age_s) and v2 decoded sensor adverts (BTHome etc.).
 */

#ifndef BLE_SCAN_H
#define BLE_SCAN_H

#include <Arduino.h>

#ifdef WIRECLAW_BLE

/** Init NimBLE and start a continuous passive scan. Logs and runs deaf on
 *  failure — bleAvailable() reports the truth. Call AFTER WiFi is up so the
 *  brain link is never delayed by the BLE stack. */
void bleScanInit();

/** True when the stack initialized and the scan was started. */
bool bleAvailable();

/** Watchdog — call from loop(); restarts a stopped scan (rate-limited) and
 *  counts every restart, so scan instability is visible in the stats. */
void bleScanTick();

/** Format stats into `out`. The FIRST number is ble_adv_age_s (seconds
 *  since the last advertisement heard — or since scan start while nothing
 *  has been heard yet: an honest lower bound, so "deaf forever" can still
 *  alarm). */
void bleStats(char *out, int out_len);

#else /* !WIRECLAW_BLE — no-ops; callers need no #ifdefs */

static inline void bleScanInit() {}
static inline bool bleAvailable() { return false; }
static inline void bleScanTick() {}
static inline void bleStats(char *, int) {}

#endif /* WIRECLAW_BLE */

#endif /* BLE_SCAN_H */
