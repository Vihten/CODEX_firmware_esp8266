# CODEX Firmware: ESP32-S3 N16R8

This repository now targets ESP32-S3-WROOM-1-N16R8 only: 16 MiB flash,
8 MiB octal PSRAM, GPIO18 LED data and a 30 FPS target.

- `firmware.bin`: raw ESP32-S3 OTA application image.
- `manifest.json`: matching S3 board, version, byte size and SHA-256.
- [Standalone source and instructions](esp32-s3/README.md).
- [Windows application and LAN agent](https://github.com/Vihten/CODEX_app).

The former ESP8266 binary/compressed binary and root manifest have been replaced.
Old versions remain in Git history, but are no longer the active release. Do not
flash the current binary onto an ESP8266. The repository name is retained to avoid
breaking firmware URLs already stored in controllers.

The `esp32-s3/manifest.json` and `esp32-s3/firmware.bin` copies remain intentionally:
the installed S3 already polls that URL. Root and compatibility copies must always
contain the same image/version/hash. The published manifests currently point to
the compatible `esp32-s3/firmware.bin` endpoint.

## Current Release

Version `1791971169630` introduces a Ukrainian controller portal with Overview,
Agents and Settings tabs. It adds visible-only live status with bounded polling,
agent filters, brightness/day presets and downloadable diagnostics. Existing
power, schedule, color, Wi-Fi, restart and GitHub OTA commands remain native POST
forms. Unsaved fields are not replaced by background updates. Rendering retains
the 30 FPS target and subpixel motion; static RAM remains 63,120 bytes.
Validation: 317 tests passed (three optional tests skipped), eight portal-script
tests passed, static analysis and the S3 PlatformIO build passed. Desktop/mobile
layout was checked, including escaped agent names and 12-agent HTML fixtures.
The initial portal version `1791970668113` completed Wi-Fi OTA with both agents
reconnected and settings preserved. Twelve status requests over about 35 seconds
had no failures (maximum latency 278 ms), with 29.37 measured FPS at a target of
30. Six page requests all returned HTTP 200 (60 ms to 1.60 s). No LED errors,
Wi-Fi disconnects or restarts were observed. The current follow-up removes an
irrelevant new-agent notice from Settings. It also completed Wi-Fi OTA: the S3
reported version `1791971169630`, both agents online, working matrix, 175,968
bytes free heap and `Up to date` after its post-boot check. The corrected Settings
page was checked at mobile width with no horizontal overflow or stray notice.
These are short observations, not a long-duration acceptance test.

## Previous Release

Version `1791967990771` adds subpixel meteor, spark and wake rendering and adjacent
sky-frame blending at the existing 30 FPS. It retains the agent-page improvements:
clear status, separate activity
and allowance readings, a compact native color swatch, live HEX readout and
responsive controls. It uses RMT DMA,
render ownership on core 1 and networking on core 0. The portal includes per-agent
activity and meteor colors, one schedule form, brightness, restart and three
Matrix power buttons. Capacity is 12 agent slots, including the requested eight.

The standalone source build uses ESP-IDF's minimal dependency graph with an
explicit LED-driver dependency, avoiding unused LCD components. A fresh build
passed. The previous release was installed over USB; local
rebuilds can differ in SDK linkage/build metadata and are not byte-identical.

The preceding version 1791961274325 was downloaded by S3 directly from GitHub over Wi-Fi, hash-verified,
installed into its OTA slot and booted successfully on 2026-10-14. HTTP returned
the new version; both real agents reconnected, Wi-Fi settings survived and the
post-boot check reported Up to date. No USB flashing was used for this update.
The current application suite passed 316 tests (three optional tests skipped),
including native subpixel math and actual renderer checks. This subpixel release
then completed Wi-Fi OTA on 2026-10-14 and reconnected both real agents. A short
31-second profile measured 29.28 actual FPS against the 30 FPS target, with a
maximum sampled frame time of 33379 us, no sampled LED errors, no restarts and
no Wi-Fi disconnects. Lowest sampled free heap was 131920 bytes during the
post-boot HTTPS manifest check; after it completed, heap returned to about 177 KB.
This short observation is not a long-duration or eight-agent acceptance test.
This is not a completed one-hour PSU/matrix or eight-agent load test, and the
rollback failure path has not been deliberately exercised. Publishing does not
force an update when the controller already has the same version.

No Codex login token, account password or Wi-Fi station credentials are shipped.
The account fingerprint is only a non-secret telemetry routing identifier.
LAN telemetry and the portal must stay on a trusted network.
