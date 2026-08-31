/* F1 native drill — the REAL appendWatch, extracted from src/lora_ears.cpp at
 * build time (lines 599-697) so this cannot drift into testing a stale copy. */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>

#define LORA_WATCH_MAX 12
static uint32_t      s_watch_id[LORA_WATCH_MAX];
static unsigned long s_watch_last_ms[LORA_WATCH_MAX];
static unsigned long s_watch_pkts[LORA_WATCH_MAX];
static float         s_watch_rssi[LORA_WATCH_MAX];
static unsigned long s_watch_direct_ms[LORA_WATCH_MAX];
static float         s_watch_direct_rssi[LORA_WATCH_MAX];
static int           s_watch_n = 0;
static int           s_watch_dropped = 0;
static unsigned long millis(void) { return 4294967295UL; }

/* ---- extracted verbatim from src/lora_ears.cpp ---- */
