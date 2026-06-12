# dude-claw — MeshForge fork of WireClaw

**Upstream**: https://github.com/M64GitHub/WireClaw (MIT) · forked at `ad84614` (v0.4.0)
**Fork branch**: `dudeclaw` · version marker `0.4.0+dudeclaw.N` (the fleet's
`+mf.N` pin convention; `_ion.discover` reports it, which is the deploy check)

## Why a fork

The MeshForge dude-claw edge node is a **Heltec WiFi LoRa 32 V4** — it has a
0.96" SSD1306 OLED and a white status LED that stock WireClaw (built for bare
ESP32 devkits) leaves dark. This fork adds display + LED support as a
**guarded optional**: every change is behind `DUDECLAW_*` build flags carried
only by the new `esp32-s3-heltec-v4` env, so the stock envs (esp32-c6,
esp32-s3, esp32-c3) build byte-identically to upstream.

## Changes (+dudeclaw.1)

| Piece | What |
|-------|------|
| `include/display.h` / `src/display.cpp` | SSD1306 128x64 status screen: device name + NATS marker, IP + RSSI, chip temp/heap/uptime, and 2 remote-writable metric rows. Vext rail power-on (GPIO36 active LOW), panel reset (GPIO21), I2C 17/18. Init failure → honest "running headless" serial log, all calls no-op. Metric rows older than 30 min draw a `(old)` suffix — a dead pusher must not masquerade as live data. |
| `display_print` tool | `{"row":0..1,"text":"..."}` over `tool_exec`; empty text clears the row. On builds/boards without a panel returns `Error: no display on this device` (honest absent, never silent ok). Registered in TOOLS_JSON (LLM), `toolExecute` dispatch, and the `_ion.discover` tools list. |
| `led()` white-LED branch | V4 has no WS2812; `DUDECLAW_WHITE_LED=35` maps RGB intensity (max channel) onto the board's white LED via PWM so `led_set` actuations are visible. |
| `platformio.ini` | New `[env:esp32-s3-heltec-v4]` carrying the pin flags + ThingPulse SSD1306 lib. Stock 4MB `partitions.csv` retained so an **app-only** reflash (`esptool write-flash 0x10000 firmware.bin`) preserves littlefs config. |
| `include/version.h` | `0.4.0+dudeclaw.1` |

## Build / deploy

```bash
pio run -e esp32-s3-heltec-v4
# app-only reflash (config survives):
esptool --chip esp32s3 --port /dev/ttyACM0 write-flash 0x10000 \
    .pio/build/esp32-s3-heltec-v4/firmware.bin
```

## Upstream-PR candidates

- The display module + `display_print` tool (generalized board-pin config).
- NATS token auth (the gap that forces network-layer protection on the bus).

## Governance

Same rules as the fleet's RNS/LXMF forks: track upstream releases, merge by
tag, re-verify the guarded-optional property (stock envs byte-identical), and
keep the wire/protocol surface unchanged — this fork adds a tool, it never
alters existing subjects or reply shapes.
