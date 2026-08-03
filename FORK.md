# dude-claw — MeshForge fork of WireClaw

**Upstream**: https://github.com/M64GitHub/WireClaw (MIT) · forked at `ad84614` (v0.4.0)
**Deploy branch**: `dudeclaw` · version marker `0.4.0+dudeclaw.N` (the fleet's
`+mf.N` pin convention; `_ion.discover` reports it, which is the deploy check)

⚠️ **The marker versions the SOURCE, not the build env.** Two claws reporting
the same `+dudeclaw.N` may still be running different firmware (LongFast vs
SHORT_TURBO vs agent). It answers "is this claw on the current source?" and
cannot answer "is this claw on the right variant?" — for that, ask the device
what it is doing (`lora_stats`, which reports what it actually hears).

## Why a fork

The MeshForge dude-claw edge node is a **Heltec WiFi LoRa 32 V4** — it has a
0.96" SSD1306 OLED and a white status LED that stock WireClaw (built for bare
ESP32 devkits) leaves dark, and its NATS bus needs token auth. Both gaps are
submitted upstream as PRs; the fork exists to run them **now**, and to carry
any board-specific work that is out of scope for upstream.

## Branch model (three lanes)

| Branch | Role | Rule |
|--------|------|------|
| `main` | mirror of upstream main | never commit here; advance only by fetching upstream |
| `pr/*` (`git branch --list 'pr/*'`) | upstream-bound feature work | feature code lives HERE — and **fixes to it live here too, never on `dudeclaw`** |
| `dudeclaw` | deployed firmware | `main` + a merge of each open `pr/*` + **fork-only commits that live here and nowhere else** |

### ⚠️ The old invariant was retired 2026-08-02 — it never described reality

This section used to read: *"`dudeclaw` never carries feature code that isn't
on a `pr/*` branch or in upstream `main`. The residue commit is FORK.md +
version.h ONLY … the moment a hand-edit lands on `dudeclaw`, the one-dev
divergence trap is back — don't."*

**It was aspirational, not descriptive, and the gap was never small.** Measured
2026-08-02 (`git rev-list main..dudeclaw --no-merges`, then checking each sha
against every `pr/*` branch): of 27 non-merge commits ahead of `main`, **10 were
dudeclaw-only, and only 2 of those were pure version-marker residue.** The other
8 were real code that had been accumulating for months:

| Kind | Commits | Why it is not on a `pr/*` branch |
|---|---|---|
| fleet-specific features | `dc27b28` LoRa watch list · `98cdab3` `config_set` | encode this fleet's operating model; upstream has no use for them |
| board/profile build config | `eac93a3` BLE retired from the V4 profile · `0f5a22e` SHORT_TURBO env | tuning for specific deployed hardware, not a general feature |
| off-device tooling | `b9da37d` `tools/prompt_eval.py` | not firmware at all |
| ~~drift — should have gone on a `pr/*`~~ **REPAID 2026-08-02** | `540969f` anomaly warm-up fix → `pr/anomaly-witness` · `89311b6` agent FS charter → `pr/agent-profile` · `ff5441b` agent honesty clause → new `pr/agent-honesty` | edits to code another branch owns, once committed only on the deploy branch. Now carried by their owners as well. |

The last row was a judgment call, not a mechanical result: it is where a commit
touched a file owned by a `pr/*` branch or by upstream `main`. The other rows
touch only files that exist because of this fork.

**Why repaying it mattered, since nothing on hardware changed:** those two
`pr/*` branches were carrying *defective versions of the features they exist to
propose*. `pr/anomaly-witness` had the warm-up that scores before the variance
estimate converges — where `fabsf()` makes cooling alarm exactly like
overheating — and `pr/agent-profile` had the agent env with no `data_agent/`,
so it would build against a prompt advertising tools that build refuses.
Submitting either would have offered upstream a known-broken feature while the
fix sat on the deploy branch. The honesty clause went to its **own** branch
rather than into `pr/agent-profile`: it is not agent-specific (it stops any
build claiming actions it did not perform), and burying a general improvement
inside a profile PR hides it.

The deploy branch keeps its own copies — those shas are still fork-only, and
the audit still lists them. Repayment means the owner branch now carries the
content too, not that history was rewritten.

Keeping a rule nobody follows is worse than having none: it made every commit
here look like a violation, so the **three** that genuinely are (the drift row)
were invisible among the **five** that are just how this fork works. And the
"Why a fork" section above always said the fork exists *"to carry any
board-specific work that is out of scope for upstream"* — the invariant simply
left that work nowhere legal to live.

### The rule that actually applies

1. **Upstream-bound work lives on a `pr/*` branch — including its fixes.**
   If code is owned by a `pr/*` branch, a fix to it goes THERE and `dudeclaw`
   re-merges. This is the half of the old invariant worth keeping: it is what
   stops one reviewable feature from having two divergent copies.
2. **Fork-only work lives directly on `dudeclaw`.** Fleet-specific behavior,
   deployed-hardware build profiles, and off-device tooling are the accepted
   divergence — that is what the fork is *for*. Say so in the commit message.
3. **Never a bare hand-edit.** Fork-only is not license to skip the reasoning;
   these commits are the ones no upstream reviewer will ever read, so the
   message carries the whole justification.

**What this costs, stated plainly:** `dudeclaw` is **no longer reproducible**
from `main` + `pr/*` alone. It is **advanced, never recreated** — see the
advance recipe below. The old recipe's `git checkout -B dudeclaw main` would have
silently destroyed every fork-only commit, including the SHORT_TURBO env
whose loss would put a reflashed ST claw back on LongFast, deaf on the one
segment it exists to hear.

⚠️ **Those counts are a dated snapshot, not an invariant** — the fork-only set
GROWS (it was 10 on the morning of 2026-08-02 and 12 by that evening, since
documentation commits are themselves fork-only). Never quote the number from
this file; **run the audit below**, which is the live answer. The per-commit
table above stays useful because shas do not drift.

**Compensating guard** — the reproducibility guarantee is gone, so the
auditability has to be explicit. This enumerates every commit that exists only
here, which is the list that must survive any rebase/rebuild:

```bash
for sha in $(git rev-list main..dudeclaw --no-merges); do
  [ -z "$(git branch --list 'pr/*' --contains $sha)" ] \
    && git log -1 --oneline $sha
done
```

(History note: `dudeclaw.1-deployed` tag = the original pre-PR fork commit
`428f10c` with `DUDECLAW_*` flag names; the hardware flashed 2026-06-11 runs
that build. From `+dudeclaw.2` on, the deploy branch uses the PR-shaped
`WIRECLAW_*` code.)

## Advance recipe (run after any `pr/*` or `main` movement)

⚠️ **`dudeclaw` is advanced by merging, never recreated.** The previous recipe
opened with `git checkout -B dudeclaw main`, which discards everything the
branch carries that `main` does not — every fork-only commit. Do not
reintroduce it.

```bash
# 1. advance the mirror
git fetch origin && git checkout main && git merge --ff-only origin/main

# 2. note what is fork-only BEFORE touching anything (see the audit above);
#    every one of these must still be listed when you are done
git checkout dudeclaw

# 3. advance the deploy branch — merge, never -B
git merge --no-edit main
#    then re-merge only the pr/* branches that have actually moved, ONE AT A
#    TIME (an octopus merge of all of them dies on the first conflict with no
#    indication of which branch caused it). This loop is a no-op whenever every
#    pr/* is already contained — it does real work only after one gains a fix.
for b in $(git branch --no-merged dudeclaw --list 'pr/*' --format='%(refname:short)'); do
  git merge --no-edit "$b" || { echo "CONFLICT merging $b — resolve, then re-run"; break; }
done

# 4. bump the marker (include/version.h -> +dudeclaw.N+1) and commit

# 5. build EVERY deployed env — a green default env proves nothing about the
#    others, and each claw runs a different one
pio run -e esp32-s3-heltec-v4 -e esp32-s3-heltec-v4-st -e esp32-s3-heltec-v4-agent
```

Step 5 matters because the three V4 envs are genuinely different builds
(`-st` overrides the modem params; `-agent` swaps the FS charter and the LLM
buffers). Building one and flashing another is a real way to brick a claw's
purpose without any error.

## Convergence plan — how this gets back to main

Per-PR state machine, checked whenever the claw is touched (no cron; solo dev):

- **MERGED upstream** → on the next upstream adoption, that `pr/*` branch is
  deleted and `dudeclaw` picks the work up from `main` instead.
  ⚠️ **Every `pr/*` merging does NOT end the fork.** That was the old plan's
  endpoint ("`dudeclaw` = `main` + residue only … the fork becomes a plain
  checkout"), and it stopped being reachable the moment fork-only work landed.
  The floor is now `main` + the fork-only commits, which upstream will never
  take. Retiring the version marker is therefore off the table for as long as
  this fleet runs claws on more than one modem preset.
- **CHANGES REQUESTED** → fix on the `pr/*` branch (single source of truth),
  push, advance `dudeclaw`, bump `+dudeclaw.N`, reflash at the next natural
  claw touch.
- **LONG-LIVED (still unreviewed after ~90 days)** → stop planning around
  near-term convergence: same governance as the fleet's RNS/LXMF forks — adopt
  upstream releases by tag-merge into `main`, re-merge `pr/*` (rebasing them
  onto the new main), re-verify the guarded-optional property (stock envs build
  unchanged), canary the claw before calling it adopted.

  This state describes **our** planning posture, not anyone's responsiveness.
  Upstream is a volunteer MIT project under no obligation to review anything on
  our schedule; a PR sitting unreviewed is a normal outcome, not a failure, and
  this file should never read as a complaint about it. The 90 days is just the
  point where it is more honest to run the fork as long-lived than to keep
  calling it temporary.

Upstream adoption procedure (any state): `git fetch upstream && git merge
<tag>` into `main`, rebase each open `pr/*`, run the advance recipe, re-run the
fork-only audit and confirm the list is unchanged, app-only reflash, confirm
`_ion.discover` reports the new marker.

## Build / deploy

**One env per claw — check which before you flash.** They are not
interchangeable: the ears profile, the SHORT_TURBO ears profile and the agent
profile differ in modem parameters, FS charter and LLM buffers.

| env | role |
|---|---|
| `esp32-s3-heltec-v4` | LongFast NATS-edge ears |
| `esp32-s3-heltec-v4-st` | SHORT_TURBO ears (905.75 MHz / BW 500 / SF 7) |
| `esp32-s3-heltec-v4-agent` | on-device agent against a local Ollama |

```bash
pio run -e <env>
# app-only reflash (LittleFS config survives — no reprovisioning):
esptool --chip esp32s3 --port /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00 \
    write-flash 0x10000 .pio/build/<env>/firmware.bin
```

⚠️ **Address the board by `/dev/serial/by-id/`, never `ttyACMn`/`ttyUSBn`** —
enumeration order is not stable across reboots, a Meshtastic node is also an
ESP32-S3, and at least one host box has a live RNode on USB that `esptool`
would hard-reset (or worse) if aimed at the wrong path. The by-id name embeds
the MAC, so it doubles as the board identity check.

⚠️ A **blank** board needs the full write, not the app-only one — it has no
bootloader, partition table or filesystem yet, and no WiFi credential, so it
cannot reach the bus to be configured remotely:

```bash
esptool ... write-flash 0x0 .pio/build/<env>/firmware.factory.bin \
                        0x290000 .pio/build/<env>/littlefs.bin
```

Bake `config.json` into that filesystem image by pointing `PLATFORMIO_DATA_DIR`
at a directory **outside the repo** (`pio run -t buildfs`), so the WiFi
credential cannot be committed. Verify afterwards that no `config.json` and no
SSID exist in the tree.

⚠️ apt esptool is dfsg-stripped (no S3 stubs) — use pipx. Flash layout stays
the stock 4MB `partitions.csv` precisely so app-only reflashes preserve the
LittleFS config.

⚠️ `Serial` is **not** on the S3's native USB in these builds — a boot-log
capture over the CDC port returns zero bytes. Verify a flashed claw over NATS
(`_ion.discover` for the marker, `<device>.tool_exec` for tools), not serial.

## Governance

Same rules as the fleet's RNS/LXMF forks: track upstream releases, merge by
tag, re-verify the guarded-optional property (stock envs build byte-identically),
and keep the wire/protocol surface unchanged — this fork adds tools and auth,
it never alters existing subjects or reply shapes.
