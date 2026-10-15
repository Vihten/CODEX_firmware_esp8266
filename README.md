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

Version `1792047118711` adds a soft external shadow to the static body image
only. Canvas and container filters remain absent. Web preview now targets a
66 ms cadence (~15 FPS), with matching demand-only capture throttling. Frame
responses enable TCP_NODELAY best-effort on valid sockets to reduce potential
header/body delayed-ACK stalls. Requests stay sequential, bounded and paused
when hidden. A gain-keyed 256-entry LUT replaces repeated sRGB calculations,
with exhaustive tests proving identical output across all brightness values.
Physical animation renderer, frame/effect data and 30 FPS target are unchanged.

Validation: PlatformIO, targeted Dart analysis, 44 export/network/native preview
tests, 26 browser-script tests and 34 web-bundle tests passed. Desktop/mobile
preview and image-only shadow scope were checked. Before deployment, eight
frame requests took 126-222 ms; actual post-update timing is checked separately.

## Matte Plastic Highlight Release

Version `1792046508883` adds restrained plastic highlights from a separate CAD
specular pass. Only the positive reflected-light difference is blended into the
body, capped at 12 gray levels before downsampling, with dark creases protected.
Existing soft shadows, fine outlines, geometry, mirroring and silhouette remain.
The 1536 x 1100 PNG is 162,352 bytes; all asset rendering happens offline.
Animation renderer, animation/effect data, preview color encoding and CSS remain
hash-identical. Brightness settings and the 30 FPS target are unchanged.

Validation: PlatformIO build, 20 export/network tests, 24 script tests and 34
web-bundle tests passed; desktop and 390px mobile live previews were checked.
The original private CAD file hash is unchanged.

Wi-Fi OTA verified on 2026-10-15: version `1792046508883`, web digest
`1b24843267a33afbffd7c86982c2b6172dcbbbd739249a55d439d685a803b51c`,
and `Up to date`. The served PNG matches the prepared asset byte-for-byte.
Live preview showed the restrained highlights without a canvas filter; brightness
remained 44/255 and two agents reconnected. No LED error or Wi-Fi disconnect was
reported in this short post-update check; it was not a long stability test.

## Soft Contact Shadow Release

Version `1792046130754` adds subtle, soft contact shadows to the gray device
body. Shadow coverage is derived from the CAD creases, blurred offline and
offset consistently with the light direction. Thin outlines and all silhouette
alpha remain intact; the 1536 x 1100 PNG is 162,865 bytes. No shadow processing
runs on the MCU. Animation renderer, frame/effect data, browser color encoding,
preview CSS and brightness settings are unchanged; target remains 30 FPS.

Validation: PlatformIO build, targeted Dart analysis, 20 export/network tests,
24 script tests and 34 bundle tests passed. Desktop and 390px mobile previews
were visually checked. Animation/preview source and frame hashes are unchanged,
as is the private original CAD file.

Wi-Fi OTA verified on 2026-10-15: version `1792046130754`, web digest
`333347b228b4dd150ccbc11847deb7b01f6f420f9f1dfd23761cad4db02accb4`,
and `Up to date`. The device-served PNG matches the prepared file byte-for-byte.
Its live page shows the new shadows with no canvas filter; brightness remains
44/255 and two agents reconnected. No LED error or Wi-Fi disconnect was reported
during this short post-update verification; this was not a long stability test.

## Soft Outline Release

Version `1792045618143` softens and thins only the gray device body's CAD outlines.
Matching shaded and edged rasters isolate the linework; subpixel thinning and
reduced contrast retain engraved controls without a drawing-like black stroke.
The 1536 x 1100 PNG is 115,906 bytes. Camera, mirroring, aperture alignment and
silhouette alpha are unchanged. Animation renderer, frame data, color encoding,
preview CSS and physical brightness settings are unchanged; target remains 30 FPS.

Validation: PlatformIO build, 20 export/network tests, 24 script tests and 34 web
bundle tests passed. Raster comparison confirmed identical alpha and over one
million unchanged shaded pixels; desktop and 390px mobile views were inspected.
The original CAD remains private and its SHA-256 is unchanged.

Wi-Fi OTA verified on 2026-10-15: version `1792045618143`, web digest
`d1ab24143a88d38d59d3c8b3cb3acda4a190a012aa82dede766bd6deeafa75ed`,
and `Up to date`. The served PNG matched the prepared asset byte-for-byte.
The live page displayed the softer body with an unfiltered, aligned animation;
brightness remained 44/255 and both agents reconnected. No LED error or Wi-Fi
disconnect was reported during this short post-update check, not a soak test.

## Lighter Outlined Body Release

Version `1792044933681` slightly lightens only the gray device render and restores
shaded-with-edges CAD outlines, making the mounting holes, seams and symbols more
readable. The mirrored 1536 x 1100 antialiased PNG is 133,863 bytes. Animation
renderer, animation/effect frame data, browser color encoding and preview CSS
were hash-checked unchanged; no brightness, normalization or palette adjustment
was made. Target frequency remains 30 FPS.

Validation: 344 Flutter tests passed (three optional tests skipped), analysis,
PlatformIO, 24 script tests and 34 bundle tests passed. Raster tests now check
body brightness and dark outline/detail pixels outside the live display area;
the old dark image and an in-memory outline-erased image fail these checks.
Desktop and 390px mobile previews were visually checked with live controller data.

Wi-Fi OTA completed on 2026-10-15. The S3 reported `1792044933681`, web digest
`69e9a3f2f747b971048597a1ad22e9824d5babb190c596702468c448b88aa398`, and `Up to date`.
The device served the matching 133,863-byte PNG with HTTP 200 in 0.62 seconds.
The live page showed lighter gray body details and restored outlines; the canvas
still had no filter. Both active agents reconnected and brightness stayed 44/255.
No LED error or Wi-Fi disconnect was reported in this short verification.

## Mirrored Preview Release

Version `1792043877479` mirrors the device body horizontally and improves its
render quality. Fine CAD tessellation, 4x supersampling and alpha-aware downsampling
produce a 1536 x 1100 grayscale PNG (72,314 bytes). Layout remains in the same
768 x 550 coordinate space. The live matrix follows the mirrored aperture with
a positive-determinant transform, preserving the original animation orientation.
The original CAD remains private and unchanged. MCU animation, GPIO18, agent
handling and the 30 FPS target are unchanged.

Validation: 344 Flutter tests passed (three optional tests skipped), analysis and
the PlatformIO build passed, plus 24 script tests, 34 bundle tests and a raster
check detecting 8,500 partially transparent antialiased edge pixels. Desktop and
390px mobile views were checked; no overflow or aperture mismatch was observed.

Wi-Fi OTA completed on 2026-10-15. The S3 reported version `1792043877479`, web
digest `36b1a66520f57ba5016a208aee98c7976e434a453ff6e37fd9638f8dda06ff92`, and
`Up to date` after boot. Its served PNG matched the prepared file byte-for-byte;
the live page loaded the 1536 x 1100 image and correctly aligned colored canvas.
Both active agents reconnected, brightness 44/255 and the schedule were preserved.
No LED error, Wi-Fi disconnect or additional reset was reported in this short
verification. This is not a long-duration hardware stability test.

## Angled Preview Release

Version `1791988635014` uses a matte-gray angled render of the actual device.
The live colored LED canvas is mapped into the CAD aperture with a measured
affine transform. Physical pixel spacing, centered letterboxing and color encoding
are unchanged. The transparent PNG is 768 x 550 pixels and 16,982 bytes; no 3D
engine, external asset service or original CAD file is included. The original CAD
file was opened read-only and its SHA-256 remained unchanged.

Validation: PlatformIO and analysis passed, 344 Flutter tests passed (three
optional tests skipped), 24 script tests and 34 bundle tests passed. The new
projection regression checks all four aperture corners at 320, 390, 768 and
956px widths. Desktop/mobile live preview was visually checked before publication.
Target output remains 30 FPS with the same renderer and GPIO18 RMT DMA driver.

Wi-Fi OTA completed on 2026-10-14: the device reported `1791988635014`, the
expected web digest `0a3d315109d813618751af36e0fbe43d0faef7dfdf9f133858305d80c4a11bd1`,
and `Up to date` after boot. Its PNG matched the published asset byte-for-byte.
The actual live page showed the aligned gray case and colored matrix. Two real
agents reconnected; brightness and schedule were preserved. No LED error or Wi-Fi
disconnect was reported in this short post-update check. This is not a soak test.

## Filesystem Portal Release

Version `1791985824771` serves the controller portal from LittleFS: actual HTML
templates, CSS, JavaScript and the rendered device PNG. The editable tree is
`esp32-s3/source/web/`. Its verified seed is included in the normal application
OTA image; boot installs it before starting matrix output, then remounts storage
read-only. The two app slots retain separate web directories for rollback.
Existing partition offsets and NVS settings are unchanged. Unknown nonblank
storage is never automatically formatted. No separate filesystem upload is needed.

Validation: PlatformIO build and static analysis passed; 344 Flutter tests passed
(three optional tests skipped), plus 23 browser-script and 34 web-bundle tests.
Native HTML tests include missing and malformed templates. Bundle tests cover
case-sensitive path order on Windows, SHA-256, traversal and size limits.
Power-loss and filesystem-corruption recovery have not been hardware fault-tested.

Installed successfully over Wi-Fi on 2026-10-14. The S3 reported version
`1791985824771`, `web_filesystem=littlefs`, and web digest
`d10c1fdeba993268392ed32dced7a84345604ffeeaa3a656ff66b26658a7d706`.
The post-boot check reported `Up to date`. All three pages, four CSS/JS resources
and the PNG returned HTTP 200; live preview returned changing LFP1 frames.
Brightness 30/255, automatic power, and the disabled weekday 08:30-17:30 schedule
survived. Real agents reconnected. Desktop and 390px mobile layouts were checked.
A short 12-request profile had no failures, 65-231ms status responses, at least
160,496 bytes free internal heap, and 29.18 measured FPS against the 30 FPS target.
No LED errors, Wi-Fi disconnects or extra restarts were observed. This is not
a long-duration or eight-agent hardware acceptance test.

## Previous Device Preview Release

Version `1791984176328` places the live screen inside an actual orthographic
render of `CodexLimiterNewFinal`, embedded as a 24,976-byte PNG in flash. Only
the rendered picture is included, not the original SolidWorks CAD file. No live
3D library or external asset server is needed. The square pixels and physical
vertical spacing retain their aspect ratio inside the case window.

Preview colors now convert linear LED PWM into browser sRGB after optional
global-brightness compensation. Previously those linear values were displayed
directly, making sky and midtones too dark. Paused frames also retain their
colors rather than being artificially dimmed. Physical LED colors, brightness,
frame rate, scene data and the binary snapshot protocol are unchanged.
Validation: 342 tests passed (three optional tests skipped), 14 preview-script
tests passed, existing portal-script tests and analyzer passed. Desktop and mobile
layout were checked with real recorded matrix bytes. PlatformIO build succeeded;
static RAM remains 66,256 bytes. Wi-Fi OTA completed and post-boot status reported
version `1791984176328` / Up to date with both agents reconnected. The PNG served
by the S3 has the same SHA-256 as the committed image; the live browser showed
the animation inside its window with the corrected encoding. Six short status
samples all succeeded, measuring 29.32 FPS at the unchanged 30 FPS target and
minimum sampled free heap of 158,936 bytes. No sampled LED errors, Wi-Fi
disconnects or resets were observed. This is not a long-duration soak test.

## Previous Preview Release

Version `1791981464774` adds an opt-in live matrix canvas on Overview. It reads
actual successful LED frames through the bounded `/frame` endpoint, including
meteors and transitions. Logical mapping, 5 mm square pixels and the 7 mm clear
vertical gap are preserved. Optional canvas brightness compensation does not
change the physical matrix. Preview requests target 10 FPS without overlap and
pause when closed, off-screen or hidden; maintenance/errors use bounded retries.
The renderer never waits on networking and keeps its 30 FPS target. Snapshot
storage is static; total static RAM is 66,256 bytes. Validation: 341 tests passed
(three optional tests skipped), 11 preview-script tests and eight portal-script
tests passed, analyzer clean, PlatformIO build successful.
Cold frame requests yield for at most 60 ms without holding a mutex, so retry
backoff cannot continually outlast the short capture lease. The preceding preview
build was checked on desktop and mobile, with actual changing meteor frames,
correct square-pixel spacing and no horizontal overflow. Forty consecutive binary
frame requests returned complete, advancing 3,024-byte snapshots.

The final version completed Wi-Fi OTA and its post-boot check reported Up to
date. A roughly 32-second streaming check received 109 frames with 107 distinct
images and one request timeout; subsequent requests recovered. The physical
matrix measured 29.23 FPS, with no LED errors, Wi-Fi disconnects or resets in
that check; free heap was 173,328 bytes afterward. Three cold requests separated
by three-second pauses all returned HTTP 200 / 3,024 bytes in 156-219 ms.
Preview cadence is a 10 FPS ceiling, not a guarantee over a variable LAN link.
This short check does not replace a long-duration powered-matrix soak test.

## Previous Portal Release

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
