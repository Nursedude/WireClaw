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

#ifdef WIRECLAW_LORA_TX
#include <mbedtls/aes.h>
#include <esp_system.h>
#include <esp_mac.h>

/* GC1109 front-end (Heltec V4.2): CSD enable, VFEM power, PA/TX-enable.
 * RX worked in Phase 1 without driving these (FEM passes RX through), but
 * TX needs the PA path powered. Conservative by default — see TX_DBM. */
#ifndef WIRECLAW_LORA_FEM_CSD
#define WIRECLAW_LORA_FEM_CSD 2
#endif
#ifndef WIRECLAW_LORA_FEM_VPA
#define WIRECLAW_LORA_FEM_VPA 7
#endif
#ifndef WIRECLAW_LORA_FEM_TXEN
#define WIRECLAW_LORA_FEM_TXEN 46
#endif
#ifndef WIRECLAW_LORA_TX_DBM
#define WIRECLAW_LORA_TX_DBM 2   /* SX1262 drive level — modest first-light EIRP through the FEM */
#endif
#ifndef WIRECLAW_LORA_HOP_LIMIT
#define WIRECLAW_LORA_HOP_LIMIT 3
#endif
#ifndef WIRECLAW_LORA_TX_CHANNEL
#define WIRECLAW_LORA_TX_CHANNEL "LongFast"
#endif
#ifndef WIRECLAW_LORA_MIN_TX_INTERVAL_MS
#define WIRECLAW_LORA_MIN_TX_INTERVAL_MS 30000UL  /* >=30s between sends (airtime restraint) */
#endif

#define MESH_BROADCAST_ADDR 0xFFFFFFFFu
#define MESHTASTIC_PORT_TEXT 1
#define MESH_MAX_TEXT 200

/* Published PUBLIC default key (Meshtastic channel-1 default, "1PG7Oi...AQ==").
 * Public, not a secret — safe in source. A private channel key is supplied at
 * runtime via loraMeshSetPsk and never compiled in. */
static const uint8_t DEFAULT_PUBLIC_PSK[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};

static uint8_t s_psk[32];
static size_t s_psk_len = 16;
static uint8_t s_channel_hash = 0;
static char s_channel_name[32] = WIRECLAW_LORA_TX_CHANNEL;
static uint32_t s_node_id = 0;
static unsigned long s_last_tx_ms = 0;
static unsigned long s_tx_count = 0;

static uint8_t xorHash(const uint8_t *p, size_t len) {
    uint8_t c = 0;
    for (size_t i = 0; i < len; i++) c ^= p[i];
    return c;
}

/* hash = xorHash(channel name) ^ xorHash(psk) — Meshtastic Channels::hash */
static void recomputeChannelHash() {
    s_channel_hash = xorHash((const uint8_t *)s_channel_name,
                             strlen(s_channel_name)) ^
                     xorHash(s_psk, s_psk_len);
}

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode hex (32 or 64 chars) into out; returns byte count or 0 on bad input. */
static size_t decodeHexKey(const char *str, uint8_t *out, size_t out_max) {
    size_t slen = strlen(str);
    if (slen % 2 != 0 || slen / 2 > out_max) return 0;
    for (size_t i = 0; i < slen; i += 2) {
        int hi = hexNibble(str[i]), lo = hexNibble(str[i + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    return slen / 2;
}

static size_t decodeB64Key(const char *str, uint8_t *out, size_t out_max) {
    /* minimal standard-base64 decoder for 24/44-char keys */
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    size_t slen = strlen(str);
    while (slen && str[slen - 1] == '=') slen--;
    size_t out_n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < slen; i++) {
        int v = val(str[i]);
        if (v < 0) return 0;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out_n >= out_max) return 0;
            out[out_n++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return out_n;
}

uint8_t loraMeshChannelHash() { return s_channel_hash; }

bool loraMeshSetChannel(const char *name, const char *key_str) {
    if (!name || !name[0]) return false;
    /* Set name first; loraMeshSetPsk recomputes the hash against it. The
     * channel name is not secret; the key still never lands in flash. */
    strncpy(s_channel_name, name, sizeof(s_channel_name) - 1);
    s_channel_name[sizeof(s_channel_name) - 1] = '\0';
    return loraMeshSetPsk(key_str);
}

bool loraMeshSetPsk(const char *key_str) {
    if (!key_str || !key_str[0]) {
        memcpy(s_psk, DEFAULT_PUBLIC_PSK, sizeof(DEFAULT_PUBLIC_PSK));
        s_psk_len = 16;
        recomputeChannelHash();
        return true;
    }
    uint8_t tmp[32];
    size_t n = decodeHexKey(key_str, tmp, sizeof(tmp));
    if (n == 0) n = decodeB64Key(key_str, tmp, sizeof(tmp));
    if (n != 16 && n != 32) return false; /* AES-128 or AES-256 only */
    memcpy(s_psk, tmp, n);
    s_psk_len = n;
    recomputeChannelHash();
    return true;
}

static void femMode(bool tx) {
    /* CSD on + VFEM powered for either direction; TX-EN selects PA vs RX. */
    pinMode(WIRECLAW_LORA_FEM_VPA, OUTPUT);
    digitalWrite(WIRECLAW_LORA_FEM_VPA, HIGH);
    pinMode(WIRECLAW_LORA_FEM_CSD, OUTPUT);
    digitalWrite(WIRECLAW_LORA_FEM_CSD, HIGH);
    pinMode(WIRECLAW_LORA_FEM_TXEN, OUTPUT);
    digitalWrite(WIRECLAW_LORA_FEM_TXEN, tx ? HIGH : LOW);
}

void loraMeshSend(const char *text, char *out, int out_len) {
    if (!s_available) {
        snprintf(out, out_len, "Error: LoRa radio not up (init failed)");
        return;
    }
    if (!text || !text[0]) {
        snprintf(out, out_len, "Error: empty text");
        return;
    }
    size_t tlen = strlen(text);
    if (tlen > MESH_MAX_TEXT) {
        snprintf(out, out_len, "Error: text too long (%u > %d)",
                 (unsigned)tlen, MESH_MAX_TEXT);
        return;
    }
    unsigned long now = millis();
    if (s_tx_count > 0 &&
        (now - s_last_tx_ms) < WIRECLAW_LORA_MIN_TX_INTERVAL_MS) {
        snprintf(out, out_len,
                 "Error: airtime guard — %lus since last TX, min %lus",
                 (now - s_last_tx_ms) / 1000UL,
                 WIRECLAW_LORA_MIN_TX_INTERVAL_MS / 1000UL);
        return;
    }

    /* Data protobuf: field1 portnum (varint) + field2 payload (bytes) */
    uint8_t plain[8 + MESH_MAX_TEXT];
    size_t pn = 0;
    plain[pn++] = 0x08;                 /* field 1, varint */
    plain[pn++] = MESHTASTIC_PORT_TEXT; /* portnum = TEXT_MESSAGE_APP (1) */
    plain[pn++] = 0x12;                 /* field 2, length-delimited */
    plain[pn++] = (uint8_t)tlen;        /* len (<=200 fits one byte) */
    memcpy(&plain[pn], text, tlen);
    pn += tlen;

    /* AES-CTR nonce: [0:8]=packetId (id in low 4, LE), [8:12]=from (LE) */
    uint32_t pkt_id = esp_random();
    if (pkt_id == 0) pkt_id = 1;
    uint8_t nonce[16];
    memset(nonce, 0, sizeof(nonce));
    memcpy(nonce, &pkt_id, 4);
    memcpy(nonce + 8, &s_node_id, 4);

    uint8_t cipher[sizeof(plain)];
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, s_psk, (unsigned)s_psk_len * 8);
    size_t nc_off = 0;
    uint8_t stream_block[16];
    memset(stream_block, 0, sizeof(stream_block));
    mbedtls_aes_crypt_ctr(&aes, pn, &nc_off, nonce, stream_block, plain, cipher);
    mbedtls_aes_free(&aes);

    /* 16-byte header: to from id flags chan next_hop relay */
    uint8_t pkt[MESH_HEADER_LEN + sizeof(cipher)];
    uint32_t to = MESH_BROADCAST_ADDR;
    memcpy(&pkt[0], &to, 4);
    memcpy(&pkt[4], &s_node_id, 4);
    memcpy(&pkt[8], &pkt_id, 4);
    pkt[12] = (uint8_t)((WIRECLAW_LORA_HOP_LIMIT & 0x07) |
                        ((WIRECLAW_LORA_HOP_LIMIT & 0x07) << 5)); /* hop_limit + hop_start */
    pkt[13] = s_channel_hash;
    pkt[14] = 0; /* next_hop */
    pkt[15] = 0; /* relay_node */
    memcpy(&pkt[MESH_HEADER_LEN], cipher, pn);
    size_t pkt_len = MESH_HEADER_LEN + pn;

    /* Half-duplex: leave RX, FEM to TX, transmit, FEM to RX, resume RX. */
    s_radio->standby();
    femMode(true);
    s_radio->setOutputPower(WIRECLAW_LORA_TX_DBM);
    int st = s_radio->transmit(pkt, pkt_len);
    femMode(false);
    s_radio->startReceive();

    if (st != RADIOLIB_ERR_NONE) {
        snprintf(out, out_len, "Error: TX failed (%d)", st);
        return;
    }
    s_last_tx_ms = millis();
    s_tx_count++;
    snprintf(out, out_len,
             "Sent: ch '%s' (hash 0x%02x) from=!%08x id=0x%08x "
             "%u bytes @ %d dBm (tx #%lu)",
             s_channel_name, (unsigned)s_channel_hash,
             (unsigned)s_node_id, (unsigned)pkt_id, (unsigned)pkt_len,
             (int)WIRECLAW_LORA_TX_DBM, s_tx_count);
}
#endif /* WIRECLAW_LORA_TX */

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
#ifdef WIRECLAW_LORA_TX
    /* Node id = low 4 bytes of the EFUSE MAC (Meshtastic convention), so the
     * claw shows a stable !xxxxxxxx in every receiver's log. Load the public
     * default channel key + hash; a private key can override at runtime. */
    {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        s_node_id = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                    ((uint32_t)mac[4] << 8) | (uint32_t)mac[5];
    }
    loraMeshSetPsk(nullptr);
    Serial.printf("LoRa TX ready: node !%08x, ch '%s' hash 0x%02x, "
                  "%d dBm, min %lus between sends\n",
                  (unsigned)s_node_id, s_channel_name,
                  (unsigned)s_channel_hash, (int)WIRECLAW_LORA_TX_DBM,
                  WIRECLAW_LORA_MIN_TX_INTERVAL_MS / 1000UL);
#endif
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
