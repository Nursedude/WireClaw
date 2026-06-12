/**
 * @file lora_ears.h
 * @brief Optional RX-only LoRa listener ("mesh ears").
 *
 * Built only when WIRECLAW_LORA_SX1262 is defined (boards with an SX1262,
 * e.g. the esp32-s3-heltec-v4 env). Every other env compiles no-ops — the
 * same guarded-optional pattern as the display and battery sense.
 *
 * RX ONLY: this module never transmits. It listens on a configured LoRa
 * channel (defaults match a Meshtastic LongFast/US mesh) and keeps honest
 * counters: packets heard, CRC errors, and the plaintext packet-header
 * fields of the last packet (Meshtastic headers are unencrypted: to/from/
 * id/flags/channel-hash). Payloads are never decrypted or stored.
 */

#ifndef LORA_EARS_H
#define LORA_EARS_H

#include <Arduino.h>

#ifdef WIRECLAW_LORA_SX1262

/** Init the SX1262 and enter continuous receive. Logs and runs deaf on
 *  failure — loraEarsAvailable() reports the truth. */
void loraEarsInit();

/** True when the radio initialized and is listening. */
bool loraEarsAvailable();

/** Drain received packets — call from loop(); cheap when idle. */
void loraEarsTick();

/** Format stats into `out`. The FIRST number is mesh_heard_age_s (seconds
 *  since the last packet — or since radio start while nothing has been
 *  heard yet: an honest lower bound, so "deaf forever" can still alarm). */
void loraEarsStats(char *out, int out_len);

#else /* !WIRECLAW_LORA_SX1262 — no-ops; callers need no #ifdefs */

static inline void loraEarsInit() {}
static inline bool loraEarsAvailable() { return false; }
static inline void loraEarsTick() {}
static inline void loraEarsStats(char *, int) {}

#endif /* WIRECLAW_LORA_SX1262 */

#endif /* LORA_EARS_H */
