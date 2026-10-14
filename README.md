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
including native subpixel math and actual renderer checks. Final OTA observation
of the current release is recorded after deployment; do not infer it from a build.
This is not a completed one-hour PSU/matrix or eight-agent load test, and the
rollback failure path has not been deliberately exercised. Publishing does not
force an update when the controller already has the same version.

No Codex login token, account password or Wi-Fi station credentials are shipped.
The account fingerprint is only a non-secret telemetry routing identifier.
LAN telemetry and the portal must stay on a trusted network.
