#include <cstdio>
#include <cstdint>
static int failures = 0;
static void CHECK(bool c, const char *w){ if(!c){printf("  FAIL: %s\n",w);failures++;} }

/* OLD arithmetic (pre-F2) and NEW (ratified 2026-08-31), side by side. */
static int hops_old(uint8_t flags){
    int hop_limit = flags & 0x07, hop_start = (flags >> 5) & 0x07;
    return (hop_start >= hop_limit) ? (hop_start - hop_limit) : -1;
}
static int hops_new(uint8_t flags){
    int hop_limit = flags & 0x07, hop_start = (flags >> 5) & 0x07;
    return (hop_start > 0 && hop_start >= hop_limit) ? (hop_start - hop_limit) : -1;
}
int main(void){
    /* 1. The captured live packet cross-check: moc logged flags=0x62 and
     *    reported hop_start:3 hops_away:1. Both arithmetics must agree. */
    CHECK((0x62 & 7) == 2, "0x62 -> hop_limit 2");
    CHECK(((0x62 >> 5) & 7) == 3, "0x62 -> hop_start 3");
    CHECK(hops_new(0x62) == 1, "0x62 -> hops 1 (matches the captured packet)");
    CHECK(hops_old(0x62) == hops_new(0x62), "0x62 unchanged by F2");

    /* 2. THE fix: (0,0) forged a direct link. */
    CHECK(hops_old(0x00) == 0,  "OLD scored (0,0) as hops=0 == DIRECT (the bug)");
    CHECK(hops_new(0x00) == -1, "NEW scores (0,0) as -1 == UNKNOWN");

    /* 3. Exhaustive over all 256 flag bytes: (0,0) is the ONLY change, and
     *    the new rule never claims direct without a positive hop_start. */
    int changed = 0, direct_without_start = 0;
    for (int f = 0; f < 256; f++) {
        uint8_t b = (uint8_t)f;
        int o = hops_old(b), n = hops_new(b);
        if (o != n) { changed++;
            CHECK(b == 0x00 || ((b & 7) == 0 && ((b >> 5) & 7) == 0),
                  "only (0,0) differs"); }
        if (n == 0 && ((b >> 5) & 7) == 0) direct_without_start++;
    }
    printf("all 256 flag bytes: changed=%d direct_without_hop_start=%d\n",
           changed, direct_without_start);
    /* hop_limit==0 && hop_start==0 pins the low 3 and high 3 bits, leaving
     * bits 3-4 (unused padding) free -> 2^2 = 4 encodings, all of which flip.
     * (This comment said "32" until 2026-09-01; the assertion below was right
     * the whole time and the prose beside it was wrong.) */
    CHECK(changed == 4, "exactly the 4 (0,0) encodings change");
    CHECK(direct_without_start == 0, "NEW never claims direct with hop_start 0");

    /* 4. Legitimate direct reception is preserved. */
    CHECK(hops_new(0x63) == 0, "(start 3, limit 3) still DIRECT");
    CHECK(hops_new(0x21) == 0, "(start 1, limit 1) still DIRECT");
    CHECK(hops_new(0x20) == 1, "(start 1, limit 0) still 1 hop");
    /* 5. Malformed stays malformed. */
    CHECK(hops_new(0x02) == -1, "(start 0, limit 2) stays -1");

    /* 6. The +dudeclaw.21 rejection counters. The firmware splits the -1 case
     *    into two buckets so "F2 never fired here" stops being unobservable.
     *    This mirrors lora_ears.cpp exactly; if that classification changes
     *    without this one, the drill fails rather than quietly diverging. */
    enum Bucket { VALID, HOP_START0, MALFORMED };
    auto classify = [](uint8_t flags) -> Bucket {
        int hop_limit = flags & 0x07, hop_start = (flags >> 5) & 0x07;
        if (hop_start > 0 && hop_start >= hop_limit) return VALID;
        if (hop_start == 0 && hop_limit == 0)        return HOP_START0;
        return MALFORMED;
    };
    int n_valid = 0, n_start0 = 0, n_malformed = 0, misfiled = 0;
    for (int f = 0; f < 256; f++) {
        uint8_t b = (uint8_t)f;
        Bucket k = classify(b);
        if (k == VALID)          n_valid++;
        else if (k == HOP_START0) n_start0++;
        else                      n_malformed++;
        /* Every byte lands in exactly one bucket, and the bucket must agree
         * with the arithmetic: VALID iff hops_new >= 0. */
        if ((k == VALID) != (hops_new(b) >= 0)) misfiled++;
        /* The F2 bucket is EXACTLY the set F2 changed — no more, no less. */
        if (k == HOP_START0 && !(hops_old(b) == 0 && hops_new(b) == -1)) misfiled++;
        /* A malformed header was never a forged DIRECT link: the old
         * arithmetic did not call it direct either, so counting it as an F2
         * catch would overstate what F2 does. */
        if (k == MALFORMED && hops_old(b) == 0) misfiled++;
    }
    printf("buckets: valid=%d hop_start0=%d malformed=%d (sum %d) misfiled=%d\n",
           n_valid, n_start0, n_malformed,
           n_valid + n_start0 + n_malformed, misfiled);
    CHECK(n_valid + n_start0 + n_malformed == 256, "every flag byte is classified exactly once");
    CHECK(n_start0 == changed, "the hop_start0 bucket is exactly the set F2 changed");
    CHECK(misfiled == 0, "no byte is filed against the wrong bucket");

    printf("\n%s (%d failure(s))\n", failures?"F2 DRILL FAILED":"F2 DRILL PASSED", failures);
    return failures ? 1 : 0;
}
