/**
 * @file ble_scan.cpp
 * @brief Passive BLE advertisement listener (WIRECLAW_BLE builds only).
 *
 * NimBLE, not Bluedroid (~100 kB less RAM). Observer role only — the
 * central/peripheral/broadcaster roles are compiled out via build flags.
 * The NimBLE host task runs on core 0 beside the WiFi stack; the Arduino
 * loop (NATS) owns core 1. setMaxResults(0) keeps the scan callback-only:
 * nothing is stored per device, so a busy RF environment cannot grow the
 * heap. Counters are plain 32-bit writes from the host task, read from the
 * loop task — diagnostic precision, not transactional.
 */

#ifdef WIRECLAW_BLE

#include "ble_scan.h"
#include <NimBLEDevice.h>

/* Scan duty: 48 ms listening out of every 320 ms (~15%) leaves WiFi most
 * of the airtime on the shared radio. Build-flag tunable per board. */
#ifndef WIRECLAW_BLE_SCAN_INTERVAL_MS
#define WIRECLAW_BLE_SCAN_INTERVAL_MS 320
#endif
#ifndef WIRECLAW_BLE_SCAN_WINDOW_MS
#define WIRECLAW_BLE_SCAN_WINDOW_MS 48
#endif

/* How often the loop-side watchdog checks that the scan is still running. */
#define BLE_RESTART_CHECK_MS 5000UL

#define BLE_UNIQ_MAX 32

static bool s_available = false;
static unsigned long s_started_ms = 0;

static volatile unsigned long s_advs = 0;       /* every advert, incl. repeats */
static volatile unsigned long s_last_adv_ms = 0;
static volatile int s_last_rssi = 0;

/* Bounded unique-device table (host task is the only writer). When full,
 * new addresses are counted as misses instead of silently merged — the
 * stats then report "32+" rather than a false-precise number. */
static uint8_t s_uniq_mac[BLE_UNIQ_MAX][6];
static volatile int s_uniq = 0;
static volatile unsigned long s_uniq_misses = 0;

static volatile unsigned long s_scan_ends = 0;  /* unsolicited scan stops */
static volatile int s_last_end_reason = 0;
static unsigned long s_restarts = 0;
static unsigned long s_restart_fails = 0;
static unsigned long s_last_restart_check_ms = 0;

class BleEarsCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice *dev) override {
        s_advs = s_advs + 1;
        s_last_adv_ms = millis();
        s_last_rssi = dev->getRSSI();

        const uint8_t *mac = dev->getAddress().getBase()->val;
        for (int i = 0; i < s_uniq; i++)
            if (memcmp(s_uniq_mac[i], mac, 6) == 0) return;
        if (s_uniq < BLE_UNIQ_MAX) {
            memcpy(s_uniq_mac[s_uniq], mac, 6);
            s_uniq = s_uniq + 1;
        } else {
            s_uniq_misses = s_uniq_misses + 1;
        }
    }
    void onScanEnd(const NimBLEScanResults &results, int reason) override {
        /* Continuous scan should never end on its own. Count it and let
         * bleScanTick() (loop task) do the restart — restarting from inside
         * the host-task callback risks a deadlock. */
        (void)results;
        s_scan_ends = s_scan_ends + 1;
        s_last_end_reason = reason;
    }
};

static BleEarsCallbacks s_callbacks;

void bleScanInit() {
    if (!NimBLEDevice::init("")) {
        Serial.printf("BLE ears: NimBLE init FAILED — running BLE-deaf\n");
        s_available = false;
        return;
    }
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&s_callbacks, true /* report repeats: keeps
                                                 ble_adv_age_s honest */);
    scan->setActiveScan(false); /* passive: never transmit a scan request */
    scan->setDuplicateFilter(0);
    scan->setMaxResults(0);     /* callback-only — no per-device storage */
    scan->setInterval(WIRECLAW_BLE_SCAN_INTERVAL_MS);
    scan->setWindow(WIRECLAW_BLE_SCAN_WINDOW_MS);
    if (!scan->start(0 /* forever */)) {
        Serial.printf("BLE ears: scan start FAILED — running BLE-deaf\n");
        s_available = false;
        return;
    }
    s_available = true;
    s_started_ms = millis();
    Serial.printf("BLE ears: passive scan up (window %d/%d ms, observer "
                  "role only, host task on core %d)\n",
                  (int)WIRECLAW_BLE_SCAN_WINDOW_MS,
                  (int)WIRECLAW_BLE_SCAN_INTERVAL_MS,
                  (int)CONFIG_BT_NIMBLE_PINNED_TO_CORE);
}

bool bleAvailable() { return s_available; }

void bleScanTick() {
    if (!s_available) return;
    unsigned long now = millis();
    if (now - s_last_restart_check_ms < BLE_RESTART_CHECK_MS) return;
    s_last_restart_check_ms = now;

    NimBLEScan *scan = NimBLEDevice::getScan();
    if (scan->isScanning()) return;
    if (scan->start(0)) {
        s_restarts++;
        Serial.printf("BLE ears: scan stopped (reason %d) — restarted "
                      "(restart #%lu)\n", s_last_end_reason, s_restarts);
    } else {
        s_restart_fails++;
        Serial.printf("BLE ears: scan restart FAILED (fail #%lu) — "
                      "retrying in %lus\n", s_restart_fails,
                      BLE_RESTART_CHECK_MS / 1000UL);
    }
}

void bleStats(char *out, int out_len) {
    if (!s_available) {
        snprintf(out, out_len, "Error: BLE scan not running (init failed)");
        return;
    }
    unsigned long now = millis();
    char uniq_buf[8];
    if (s_uniq_misses > 0)
        snprintf(uniq_buf, sizeof(uniq_buf), "%d+", (int)s_uniq);
    else
        snprintf(uniq_buf, sizeof(uniq_buf), "%d", (int)s_uniq);

    if (s_advs == 0) {
        /* Honest lower bound: never-heard reports time since scan start so
         * a "silent too long" threshold can still trip on a deaf scanner. */
        snprintf(out, out_len,
                 "ble_adv_age_s: %lu (advs 0 since scan start, uniq 0, "
                 "restarts %lu/%lu, window %d/%dms)",
                 (now - s_started_ms) / 1000UL, s_restarts, s_restart_fails,
                 (int)WIRECLAW_BLE_SCAN_WINDOW_MS,
                 (int)WIRECLAW_BLE_SCAN_INTERVAL_MS);
        return;
    }
    snprintf(out, out_len,
             "ble_adv_age_s: %lu (advs %lu, uniq %s, last rssi %d dBm, "
             "restarts %lu/%lu, window %d/%dms)",
             (now - s_last_adv_ms) / 1000UL, (unsigned long)s_advs, uniq_buf,
             (int)s_last_rssi, s_restarts, s_restart_fails,
             (int)WIRECLAW_BLE_SCAN_WINDOW_MS,
             (int)WIRECLAW_BLE_SCAN_INTERVAL_MS);
}

#endif /* WIRECLAW_BLE */
