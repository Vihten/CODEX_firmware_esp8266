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

Version `1792491351127` replaces solid corner fill with compact mirrored
pixel-art glyphs: a yellow delta-wing drone for yellow level, a red rocket
for red level and a grey question mark for unknown data. These are symbolic
level labels, not independent confirmation of a weapon type; the web settings
state this explicitly. Provider polling and real-alert/demo priority are unchanged.

Glyphs and their dark backdrop remain strictly inside the existing 44 corner
pixels. The sky and all foreground artwork remain untouched. The last glyph
is retained during the 600 ms clear fade so its body cannot disappear before
the fade finishes. The original corner artwork returns after clear/disable.
Brightness, off/schedule gating and physical 30 FPS output are unchanged.
Validation: PlatformIO build, 384 firmware tests (two existing skips),
56 browser-script checks, targeted Dart analysis, allocation/mask/fade
regressions and native frame inspection passed. Physical appearance and
installation remain separate on-device checks.

## Previous Corner Indicator Release

Version `1792487114081` restores the original day/night scene and moves
air-alert indication entirely into both upper rounded corner regions.
Each corner follows the fixed hemisphere boundary (row widths 7, 5, 4, 3,
2, 1), for 44 indicator pixels total. Yellow/red identify the respective
alert level; grey identifies unknown data, never an all-clear.

The original sky, sun, moon, clouds, stars, pyramid, meteor rendering and
night-blue safeguard are restored without grading or edge recoloring.
Corners transition over 600 ms and return to their original artwork after
all-clear or disabling the feature. Brightness, schedule, manual-off and
maintenance gating remain authoritative. Physical output stays at 30 FPS.

Native full-pipeline comparisons at 20% and 100% brightness confirm that
every output byte outside these corners is identical with and without an
alert across sampled day/night frames. The overlay edits already scaled
GRB only in those corners, with no extra allocation or scene rescaling.
Validation: PlatformIO build, 383 firmware tests (two existing skips),
56 browser-script checks, targeted Dart analysis and native output inspection
passed. On-device installation and physical appearance are separate checks.

## Previous Sky Tint Release

Version `1792486005042` softens the air-alert sky treatment. A 35% color
grade replaces the former opaque tint and removes the absolute brightness
lift. Original sky gradients and darkness remain visible; black stays black.
Warm/bright unmasked artwork is protected in addition to the existing masks.

Each source frame is graded before temporal interpolation, avoiding a color
pop when cloud/object coverage changes. Pale cloud and moon edges adapt only
their estimated background contribution; white cores and foreground detail
remain intact. Background estimation uses a bounded, allocation-free search.
The normal brightness controls, 600 ms interruptible transition, memorial
exclusion and 30 FPS physical output are unchanged.
The legacy blue safeguard is bypassed while alert tinting is active so it
cannot recolor the graded night. A steady 2x2 corner indicator identifies the
alert level when very low brightness quantizes away the subtle sky tint.
Validation: PlatformIO build, 381 firmware tests (two existing skips), Dart
analysis, actual artwork/mask regressions and native render-pipeline snapshots
at 20% and 100% brightness passed. Physical appearance remains a hardware check.

## Previous Air Alert Release

Version `1792484044073` adds optional air-alert sky tinting for Vinnytsia
and its territorial community. Configure it under Settings > Air alert;
obtain a personal API token at https://alerts.in.ua/api-request and enter it
directly in the device's web page, on a trusted LAN only: the local web
interface uses plain HTTP, not HTTPS. The token is stored in a separate local
NVS record, never returned in web status or embedded in this repository.

The controller polls the authenticated alerts.in.ua v2 IoT endpoint every
30 seconds over certificate-verified HTTPS. It checks city UID 155, raion
UID 36 and oblast UID 4. Explicit whole-scope red/yellow warnings cover the
city; partial parent scope is not interpreted as a city warning. Colors
represent the provider's alert levels, not independently detected weapons.
Provider documentation: https://devs.alerts.in.ua/.

Only the moving sky changes color, with a 600 ms transition. Celestial
objects, stars, clouds, pyramid and meteors keep their colors. Physical
output remains at 30 FPS. Schedule and manual matrix-off remain authoritative.
Manual yellow/red previews last 60 seconds and cannot lower a real red alert.
Missing/stale data never fabricates an all-clear: a previous active warning
is retained, otherwise an unknown marker is shown. Freshness expires after
two minutes. During memorial/IP screens a small corner indicator is used.
This is supplementary indication, not a replacement for official warnings.

The bounded network worker runs separately from rendering and coordinates
with flash maintenance. Expiry is latched so old demos and stale clears
cannot revive after the 32-bit uptime wraps. Validation: PlatformIO build, 379 firmware tests
(two existing skips), 56 browser-script checks, targeted Dart analysis and
desktop/mobile UI inspection passed. Live authenticated provider responses
require the owner's token and were not tested as part of this release.

## Previous Night Palette Release

Version `1792419738139` restores the night background palette from before
the last darkening, while retaining the sixteen added stars, faster spin,
two smooth opening oscillations and one closing excursion.

The dark-blue safeguard no longer divides by configured brightness. Eligible
moving background pixels use a fixed raw blue floor of 5, then the normal LED
brightness scaling applies. Thus brightness 51/102/153/204/255 produces blue
PWM levels 1/2/3/4/5 rather than holding the night at a constant visible level.
At settings below 51/255, these very dark pixels can quantize to zero; brightness
zero remains fully off. Black masks, fixed artwork and bright objects are exempt.

Raw artwork caches remain unmodified, and memorial reference matching uses the
same fixed-floor transform. Physical output stays at 30 FPS, with no new heap
allocation or changes to saved settings. Validation covers the PlatformIO
build, Dart analysis, palette preservation and native brightness-response,
idempotency, shutdown-mask and memorial-reference tests.

## Previous Faster Starry Night Release

Version `1792418320059` speeds up the opening spin from 3000 to 2400 ms and
the closing spin from 3400 to 2800 ms. Pyramid movement and the smooth damped
settles retain their timing: two opening oscillations, one closing excursion.
The complete transition now lasts 4000 ms rather than 4600 ms. Renderer,
maintenance, memorial transitions and web status share the updated duration.

The night background uses a darker deep-blue palette. Sixteen new isolated
star positions join the existing artwork, with varied brightness and the
existing independent twinkle effect. The exported full-night frame has 25
star pixels. Exact background matching preserves the moon, sun, clouds,
existing stars, pyramid and full daylight palette.

The minimum visible blue PWM floor remains: at low configured brightness,
the darkest night pixels may already be at the lowest nonzero LED output.
No temporal dithering or new per-frame allocations are introduced.

Validation: ESP32-S3 PlatformIO build, 295 regression tests, targeted Dart
analysis and rendered artwork inspection passed; two existing tests remain
skipped. Physical output stays at 30 FPS and saved settings are unchanged.

## Previous Directional Settle Release

Version `1792417273550` separates opening and closing motion. Opening settles
through two damped oscillations with progressively smaller amplitudes; closing
uses just one excursion. Quintic settle segments match velocity and acceleration
at their joins, removing the hard reversals of the previous short swing.

Both paths retain the 4600 ms total duration, lower-left sky origin and final
quota-dependent position. Their precomputed phase tables are interpolated at
runtime without heap allocations or additional RAM. Settled endpoint switches
select the correct trajectory immediately; interrupted transitions reuse the
retargetable 160 ms direction blend instead of jumping between phase tables.

Validation includes the ESP32-S3 PlatformIO build, Dart analysis, regression
tests and native renderer captures. Tests cover decaying extrema, C2 settle
joins, both directional tables, endpoint switching and memorial interruption.
Physical output remains 30 FPS; colors, brightness and saved settings are intact.

## Previous Night Visibility Release

Version `1792414893985` prevents dark-blue night pixels from disappearing at
low configured brightness. At brightness 51/255, the former blue values 2-4
were truncated to zero by LED PWM scaling. Eligible moving night backgrounds
now retain one visible blue PWM count; black/off pixels, fixed artwork and
bright objects are excluded. Raw artwork and the brightness setting stay intact.
Memorial brightness matching uses the same current-brightness reference.

The single settling swing is shortened from 800 to 400 ms, doubling its peak
return speed while preserving continuous velocity at the joins. Total startup
duration remains 4600 ms; physical output remains 30 FPS. No heap is allocated.

Validation: ESP32-S3 PlatformIO build, 288 regression tests, targeted Dart
analysis and captured native renderer frames passed. Tests include actual LED
PWM output at 20% brightness, closing sky visibility, raw-reference preservation
and the sharper reversible curve. Two existing tests remain skipped.

## Previous Full-Sky Release

Version `1792413003306` preserves the entire authored sky during rotation.
The old lunar-entry black mask is removed: daylight now remains behind the
outgoing sun while night enters, rather than leaving an isolated sun on black.
The pyramid-origin mask is retained only for the initial sky reveal.

Shutdown no longer shrinks or fades the night using a separate radial mask.
A rotating bottom-edge clip lets the final night row leave the viewport before
blanking, while still suppressing the redundant closing sun. Resolution-scaled
edge antialiasing keeps boundary joins continuous on other matrix sizes too.
Configured colors, brightness, 30 FPS, quota mapping and saved settings remain.

Validation: ESP32-S3 PlatformIO build, 287 regression tests, targeted Dart
analysis and native captured frames passed. Tests verify background RGB, not
only celestial bodies, plus last-row exit and multi-size continuity. Two
existing tests remain skipped. Per-pixel work is reduced; no heap is allocated.

## Previous Celestial Departure Release

Version `1792406227615` removes the global sky fade-in/fade-out. After the
pyramid rises, the moving artwork uses its normal contrast; the geometric
origin mask still controls where it appears, rather than dimming the whole sky.

Outgoing sun and moon footprints are protected from the background wipe until
they leave the viewport. The opening sun is composed before direction blending,
so interrupted transitions stay continuous without restoring the redundant
closing sun. Configured brightness, colors, 30 FPS and saved settings are intact.

Validation: ESP32-S3 PlatformIO build, 285 regression tests, targeted Dart
analysis and native frame captures passed. Coverage includes actual-asset
antialiased horizon pixels, no global fade, direction interruptions and C1 mask
boundaries. Two existing tests remain skipped; no additional heap is allocated.

## Previous Pyramid-Origin Release

Version `1792404806928` anchors the first sky pixels beside the pyramid in
the lower-left sector, rather than revealing the whole left edge. A bounded
radial/sector mask follows the physical LED spacing and opens into the existing
day/night orbit without changing the settled animation.

Closing now contracts the lunar sky toward the same pyramid origin and hides
the redundant initial-day pass. A retargetable 160 ms mask blend prevents a
hard flash when an entrance, shutdown or memorial transition is interrupted.
Physical 30 FPS, colors, brightness and persistent settings remain unchanged.

Validation: ESP32-S3 PlatformIO build, 280 regression tests, targeted Dart
analysis and native captured frames passed; two existing tests remain skipped.
Mask geometry is cached per frame, uses no heap, and adds 16 bytes of static RAM.

## Previous Ordered Sky Release

Version `1792402513472` separates the startup sky into ordered day and night
passes. Unfilled areas are black: daylight is hidden during the lunar reveal
and returns only after the full-night position. The continuous trajectory has
no added night-zenith pause. Fixed space corners and the pyramid
remain intact; masking fades away before the unchanged quota-dependent frame.

The spin now flows into one short settling excursion with continuous velocity,
instead of a fast rotation followed by a slow swing. Reverse playback retraces
the same lower-left path, including interrupted memorial transitions.
Palette, brightness, persistent settings and physical 30 FPS remain unchanged.

Validation: ESP32-S3 PlatformIO build, 276 regression tests, targeted Dart
analysis and captured renderer frames passed. Two existing skipped tests remain
excluded, including the optional legacy ESP8266 build. Rendering uses bounded
per-pixel arithmetic and no new allocations.

## Previous Lower-Left Transition Release

Version `1792400561623` removes the memorial's blue background: only the
Ukraine outline, clock, candle and flame remain lit. The main sky transition
now uses a fixed lower-left entry/exit anchor independent of quota, with day
rising first and one settling excursion instead of repeated oscillations.
Reverse playback retraces the same path to the lower-left anchor. The final
sky position still represents the available quota, including night at low quota.
The pyramid-first reveal, subpixel sampling, shared brightness, 30 FPS,
maintenance handshakes and persistent settings remain intact.

Validation: ESP32-S3 PlatformIO build, 184 ESP32-S3 tests, seven curve tests,
exporter tests and targeted Dart analysis passed. Captured renderer frames
confirm lower-left appearance and reverse motion. The updated LUT and
standalone renderer use bounded signed arithmetic and positive cycle wrapping.

## Previous Brightness Matching Release

Version `1792398844425` matches the memorial panel's average weighted RGB
brightness to the current base animation. A uniform palette gain is captured
once from the settled sky, or a validated source-frame fallback, excluding
meteors and global brightness. Entry, exit and replay retain that gain so
twinkling and telemetry cannot cause brightness pumping. The existing global
brightness setting remains live and is applied exactly once by the LED driver.
A bounded low-brightness visibility floor keeps the text from quantizing away.
Normal animation colors, timing, 30 FPS and persistent settings are unchanged.

Validation: ESP32-S3 PlatformIO build, all 184 ESP32-S3 tests and targeted
analysis passed. Coverage includes day/night references, live
dimming, minimum brightness, invalid assets and frozen gain through replay.

## Previous Reverse Transition Release

Version `1792397625522` adds the reverse memorial transition on natural expiry
and early stop: the entire memorial panel slides upward, then the saved sky
reopens along the original entrance curve. Interruptions reverse from the last
displayed position; play during the return changes direction without recapturing
the partially opened sky. The old end fade no longer hides the moving panel.
Power-off takes precedence, persistence pauses the return clock, and automatic
OTA waits until the final restored frame has been transmitted successfully.
The portal reports the return instead of mistaking it for an IP-address screen.
Normal colors, brightness, persistent settings and 30 FPS remain unchanged.

Validation: ESP32-S3 PlatformIO build, 176 ESP32-S3 tests, 49 portal script tests
and targeted Dart analysis passed. Reverse pixels, cancellation, replay, power,
persistence, timer rollover, final-show retry and OTA handshakes are covered.

## Previous Entrance Transition Release

Version `1792396389304` adds the memorial entrance transition. The current sky
closes using the existing shutdown animation, then the entire Ukraine outline,
clock and candle slide down from above with subpixel vertical sampling and
gentle deceleration. The transition remains inside the existing 60-second window.
Cancellation, matrix power-off and maintenance preserve or restore scene state.
Normal animation colors, brightness, stored settings and 30 FPS are unchanged.

Validation: ESP32-S3 PlatformIO build, all 167 ESP32-S3 tests and targeted Dart
analysis passed. Coverage includes fractional quota, interrupted transitions,
short remaining duration, repeated requests and maintenance handshakes.

## Previous Ukraine Outline Release

Version `1792394198807` frames the minute-of-silence clock and candle inside
a blue-and-gold outline of Ukraine, including Crimea. The 50 x 20 composition
accounts for the physical LED spacing: the clock and border remain steady,
while only the small flame moves. Scheduling, persistence, manual controls,
normal-scene colors, brightness and 30 FPS output are unchanged.

Validation: ESP32-S3 PlatformIO build, native exact-pixel/containment and
full-minute continuity tests, scheduler/network tests and targeted analysis.

## Previous Candle Release

Version `1792392780201` adds an optional daily minute of silence at 09:00
Kyiv time. The matrix displays a steady warm `9:00` above a shaded candle
with a gently leaning flame, then returns to its previous scene after 60 seconds.
Meteors pause during the commemoration. Settings include enable/save, manual
preview and stop; the preference survives reboot in a separate NVS record.
The feature is initially disabled. Automatic playback requires synchronized time
and respects matrix power and schedule. Manual preview does not require NTP.

Validation: ESP32-S3 PlatformIO build, native scheduler/network/renderer tests,
portal script tests and web-bundle tests passed. The normal animation remains
30 FPS with unchanged colors, brightness, Wi-Fi, agent and ranking settings.

## Previous Online-First Release

Version `1792157021571` lists connected agents before disconnected agents in
both the management list and overview summary, including initial server-side
rendering. Status polls update ordering without reload. Stable partitioning keeps
relative order inside each group and moves existing DOM nodes only when needed;
unsaved forms, focus/selection, open controls and filters are preserved. Weekly
token ranking remains sorted by tokens, independently of connection state.

Validation: ESP32-S3 PlatformIO build, 32 native network tests, 42 portal script
tests and targeted Dart analysis passed. Browser verification confirmed online
agents first, with no console errors or overflow. Matrix output remains 30 FPS;
animation colors, brightness and persistent settings are unchanged.

## Previous Automatic Roster Release

Version `1792147117597` automatically inserts newly reported agents in the
overview summary and agent management list during the existing status poll.
The new-agent notice and manual list reload are removed. Inert server-rendered
templates reuse existing controls; client-side insertion uses DOM APIs, validates
IDs, preserves unsaved edits and focus, and retains bounded offline history.
Native forms, filters, capability gates and uninstall confirmation are preserved.
Status reports each agent's saved-color mode for newly inserted controls.

Validation: ESP32-S3 PlatformIO build, 31 native network tests, 33 portal script
tests and 34 web-bundle tests passed. Local browser checks confirmed new rows in
both destinations without page reload or console errors. Physical animation,
30 FPS, colors and persisted settings are unchanged. No password is included.

## Previous Overview Release

Version `1792146750418` removes the duplicate overview controls when interactive
device settings are available. Brightness, power and schedule use the same native
forms inside the device-image dialogs; unsupported browsers retain the fallback
controls. Agent activity remains visible below the image.

The shared header no longer repeats the matrix dimensions. Page headings have a
24 px gap below navigation, and the last-update timestamp now sits beside the
connection status instead of beside the page title. Matrix colors, brightness,
30 FPS target and persisted settings are unchanged. No password feature is added.

Validation: ESP32-S3 PlatformIO build, 31 network contract tests and 24 portal
JavaScript tests passed. The release retains the previous filesystem asset
validators, favicon and bounded initial preloader.

## Previous Portal Release

Version `1792141127529` updates the Codex web portal: explicit filesystem asset
validators and conditional responses, quieter background refresh, compact agent
management controls, corrected live counters and form state, and grouped weekly
token totals. A matrix/meteor favicon and a bounded, fail-open initial loader
prevent progressive page rendering without delaying live preview startup.
The loader watchdog is embedded in the first HTML chunk; the remaining assets
continue to be maintained in the filesystem web tree.

No web password feature is included. Physical matrix output remains 30 FPS on
GPIO18, with unchanged animation colors and brightness behavior. Existing NVS
settings, Wi-Fi configuration, agent colors and weekly statistics are preserved
by the normal OTA application update. The signed agent package remains version
`20261016094859`; no account credentials or local statistics are published.

Before publication: ESP32-S3 PlatformIO build and 32 network/asset contract tests
passed. Portal JavaScript: 23 tests passed. This same firmware version was
previously installed locally through USB. Publication does not assert that a
controller has already downloaded or installed the release over Wi-Fi.

## Previous Roster Release

Version `1792135037323` fixes agent roster slot allocation: unused slots are
preferred over offline identities; only a full live table reclaims the oldest
expired entry. The web snapshot also lists missing agents from the persisted
current-week ranking as offline, without creating telemetry or meteor activity.
Historical-only agents can retain and change their saved color. This is not a
permanent agent registry: weekly-only identities expire with the weekly history.

This release includes the locally verified interactive device settings, saved
weekly token ranking, signed keyless agent updates, and remote rename/uninstall
commands. Remote uninstall retains identity, configuration and statistics on the
PC. Management actions require an online compatible agent; legacy telemetry
remains accepted, but a legacy heartbeat does not imply management support.
The bundled signed agent package is version `20261016094859`; publisher private
keys, controller credentials and local user statistics are not distributed.

Validation: 73 native network/preview/stream tests passed, along with the export,
targeted Dart analysis and ESP32-S3 PlatformIO build. Physical output remains
30 FPS on GPIO18. USB installation and the live device version/roster are checked
separately; this release makes no new long-duration stability claim.

## Previous Stream Release

Version `1792049430436` replaces per-frame browser requests with a bounded raw
LFP1 HTTP stream on `/frames`, keeping `/frame` as a compatibility fallback.
One low-priority Core 0 worker owns one async request and a single copied frame;
HTTP page handling stays independent. Streams renew after 30 seconds, skip
duplicate frames, never queue historical frames, and use a one-second per-send
socket timeout (not a total per-frame deadline). Only one streaming viewer is
admitted; other viewers fall back to polling and may re-probe after 30 seconds.

Before persistence, OTA or restart, stream admission is sealed and the worker
must drain before flash work. Clean EOF half-closes only the write side, preserving
queued data; errors shut down both directions before async completion.
HTTPD retains descriptor ownership. Browser parsing accepts arbitrary fragments,
keeps bounded storage and draws only the newest complete frame per read. Pause,
hidden/offscreen pages, malformed packets and a two-second no-frame timeout
abort the stream. Colors, brightness, physical frame/effect data and 30 FPS
matrix output remain unchanged.

Validation: 67 export/network/native preview tests, 29 browser-preview tests,
9 portal tests, 34 web-bundle tests and targeted Dart analysis passed. Includes
23 native stream lifecycle/error cases. Device timing is verified after OTA;
no sustained frame-rate claim is made solely from unit tests.

The initial stream build `1792048698713` reached 13.21 distinct FPS over 45 seconds
but had two truncated connections and a 1.16 s maximum gap. The revised build
relaxes the overly short send timeout and distinguishes graceful renewal from
error shutdown; final device results are recorded after deployment.

Wi-Fi OTA verified on 2026-10-15: version `1792049430436`, web digest
`c11766433bb89e1ec2db90ccd0bfe4a9dc07f722995bab5dba50337efd9de3f5`,
and `Up to date`. A 45.05-second stream test delivered 541 unique frames
(12.01 FPS) over two connections, including normal renewal, with zero truncated
connections or parser errors. Median arrival gap was 63 ms; seven gaps exceeded
250 ms and the largest was 1.531 s. This is an improvement in connection
continuity, not proof of stutter-free playback on the current network.

The user explicitly chose minimum latency over a proposed two-second playback
buffer. No artificial delay or interpolation was added. Desktop and 390px mobile
browser checks confirmed `transport=stream`, changing frame sequences and no
canvas filter. Brightness remains 44/255. Physical renderer/frame/effect hashes
remain unchanged; no LED error or Wi-Fi disconnect was reported during the short
post-update check. The web page and fallback `/frame` stayed available while the
stream was active; a second stream was refused with 503 as designed.

## Lower-Latency Polling Release

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

Wi-Fi OTA verified on 2026-10-15: version `1792047118711`, web digest
`f6799bf956a88ddea3c66c8f11e908b7adede215b1acee661bc93eaa15ad026f`,
and `Up to date`. Brightness remained 44/255; live canvas filter is `none`,
while only the body image has the soft drop shadow. No LED error or Wi-Fi
disconnect was reported in the short post-update verification.

Timing caveat: seven of eight fresh-connection frame reads took 22-63 ms;
one took 228 ms. Two 30-request persistent-connection samples delivered 4.7-6.0
distinct frames/s, with isolated 1.2-1.4 s stalls. A connection-close diagnostic
still stalled. The 15 FPS value is a target, not a demonstrated sustained rate;
typical response latency improved, but network jitter remains unresolved.

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
