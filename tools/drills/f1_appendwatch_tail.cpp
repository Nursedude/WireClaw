/* ---- end extract ---- */

static const char *BASE =
  "mesh_heard_age_s: 5 (heard 1300 pkts, crc_err 8, runts 0, "
  "last from=!16cd7438 to=!ffffffff rssi=-104 snr=4.2 hops=0)";

static int failures = 0;
static void CHECK(bool cond, const char *what) {
    if (!cond) { printf("  FAIL: %s\n", what); failures++; }
}

/* 12 ids, all heard, all direct, worst-case-ish widths. */
static void seed12(void) {
    s_watch_n = 12;
    /* TRUE worst case, from the field TYPES rather than a typical sample:
     * age is (millis-last)/1000, so 7 digits at the 49.7-day wrap; pkts is an
     * unsigned long (10 digits); rssi prints %.0f (4 chars at -128). A narrow
     * sample understates the buffer requirement, which is exactly how a cap
     * gets sized too small in the first place. */
    for (int i = 0; i < 12; i++) {
        s_watch_id[i]          = 0x32962f10u + (uint32_t)i;
        s_watch_last_ms[i]     = 1UL;              /* age -> 4294967 (7 dig) */
        s_watch_pkts[i]        = 4294967295UL;     /* 10 digits */
        s_watch_rssi[i]        = -128.0f;
        s_watch_direct_ms[i]   = 1UL;
        s_watch_direct_rssi[i] = -128.0f;
    }
    s_watch_dropped = 99;   /* ids offered past MAX — coexists with n == MAX */
}

#define GUARD 64
/* Run appendWatch into a buffer of exactly `n` usable bytes, with a poisoned
 * guard region behind it. Returns the resulting string in `outCopy`. */
static void runInto(int n, char *outCopy, int outCopyLen, bool *guardOk) {
    char *heap = (char *)malloc((size_t)n + GUARD);
    memset(heap, (int)0xAA, (size_t)n + GUARD);
    snprintf(heap, (size_t)n, "%s", BASE);
    appendWatch(heap, n);
    *guardOk = true;
    for (int i = n; i < n + GUARD; i++)
        if ((unsigned char)heap[i] != 0xAAu) { *guardOk = false; break; }
    /* must always be NUL-terminated within n */
    bool term = false;
    for (int i = 0; i < n; i++) if (heap[i] == '\0') { term = true; break; }
    if (!term) { printf("  FAIL: not NUL-terminated at n=%d\n", n); failures++; }
    snprintf(outCopy, (size_t)outCopyLen, "%s", heap);
    free(heap);
}

int main(void) {
    char buf[4096];
    bool g;
    seed12();

    /* --- reference: the COMPLETE 12-id string --- */
    runInto(4096, buf, sizeof(buf), &g);
    int full_len = (int)strlen(buf);
    printf("complete 12-id stats string: %d chars\n", full_len);
    CHECK(g, "guard intact on the complete run");
    CHECK(strstr(buf, " cut=1") == NULL, "complete output carries NO cut=1");
    CHECK(strstr(buf, " watch=") != NULL, "complete output has watch=");
    CHECK(strstr(buf, " direct=") != NULL, "complete output has direct=");
    char complete[4096];
    snprintf(complete, sizeof(complete), "%s", buf);

    /* --- the OLD reply budget (768 - wrapper ~= 743) MUST truncate --- */
    runInto(743, buf, sizeof(buf), &g);
    CHECK(g, "guard intact at the old 743-byte budget");
    CHECK(strstr(buf, " cut=1") != NULL,
          "old 743-byte budget truncates AND says so");
    printf("at 743 (old reply budget): %s\n",
           strstr(buf, " cut=1") ? "cut=1 present" : "SILENT (defect)");

    /* --- the real cmdResponseBuf (1024) and the NEW reply budget --- */
    runInto(1024, buf, sizeof(buf), &g);
    CHECK(g, "guard intact at cmdResponseBuf 1024");
    CHECK(strcmp(buf, complete) == 0, "1024 yields the COMPLETE string");
    CHECK(strstr(buf, " cut=1") == NULL, "1024 carries no cut=1");

    /* --- exhaustive sweep: every size from 1 to full+64 --- */
    int silent_trunc = 0, overruns = 0, false_marks = 0;
    for (int n = 1; n <= full_len + 64; n++) {
        runInto(n, buf, sizeof(buf), &g);
        if (!g) overruns++;
        bool marked = (strstr(buf, " cut=1") != NULL);
        bool complete_now = (strcmp(buf, complete) == 0);
        if (!complete_now && !marked) {
            /* Truncated but silent. Legal ONLY when the buffer is too small to
             * hold the reserve at all (lim <= 1) or the watch list is skipped
             * entirely — those return before any watch data is claimed. */
            if (n > WATCH_CUT_RESERVE + 2 && strstr(buf, "watch=") != NULL)
                { silent_trunc++;
                  if (silent_trunc <= 3)
                      printf("  SILENT TRUNCATION at n=%d: [%s]\n", n, buf); }
        }
        if (complete_now && marked) false_marks++;
    }
    printf("sweep 1..%d: overruns=%d silent_truncations=%d false_marks=%d\n",
           full_len + 64, overruns, silent_trunc, false_marks);
    CHECK(overruns == 0, "no buffer overrun at any size");
    CHECK(silent_trunc == 0, "no SILENT truncation once watch data is claimed");
    CHECK(false_marks == 0, "cut=1 never appears on a complete string");

    /* --- the forged reading: clip mid-RSSI, confirm the witness fires --- */
    int clip = full_len - 3;   /* lands inside the final @-104 token */
    runInto(clip, buf, sizeof(buf), &g);
    CHECK(strstr(buf, " cut=1") != NULL, "mid-RSSI clip is witnessed");
    printf("mid-RSSI clip tail: ...%s\n",
           buf + (strlen(buf) > 40 ? strlen(buf) - 40 : 0));

    printf("\n%s (%d failure(s))\n", failures ? "DRILL FAILED" : "DRILL PASSED",
           failures);
    return failures ? 1 : 0;
}
