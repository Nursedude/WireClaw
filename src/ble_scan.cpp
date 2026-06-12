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
 *
 * Failure honesty: this board's serial console is invisible in deployment
 * (app Serial is not on the USB port), so every failure must be readable
 * through ble_stats — which stage failed, how many retries, whether the
 * stack memory was reclaimed. Init/start failures are retried from the
 * loop (a start attempted at setup time can fail transiently before the
 * host settles); after BLE_MAX_TRIES the stack is deinitialized so a dead
 * scanner does not hold ~60 kB of heap hostage.
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

/* How often the loop-side watchdog ticks: retries a failed init/start,
 * restarts a stopped scan. */
#define BLE_TICK_MS 5000UL

/* Give up (and free the stack's memory) after this many failed tries. */
#define BLE_MAX_TRIES 12

#define BLE_UNIQ_MAX 32

enum BleEarsState {
    BLE_OFF = 0,     /* bleScanInit never ran */
    BLE_INIT_RETRY,  /* NimBLEDevice::init failed — retrying from loop */
    BLE_START_RETRY, /* stack up, scan start failed — retrying from loop */
    BLE_RUNNING,     /* scanning */
    BLE_GAVE_UP,     /* retries exhausted; stack deinitialized */
};

static BleEarsState s_state = BLE_OFF;
static int s_tries = 0;
static const char *s_fail_stage = "";
static unsigned long s_started_ms = 0;
static unsigned long s_last_tick_ms = 0;

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

/* Configure + start the continuous passive scan (stack must be inited). */
static bool bleTryStart() {
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&s_callbacks, true /* report repeats: keeps
                                                 ble_adv_age_s honest */);
    scan->setActiveScan(false); /* passive: never transmit a scan request */
    scan->setDuplicateFilter(0);
    scan->setMaxResults(0);     /* callback-only — no per-device storage */
    scan->setInterval(WIRECLAW_BLE_SCAN_INTERVAL_MS);
    scan->setWindow(WIRECLAW_BLE_SCAN_WINDOW_MS);
    return scan->start(0 /* forever */);
}

static void bleNowRunning() {
    s_state = BLE_RUNNING;
    s_started_ms = millis();
    Serial.printf("BLE ears: passive scan up after %d tr%s (window %d/%d ms, "
                  "observer role only, host task on core %d)\n",
                  s_tries + 1, s_tries == 0 ? "y" : "ies",
                  (int)WIRECLAW_BLE_SCAN_WINDOW_MS,
                  (int)WIRECLAW_BLE_SCAN_INTERVAL_MS,
                  (int)CONFIG_BT_NIMBLE_PINNED_TO_CORE);
}

void bleScanInit() {
    if (!NimBLEDevice::init("")) {
        Serial.printf("BLE ears: NimBLE init FAILED — retrying from loop\n");
        s_state = BLE_INIT_RETRY;
        s_tries = 1;
        return;
    }
    if (!bleTryStart()) {
        Serial.printf("BLE ears: scan start FAILED — retrying from loop\n");
        s_state = BLE_START_RETRY;
        s_tries = 1;
        return;
    }
    bleNowRunning();
}

bool bleAvailable() { return s_state == BLE_RUNNING; }

static void bleGiveUp(const char *stage) {
    /* Retries exhausted: record the stage for ble_stats, then release the
     * stack so a dead scanner does not hold its RAM. deinit no-ops if the
     * stack never fully initialized. */
    s_fail_stage = stage;
    NimBLEDevice::deinit(true);
    s_state = BLE_GAVE_UP;
    Serial.printf("BLE ears: gave up after %d tries (failed at %s) — "
                  "stack deinitialized\n", s_tries, stage);
}

void bleScanTick() {
    if (s_state == BLE_OFF || s_state == BLE_GAVE_UP) return;
    unsigned long now = millis();
    if (now - s_last_tick_ms < BLE_TICK_MS) return;
    s_last_tick_ms = now;

    switch (s_state) {
    case BLE_INIT_RETRY:
        if (s_tries >= BLE_MAX_TRIES) { bleGiveUp("init"); return; }
        s_tries++;
        if (NimBLEDevice::init("")) {
            if (bleTryStart()) bleNowRunning();
            else s_state = BLE_START_RETRY;
        }
        break;

    case BLE_START_RETRY:
        if (s_tries >= BLE_MAX_TRIES) { bleGiveUp("start"); return; }
        s_tries++;
        if (bleTryStart()) bleNowRunning();
        break;

    case BLE_RUNNING: {
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
                          BLE_TICK_MS / 1000UL);
        }
        break;
    }

    default:
        break;
    }
}

void bleStats(char *out, int out_len) {
    switch (s_state) {
    case BLE_OFF:
        snprintf(out, out_len, "Error: BLE scan not initialized");
        return;
    case BLE_INIT_RETRY:
        snprintf(out, out_len,
                 "Error: BLE init failed (try %d/%d, retrying)",
                 s_tries, BLE_MAX_TRIES);
        return;
    case BLE_START_RETRY:
        snprintf(out, out_len,
                 "Error: BLE scan start failed (try %d/%d, retrying — "
                 "stack is up)", s_tries, BLE_MAX_TRIES);
        return;
    case BLE_GAVE_UP:
        snprintf(out, out_len,
                 "Error: BLE gave up after %d tries (failed at %s; stack "
                 "deinitialized, heap reclaimed)", s_tries, s_fail_stage);
        return;
    case BLE_RUNNING:
        break;
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
