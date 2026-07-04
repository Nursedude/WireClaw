/**
 * @file display.h
 * @brief Optional SSD1306 status display.
 *
 * Built only when WIRECLAW_OLED is defined (boards with a panel, e.g. the
 * esp32-s3-heltec-v4 env). Every other env compiles these as no-ops, so
 * existing targets are unaffected — the same guarded-optional pattern as
 * the chip-temp sensor.
 *
 * WIRECLAW_OLED_PAGES additionally builds the v2 surface: a brain-tier
 * glyph (display_tier tool), an alert banner (display_alert tool), and two
 * locally-rendered pages (EARS / SELF) cycled by the PRG/USER button
 * (WIRECLAW_OLED_BUTTON, a GPIO number). Without the flag, the v2 entry
 * points compile to honest refusals (return false), never silent oks.
 *
 * Pins come from WIRECLAW_OLED_SDA/SCL/RST/VEXT build flags so other
 * OLED boards only need a new env. Reference wiring (Heltec WiFi LoRa 32
 * V4, official pinmap):
 *   OLED_SDA=GPIO17  OLED_SCL=GPIO18  OLED_RST=GPIO21
 *   Vext_Ctrl=GPIO36 (active LOW — powers the OLED rail)
 *   SSD1306 128x64 @ I2C 0x3C
 *   PRG/USER button = GPIO0 (per the V3 pinmap; V4 near-identical — the
 *   flag exists so a board where that differs only needs a new env)
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>

/* Remote-writable metric rows (display_print tool) */
#define DISPLAY_METRIC_ROWS 2
#define DISPLAY_METRIC_COLS 24  /* chars per row incl. NUL */

#ifdef WIRECLAW_OLED

/** Power the OLED rail, reset + init the panel, draw the boot screen. */
void displayInit();

/** True when the panel initialized OK (i2c device answered). */
bool displayAvailable();

/** Big "setup mode" screen shown before the blocking captive portal. */
void displayShowSetupMode();

/**
 * Periodic status redraw — call from loop(). Internally rate-limited
 * (~1 Hz); cheap to call every iteration. Pages builds also poll the
 * page button and close sparkline buckets here.
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

/**
 * Set the brain-tier glyph ('F' frontier / 'L' local LLM / 'R' rules-only).
 * The glyph decays ON THE DEVICE'S OWN CLOCK: <15 min fresh, <30 min drawn
 * with a trailing '?', older drawn as "SOLO" — a dead brain can never leave
 * a fresh-looking claim on the glass. Never-pushed draws nothing (absence).
 * @return false on an unknown tier char, or when pages are not built.
 */
bool displaySetTier(char tier);

/**
 * Show (or clear) the inverted alert banner on the status page. `age_s` is
 * the alert age at push time; the display ticks it forward locally, so a
 * NATS drop mid-alert leaves the banner up with the age still climbing.
 * NULL/empty `cls` is the ONLY way the banner clears.
 * @return false when pages are not built (headless refusal, not silence).
 */
bool displaySetAlert(const char *cls, long age_s);

#else /* !WIRECLAW_OLED — no-ops; callers need no #ifdefs */

static inline void displayInit() {}
static inline bool displayAvailable() { return false; }
static inline void displayShowSetupMode() {}
static inline void displayTick(bool) {}
static inline bool displayPrintRow(int, const char *) { return false; }
static inline bool displaySetTier(char) { return false; }
static inline bool displaySetAlert(const char *, long) { return false; }

#endif /* WIRECLAW_OLED */

#endif /* DISPLAY_H */
