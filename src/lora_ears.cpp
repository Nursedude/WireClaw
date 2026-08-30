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
static int      s_last_hops = -1;   /* -1 == unknown/malformed, never "direct" */
static uint8_t s_last_ch = 0;
static float s_last_rssi = 0.0f, s_last_snr = 0.0f;

/* WATCH LIST — per-id last-heard, so "is OUR transmitter on the air?" can be
 * answered separately from "is the channel silent?". Bounded and static: this
 * runs on a 237 kB-heap ESP32-S3, and an unbounded list would trade a real
 * blind spot for a heap risk. */
/* Sized to the DOMAIN, not to a round number: this fleet runs ~8 radios, so a
 * cap of 6 would silently drop real members (the operator's "watch the ones
 * that are part of the fleet"). 12 gives headroom at 16 bytes/entry = 192 B on a
 * 237 kB heap — bounded, but not artificially tight. */
#define LORA_WATCH_MAX 12
static uint32_t      s_watch_id[LORA_WATCH_MAX];
static unsigned long s_watch_last_ms[LORA_WATCH_MAX];
static unsigned long s_watch_pkts[LORA_WATCH_MAX];
static float         s_watch_rssi[LORA_WATCH_MAX];
/* DIRECT reception, tracked separately from any-path reception (2026-08-30).
 * `watch=` answers "traffic bearing this id reached me" — in a flood mesh that
 * may be a NEARER node rebroadcasting, so its RSSI is that relay's signal, not
 * this node's. For siting a digipeater the only useful question is "do I have
 * a DIRECT link to this node, and how good is it", and the two were
 * indistinguishable until now. hops == 0 means the originator's own
 * transmission. */
static unsigned long s_watch_direct_ms[LORA_WATCH_MAX];
static float         s_watch_direct_rssi[LORA_WATCH_MAX];
static int           s_watch_n = 0;
static int           s_watch_dropped = 0;   /* ids past MAX — reported, not hidden */

void loraEarsSetWatch(const char *csv) {
    s_watch_n = 0;
    s_watch_dropped = 0;
    if (!csv || !*csv) return;
    const char *p = csv;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '!') p++;
        if (!*p) break;
        char tok[16]; int n = 0;
        while (*p && *p != ',' && *p != ' ' && n < (int)sizeof(tok) - 1) tok[n++] = *p++;
        tok[n] = '\0';
        if (n == 0) continue;
        unsigned long v = strtoul(tok, nullptr, 16);
        if (v == 0UL) continue;                 /* unparseable -> skip, never 0 */
        if (s_watch_n >= LORA_WATCH_MAX) { s_watch_dropped++; continue; }
        s_watch_id[s_watch_n]      = (uint32_t)v;
        s_watch_last_ms[s_watch_n] = 0UL;       /* 0 == NEVER heard, not "now" */
        s_watch_pkts[s_watch_n]    = 0UL;
        s_watch_rssi[s_watch_n]    = 0.0f;
        s_watch_direct_ms[s_watch_n]   = 0UL;   /* 0 == never heard DIRECT */
        s_watch_direct_rssi[s_watch_n] = 0.0f;
        s_watch_n++;
    }
}

static void IRAM_ATTR onLoraDio1() { s_rx_flag = true; }

#ifdef WIRECLAW_LORA_TX
#include <mbedtls/aes.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <LittleFS.h>

#define LORA_CHANNEL_FILE "/lora_channel.json"

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

/* Decode hex into out; returns byte count or 0 on bad input. Tolerant of a
 * leading 0x, and of whitespace / ':' separators (e.g. "aa:bb cc"). */
static size_t decodeHexKey(const char *str, uint8_t *out, size_t out_max) {
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) str += 2;
    size_t out_n = 0;
    int hi = -1;
    for (const char *p = str; *p; p++) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ':')
            continue;
        int nib = hexNibble(c);
        if (nib < 0) return 0;
        if (hi < 0) {
            hi = nib;
        } else {
            if (out_n >= out_max) return 0;
            out[out_n++] = (uint8_t)((hi << 4) | nib);
            hi = -1;
        }
    }
    if (hi >= 0) return 0; /* odd nibble count */
    return out_n;
}

static size_t decodeB64Key(const char *str, uint8_t *out, size_t out_max) {
    /* base64 standard AND url-safe (- _); skips whitespace and '=' padding. */
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    size_t out_n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (const char *p = str; *p; p++) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '=')
            continue;
        int v = val(c);
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

/* Bytes the last key decode produced (diagnostic only — a length, not the
 * key). -1 before any attempt. */
static int s_last_key_len = -1;
int loraLastKeyLen() { return s_last_key_len; }

uint8_t loraMeshChannelHash() { return s_channel_hash; }

bool loraMeshSetChannel(const char *name, const char *key_str) {
    if (!name || !name[0]) return false;
    /* Set name first; loraMeshSetPsk recomputes the hash against it. The
     * channel name is not secret. */
    strncpy(s_channel_name, name, sizeof(s_channel_name) - 1);
    s_channel_name[sizeof(s_channel_name) - 1] = '\0';
    return loraMeshSetPsk(key_str);
}

/* Minimal JSON string-value extractor for the tiny persisted file. */
static bool loraJsonGet(const char *json, const char *key, char *dst,
                        int dst_len) {
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return false;
    p++;
    int w = 0;
    while (*p && *p != '"' && w < dst_len - 1) {
        if (*p == '\\' && *(p + 1)) p++;
        dst[w++] = *p++;
    }
    dst[w] = '\0';
    return true;
}

bool loraPersistChannel(const char *name, const char *key_str) {
    /* Operator opted into durability: the channel (name + key) is written to
     * the device's own flash — the same trust model as Meshtastic's channel
     * store. Kept in a DEDICATED file, not config.json, so it never clobbers
     * keys this build doesn't know (e.g. another fork branch's). */
    if (!name || !name[0]) return false;
    File f = LittleFS.open(LORA_CHANNEL_FILE, "w");
    if (!f) return false;
    f.print("{\"name\":\"");
    for (const char *c = name; *c; c++)
        if (*c != '"' && *c != '\\') f.print(*c);
    f.print("\",\"psk\":\"");
    if (key_str)
        for (const char *c = key_str; *c; c++)
            if (*c != '"' && *c != '\\') f.print(*c);
    f.print("\"}\n");
    f.close();
    return true;
}

void loraClearPersistedChannel() {
    if (LittleFS.exists(LORA_CHANNEL_FILE)) LittleFS.remove(LORA_CHANNEL_FILE);
}

void loraLoadPersistedChannel() {
    if (!LittleFS.exists(LORA_CHANNEL_FILE)) return;
    File f = LittleFS.open(LORA_CHANNEL_FILE, "r");
    if (!f) return;
    char buf[160];
    int n = f.readBytes(buf, sizeof(buf) - 1);
    f.close();
    if (n <= 0) return;
    buf[n] = '\0';
    char name[40] = {0}, psk[96] = {0};
    if (loraJsonGet(buf, "name", name, sizeof(name)) && name[0]) {
        loraJsonGet(buf, "psk", psk, sizeof(psk));
        if (loraMeshSetChannel(name, psk))
            Serial.printf("LoRa TX: persisted channel '%s' restored\n", name);
        else
            Serial.printf("LoRa TX: persisted channel INVALID — public "
                          "default stands\n");
    }
}

bool loraMeshSetPsk(const char *key_str) {
    if (!key_str || !key_str[0]) {
        memcpy(s_psk, DEFAULT_PUBLIC_PSK, sizeof(DEFAULT_PUBLIC_PSK));
        s_psk_len = 16;
        s_last_key_len = 16;
        recomputeChannelHash();
        return true;
    }
    uint8_t tmp[32];
    size_t n = decodeHexKey(key_str, tmp, sizeof(tmp));
    if (n != 16 && n != 32) n = decodeB64Key(key_str, tmp, sizeof(tmp));
    s_last_key_len = (int)n;
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

unsigned long loraMeshTxCount() { return s_tx_count; }

long loraMeshTxGuardRemainS() {
    if (s_tx_count == 0) return 0; /* never sent: guard has nothing to hold */
    unsigned long since = millis() - s_last_tx_ms;
    if (since >= WIRECLAW_LORA_MIN_TX_INTERVAL_MS) return 0;
    return (long)((WIRECLAW_LORA_MIN_TX_INTERVAL_MS - since) / 1000UL) + 1;
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
            /* flags byte: low 3 bits = hop_limit REMAINING, high 3 bits =
             * hop_start ORIGINAL — the same layout this file's own TX path
             * writes at pkt[12], and confirmed against a captured live packet
             * (moc logged flags=0x62 for one it reported hop_start:3
             * hops_away:1; 0x62 -> limit 2, start 3, 3-2 = 1).
             * hops == 0 therefore means the ORIGINATOR's own transmission
             * reached this radio: a direct link, not a rebroadcast. */
            uint8_t flags = buf[12];
            int hop_limit = flags & 0x07;
            int hop_start = (flags >> 5) & 0x07;
            /* A relay decrements hop_limit, so start >= limit. If the wire says
             * otherwise the header is malformed or from a foreign stack: treat
             * it as UNKNOWN hop distance, never as direct — claiming a direct
             * link we cannot substantiate is the failure that matters here. */
            int hops = (hop_start >= hop_limit) ? (hop_start - hop_limit) : -1;
            s_last_hops = hops;
            /* Watch list: attribute this packet to a tracked id, if any. */
            for (int i = 0; i < s_watch_n; i++) {
                if (s_watch_id[i] == s_last_from) {
                    s_watch_last_ms[i] = s_last_heard_ms;
                    s_watch_pkts[i]++;
                    s_watch_rssi[i] = s_last_rssi;
                    if (hops == 0) {
                        s_watch_direct_ms[i]   = s_last_heard_ms;
                        s_watch_direct_rssi[i] = s_last_rssi;
                    }
                    break;
                }
            }
        } else {
            s_runts++; /* heard energy, not a mesh packet — count separately */
        }
    }
    s_radio->startReceive();
}


/* Append " watch=<id>:<age>/<pkts>@<rssi>" per tracked id. A never-heard id
 * reports "never" — NOT 0, which would read as "heard just now" and is exactly
 * the degraded-value-overlapping-the-healthy-domain trap. Nothing is appended
 * when the list is empty, so the host can tell "not configured" from
 * "configured but silent" — two facts a single 0 would merge. */
static void appendWatch(char *out, int out_len) {
    if (s_watch_n <= 0 || out_len <= 1) return;
    unsigned long now = millis();
    int used = (int)strlen(out);
    if (used >= out_len - 1) return;

    /* ⚠️ snprintf returns what it WOULD have written, not what it did. Advancing
     * `used` by that value on a truncating call pushes it past the buffer, and
     * (out_len - used) then goes NEGATIVE — as a size_t that is enormous, and the
     * next call writes off the end. So every advance is bound-checked before the
     * next write. On truncation we stop with a still-valid NUL-terminated string
     * rather than corrupting memory on a device whose job is to not crash. */
    int n = snprintf(out + used, (size_t)(out_len - used), " watch=");
    if (n < 0) return;
    used += n;
    if (used >= out_len - 1) return;

    for (int i = 0; i < s_watch_n; i++) {
        if (s_watch_last_ms[i] == 0UL) {
            /* NEVER heard since radio start — reported as "never", never as 0,
             * which downstream would read as "heard just now". */
            n = snprintf(out + used, (size_t)(out_len - used), "%s!%08x:never",
                         i ? "," : "", (unsigned)s_watch_id[i]);
        } else {
            n = snprintf(out + used, (size_t)(out_len - used),
                         "%s!%08x:%lu/%lu@%.0f",
                         i ? "," : "", (unsigned)s_watch_id[i],
                         (now - s_watch_last_ms[i]) / 1000UL,
                         s_watch_pkts[i], (double)s_watch_rssi[i]);
        }
        if (n < 0) return;
        used += n;
        if (used >= out_len - 1) return;
    }
    /* DIRECT-only view, emitted as its OWN field rather than folded into the
     * watch token above. Two reasons: the host-side parser for `watch=` is
     * shipped and would break on a changed token shape (reader/writer pairs
     * wire together or fail together), and the two facts answer different
     * questions — `watch=` is "traffic bearing this id reached me by ANY
     * path", `direct=` is "the originator's own transmission reached me".
     * A node can be watch-heard at a strong RSSI and direct-never: that is a
     * NEARER node rebroadcasting it, and reading the first as a link budget
     * is how you site a digipeater in the wrong place. */
    if (used < out_len - 1) {
        n = snprintf(out + used, (size_t)(out_len - used), " direct=");
        if (n < 0) return;
        used += n;
        if (used >= out_len - 1) return;
        for (int i = 0; i < s_watch_n; i++) {
            if (s_watch_direct_ms[i] == 0UL) {
                n = snprintf(out + used, (size_t)(out_len - used), "%s!%08x:never",
                             i ? "," : "", (unsigned)s_watch_id[i]);
            } else {
                n = snprintf(out + used, (size_t)(out_len - used),
                             "%s!%08x:%lu@%.0f",
                             i ? "," : "", (unsigned)s_watch_id[i],
                             (now - s_watch_direct_ms[i]) / 1000UL,
                             (double)s_watch_direct_rssi[i]);
            }
            if (n < 0) return;
            used += n;
            if (used >= out_len - 1) return;
        }
    }
    if (s_watch_dropped > 0)
        snprintf(out + used, (size_t)(out_len - used), " watch_dropped=%d",
                 s_watch_dropped);
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
        appendWatch(out, out_len);
        return;
    }
    snprintf(out, out_len,
             "mesh_heard_age_s: %lu (heard %lu pkts, crc_err %lu, runts %lu, "
             "last from=!%08x to=!%08x ch=0x%02x rssi=%.0f snr=%.1f hops=%d)",
             (now - s_last_heard_ms) / 1000UL, s_heard, s_crc_err, s_runts,
             (unsigned)s_last_from, (unsigned)s_last_to, (unsigned)s_last_ch,
             (double)s_last_rssi, (double)s_last_snr, s_last_hops);
    appendWatch(out, out_len);
}

unsigned long loraEarsHeardCount() { return s_heard; }

long loraEarsHeardAgeS() {
    if (!s_available) return -1;
    /* Same honest lower bound as loraEarsStats: never-heard reports time
     * since radio start so a deaf node still reads as aging. */
    unsigned long last = s_heard ? s_last_heard_ms : s_started_ms;
    return (long)((millis() - last) / 1000UL);
}

#endif /* WIRECLAW_LORA_SX1262 */
