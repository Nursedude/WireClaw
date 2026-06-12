/**
 * @file lora_ears.cpp
 * @brief RX-only LoRa listener (WIRECLAW_LORA_SX1262 builds only).
 *
 * Defaults tuned to a Meshtastic LongFast/US mesh (906.875 MHz, BW 250 kHz,
 * SF11, CR 4/5, sync 0x2B, preamble 16); every parameter is a build flag so
 * other meshes only need a new env. The 16-byte Meshtastic packet header is
 * plaintext — we surface its fields and the radio's RSSI/SNR, and never
 * touch the encrypted payload.
 */

#ifdef WIRECLAW_LORA_SX1262

#include "lora_ears.h"
#include <RadioLib.h>
#include <SPI.h>

#ifndef WIRECLAW_LORA_SCK
#define WIRECLAW_LORA_SCK 9
#endif
#ifndef WIRECLAW_LORA_MISO
#define WIRECLAW_LORA_MISO 11
#endif
#ifndef WIRECLAW_LORA_MOSI
#define WIRECLAW_LORA_MOSI 10
#endif
#ifndef WIRECLAW_LORA_NSS
#define WIRECLAW_LORA_NSS 8
#endif
#ifndef WIRECLAW_LORA_RST
#define WIRECLAW_LORA_RST 12
#endif
#ifndef WIRECLAW_LORA_BUSY
#define WIRECLAW_LORA_BUSY 13
#endif
#ifndef WIRECLAW_LORA_DIO1
#define WIRECLAW_LORA_DIO1 14
#endif
#ifndef WIRECLAW_LORA_TCXO_V
#define WIRECLAW_LORA_TCXO_V 1.8f
#endif
#ifndef WIRECLAW_LORA_FREQ_MHZ
#define WIRECLAW_LORA_FREQ_MHZ 906.875f
#endif
#ifndef WIRECLAW_LORA_BW_KHZ
#define WIRECLAW_LORA_BW_KHZ 250.0f
#endif
#ifndef WIRECLAW_LORA_SF
#define WIRECLAW_LORA_SF 11
#endif
#ifndef WIRECLAW_LORA_CR
#define WIRECLAW_LORA_CR 5
#endif
#ifndef WIRECLAW_LORA_SYNC
#define WIRECLAW_LORA_SYNC 0x2B
#endif
#ifndef WIRECLAW_LORA_PREAMBLE
#define WIRECLAW_LORA_PREAMBLE 16
#endif

#define MESH_HEADER_LEN 16

static SX1262 *s_radio = nullptr; /* constructed in loraEarsInit */
static bool s_available = false;
static volatile bool s_rx_flag = false;

static unsigned long s_started_ms = 0;
static unsigned long s_heard = 0;
static unsigned long s_crc_err = 0;
static unsigned long s_runts = 0;
static unsigned long s_last_heard_ms = 0;
static uint32_t s_last_from = 0, s_last_to = 0;
static uint8_t s_last_ch = 0;
static float s_last_rssi = 0.0f, s_last_snr = 0.0f;

static void IRAM_ATTR onLoraDio1() { s_rx_flag = true; }

void loraEarsInit() {
    SPI.begin(WIRECLAW_LORA_SCK, WIRECLAW_LORA_MISO, WIRECLAW_LORA_MOSI,
              WIRECLAW_LORA_NSS);
    s_radio = new SX1262(new Module(WIRECLAW_LORA_NSS, WIRECLAW_LORA_DIO1,
                                    WIRECLAW_LORA_RST, WIRECLAW_LORA_BUSY));

    int state = s_radio->begin(WIRECLAW_LORA_FREQ_MHZ, WIRECLAW_LORA_BW_KHZ,
                              WIRECLAW_LORA_SF, WIRECLAW_LORA_CR,
                              WIRECLAW_LORA_SYNC, 10, WIRECLAW_LORA_PREAMBLE,
                              WIRECLAW_LORA_TCXO_V);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("LoRa ears: SX1262 begin FAILED (%d) — running deaf\n",
                      state);
        s_available = false;
        return;
    }
    /* The V4 routes its RF switch from DIO2 (Meshtastic variant.h:
     * SX126X_DIO2_AS_RF_SWITCH) */
    s_radio->setDio2AsRfSwitch(true);
    s_radio->setCRC(2); /* Meshtastic uses 2-byte LoRa CRC */
    s_radio->setDio1Action(onLoraDio1);
    state = s_radio->startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("LoRa ears: startReceive FAILED (%d) — running deaf\n",
                      state);
        s_available = false;
        return;
    }
    s_available = true;
    s_started_ms = millis();
    Serial.printf("LoRa ears: RX-only listening on %.3f MHz "
                  "(bw %.0f kHz, sf %d, cr 4/%d, sync 0x%02X)\n",
                  (double)WIRECLAW_LORA_FREQ_MHZ,
                  (double)WIRECLAW_LORA_BW_KHZ,
                  (int)WIRECLAW_LORA_SF, (int)WIRECLAW_LORA_CR,
                  (unsigned)WIRECLAW_LORA_SYNC);
}

bool loraEarsAvailable() { return s_available; }

void loraEarsTick() {
    if (!s_available || !s_rx_flag) return;
    s_rx_flag = false;

    /* Single SX1262 buffer: a packet arriving before we drain overwrites
     * the previous one — fine for a stats sniffer, counters stay honest. */
    uint8_t buf[256];
    size_t len = s_radio->getPacketLength();
    if (len > sizeof(buf)) len = sizeof(buf);
    int state = s_radio->readData(buf, len);

    if (state == RADIOLIB_ERR_CRC_MISMATCH) {
        s_crc_err++;
    } else if (state == RADIOLIB_ERR_NONE) {
        if (len >= MESH_HEADER_LEN) {
            s_heard++;
            s_last_heard_ms = millis();
            /* Meshtastic PacketHeader, little-endian on the wire:
             * to(4) from(4) id(4) flags(1) channel(1) next_hop(1) relay(1) */
            memcpy(&s_last_to, &buf[0], 4);
            memcpy(&s_last_from, &buf[4], 4);
            s_last_ch = buf[13];
            s_last_rssi = s_radio->getRSSI();
            s_last_snr = s_radio->getSNR();
        } else {
            s_runts++; /* heard energy, not a mesh packet — count separately */
        }
    }
    s_radio->startReceive();
}

void loraEarsStats(char *out, int out_len) {
    if (!s_available) {
        snprintf(out, out_len, "Error: LoRa radio not listening (init failed)");
        return;
    }
    unsigned long now = millis();
    if (s_heard == 0) {
        /* Honest lower bound: never-heard reports time since radio start so
         * a "silent too long" threshold can still trip on a deaf node. */
        snprintf(out, out_len,
                 "mesh_heard_age_s: %lu (heard 0 pkts since radio start, "
                 "crc_err %lu, runts %lu, %.3f MHz)",
                 (now - s_started_ms) / 1000UL, s_crc_err, s_runts,
                 (double)WIRECLAW_LORA_FREQ_MHZ);
        return;
    }
    snprintf(out, out_len,
             "mesh_heard_age_s: %lu (heard %lu pkts, crc_err %lu, runts %lu, "
             "last from=!%08x to=!%08x ch=0x%02x rssi=%.0f snr=%.1f)",
             (now - s_last_heard_ms) / 1000UL, s_heard, s_crc_err, s_runts,
             (unsigned)s_last_from, (unsigned)s_last_to, (unsigned)s_last_ch,
             (double)s_last_rssi, (double)s_last_snr);
}

#endif /* WIRECLAW_LORA_SX1262 */
