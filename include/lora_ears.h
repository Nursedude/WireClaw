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

/* TX surface gates on WIRECLAW_LORA_TX independently — its stubs must exist
 * on stock builds too (main/tools call them unconditionally). */
#ifdef WIRECLAW_LORA_TX

/** Set the TX channel PSK at runtime (base64 or hex; empty = built-in
 *  public default). Kept OUT of firmware so a private channel key is never
 *  compiled in (mirrors nats_token). Recomputes the channel hash. Returns
 *  false on a malformed key (length not 16/32 after decode). */
bool loraMeshSetPsk(const char *key_str);

/** Set BOTH the channel name and PSK at runtime (the name drives the hash,
 *  so a private channel needs both). Name is not secret; the key still never
 *  lands in flash. Returns false on empty name or a malformed key. */
bool loraMeshSetChannel(const char *name, const char *key_str);

/** Current channel hash byte (the cleartext header field — not secret). */
uint8_t loraMeshChannelHash();

/** Persist the channel (name + key) to the device's own flash so it survives
 *  reboots (operator-opt-in durability; dedicated file, not config.json). */
bool loraPersistChannel(const char *name, const char *key_str);

/** Restore a persisted channel at boot (no-op if none). */
void loraLoadPersistedChannel();

/** Forget any persisted channel (revert to the public default on next boot). */
void loraClearPersistedChannel();

/** Broadcast a Meshtastic TEXT_MESSAGE_APP packet on the configured channel.
 *  Half-duplex: pauses RX, transmits, resumes RX. Enforces a minimum
 *  inter-TX interval (airtime restraint) and refuses oversize text. Writes
 *  a human-readable outcome (incl. the node id and packet id) into `out`. */
void loraMeshSend(const char *text, char *out, int out_len);

#else

static inline bool loraMeshSetPsk(const char *) { return false; }
static inline bool loraMeshSetChannel(const char *, const char *) {
    return false;
}
static inline uint8_t loraMeshChannelHash() { return 0; }
static inline bool loraPersistChannel(const char *, const char *) {
    return false;
}
static inline void loraLoadPersistedChannel() {}
static inline void loraClearPersistedChannel() {}
static inline void loraMeshSend(const char *, char *out, int out_len) {
    snprintf(out, out_len, "Error: LoRa TX not built on this device");
}

#endif /* WIRECLAW_LORA_TX */

#endif /* LORA_EARS_H */
