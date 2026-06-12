/**
 * @file display.h
 * @brief Optional SSD1306 status display (dude-claw fork).
 *
 * Built only when DUDECLAW_OLED is defined (the esp32-s3-heltec-v4 env).
 * Every other env compiles these as no-ops, so upstream targets are
 * byte-for-byte unaffected — the same guarded-optional pattern as the
 * chip-temp sensor.
 *
 * Heltec WiFi LoRa 32 V4 wiring (official pinmap, verified 2026-06-11):
 *   OLED_SDA=GPIO17  OLED_SCL=GPIO18  OLED_RST=GPIO21
 *   Vext_Ctrl=GPIO36 (active LOW — powers the OLED rail)
 *   SSD1306 128x64 @ I2C 0x3C
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>

/* Remote-writable metric rows (display_print tool) */
#define DISPLAY_METRIC_ROWS 2
#define DISPLAY_METRIC_COLS 24  /* chars per row incl. NUL */

#ifdef DUDECLAW_OLED

/** Power the OLED rail, reset + init the panel, draw the boot screen. */
void displayInit();

/** True when the panel initialized OK (i2c device answered). */
bool displayAvailable();

/** Big "setup mode" screen shown before the blocking captive portal. */
void displayShowSetupMode();

/**
 * Periodic status redraw — call from loop(). Internally rate-limited
 * (~1 Hz); cheap to call every iteration.
 * @param nats_connected  live NATS connection state from the caller
 */
void displayTick(bool nats_connected);

/**
 * Write a remote metric row (0..DISPLAY_METRIC_ROWS-1). Empty text clears
 * the row. Rows older than 30 min are drawn with a trailing "(old)" marker —
 * a stale metric must not read as a live one.
 * @return false when row is out of range.
 */
bool displayPrintRow(int row, const char *text);

#else /* !DUDECLAW_OLED — no-ops; callers need no #ifdefs */

static inline void displayInit() {}
static inline bool displayAvailable() { return false; }
static inline void displayShowSetupMode() {}
static inline void displayTick(bool) {}
static inline bool displayPrintRow(int, const char *) { return false; }

#endif /* DUDECLAW_OLED */

#endif /* DISPLAY_H */
