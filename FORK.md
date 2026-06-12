# dude-claw — MeshForge fork of WireClaw

**Upstream**: https://github.com/M64GitHub/WireClaw (MIT) · forked at `ad84614` (v0.4.0)
**Deploy branch**: `dudeclaw` · version marker `0.4.0+dudeclaw.N` (the fleet's
`+mf.N` pin convention; `_ion.discover` reports it, which is the deploy check)

## Why a fork

The MeshForge dude-claw edge node is a **Heltec WiFi LoRa 32 V4** — it has a
0.96" SSD1306 OLED and a white status LED that stock WireClaw (built for bare
ESP32 devkits) leaves dark, and its NATS bus needs token auth. Both gaps are
submitted upstream as PRs; the fork exists to run them **now**, and to carry
any future board-specific work the upstream doesn't want.

## Branch model + THE INVARIANT (solo-dev divergence guard)

| Branch | Role | Rule |
|--------|------|------|
| `main` | mirror of upstream main | never commit here; advance only by fetching upstream |
| `pr/display-status-screen` | upstream PR #15 | feature code lives HERE; fix review feedback HERE |
| `pr/nats-token-auth` | upstream PR #16 | feature code lives HERE; fix review feedback HERE |
| `pr/vbat-battery-read` | PR-pending (stacked ON pr/display — the heltec-v4 env lives there); open upstream once #15 merges | battery_read tool; V4 VBAT wiring verified from the official V4.2.0 datasheet |
| `pr/lora-mesh-ears` | fork-first (stacked ON pr/vbat); upstream candidacy after the stack lands | RX-only SX1262 listener + lora_stats; never transmits |
| `pr/lora-mesh-voice` | fork-first (stacked ON pr/lora-mesh-ears) | mesh_send TX (AES-CTR/protobuf/FEM); secret-safe (public default, private PSK runtime-only); airtime-guarded |
| `dudeclaw` | deployed firmware | **rebuilt, never hand-edited**: `main` + merge each open `pr/*` + the residue commit |

**THE INVARIANT: `dudeclaw` never carries feature code that isn't on a `pr/*`
branch or in upstream `main`. The residue commit is FORK.md + version.h ONLY.**
Every line of fork code therefore lives in exactly one reviewable place; the
deploy branch is always mechanically reproducible. The moment a hand-edit
lands on `dudeclaw`, the one-dev divergence trap is back — don't.

(History note: `dudeclaw.1-deployed` tag = the original pre-PR fork commit
`428f10c` with `DUDECLAW_*` flag names; the hardware flashed 2026-06-11 runs
that build. From `+dudeclaw.2` on, the deploy branch uses the PR-shaped
`WIRECLAW_*` code.)

## Rebuild recipe (mechanical — run after any pr/* or main movement)

```bash
git fetch origin && git checkout main && git merge --ff-only origin/main
git checkout -B dudeclaw main
git merge --no-edit pr/lora-mesh-voice pr/nats-token-auth   # voice tops the display->vbat->ears->voice stack: one merge brings all four
git cherry-pick <residue>          # or re-commit FORK.md + version.h (+dudeclaw.N+1)
pio run -e esp32-s3-heltec-v4      # must be green before any flash
```

## Convergence plan — how this gets back to main

Per-PR state machine, checked whenever the claw is touched (no cron; solo dev):

- **MERGED upstream** → on the next upstream adoption, that `pr/*` branch is
  deleted and `dudeclaw` rebuilds without it. Both merged → `dudeclaw` =
  `main` + residue only; if upstream ever ships a release containing both,
  retire the version marker and the fork becomes a plain checkout.
- **CHANGES REQUESTED** → fix on the `pr/*` branch (single source of truth),
  push, rebuild `dudeclaw`, bump `+dudeclaw.N`, reflash at the next natural
  claw touch.
- **STALE (>90 days, no maintainer response)** → the fork becomes long-lived:
  same governance as the fleet's RNS/LXMF forks — adopt upstream releases by
  tag-merge into `main`, re-merge `pr/*` (rebasing them onto the new main),
  re-verify the guarded-optional property (stock envs build unchanged), canary
  the claw before calling it adopted.

Upstream adoption procedure (any state): `git fetch upstream && git merge
<tag>` into `main`, rebase each open `pr/*`, run the rebuild recipe,
app-only reflash, confirm `_ion.discover` reports the new marker.

## Build / deploy

```bash
pio run -e esp32-s3-heltec-v4
# app-only reflash (config survives):
esptool --chip esp32s3 --port /dev/ttyACM0 write-flash 0x10000 \
    .pio/build/esp32-s3-heltec-v4/firmware.bin
```

⚠️ apt esptool is dfsg-stripped (no S3 stubs) — use pipx. Flash layout stays
the stock 4MB `partitions.csv` precisely so app-only reflashes preserve the
LittleFS config.

## Governance

Same rules as the fleet's RNS/LXMF forks: track upstream releases, merge by
tag, re-verify the guarded-optional property (stock envs build byte-identically),
and keep the wire/protocol surface unchanged — this fork adds tools and auth,
it never alters existing subjects or reply shapes.
