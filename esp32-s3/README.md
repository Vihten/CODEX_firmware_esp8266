# LED Forge / ESP32-S3 N16R8

This is a separate ESP-IDF firmware target for ESP32-S3-WROOM-1-N16R8:
16 MiB quad flash and 8 MiB octal PSRAM. The firmware repository now publishes
S3 as its primary target; ESP8266 releases remain only in Git history.

## Wiring

- Matrix data: GPIO18, not the NodeMCU label D4.
- Matrix: existing 50 x 20 WS2812 GRB, existing mirrored/serpentine mapping.
- Controller and matrix must share signal ground.
- Power the matrix from its rated external supply, never from a PC USB port.
- Do not connect USB power and the existing common external supply together
  unless the board's power isolation has been verified. Disconnect one first.
- An ESP32-S3 output is still 3.3 V. For the existing 5.1 V matrix and long
  data wire, use a suitable 3.3 V to 5 V logic buffer (for example 74AHCT125),
  short data wiring and appropriate signal conditioning. RMT DMA solves CPU
  timing contention, not marginal electrical signal levels.

GPIO18 avoids bootstrap pins, native USB GPIO19/20, PSRAM-related GPIO35/36/37,
and the usual UART0 GPIO43/44. Verify the particular carrier board exposes it
and has no peripheral attached before connecting.

## Runtime

### Filesystem Web Project

The portal is served from a real LittleFS directory tree, not inline C++ HTML,
CSS or JavaScript strings. Editable source lives in `web/`:

```
web/
  assets/css/portal.css
  assets/css/preview.css
  assets/js/portal.js
  assets/js/preview.js
  images/device.png
  templates/                 HTML layout and reusable server-rendered fragments
    shell/
    overview/
    agent/
    agents/
    schedule/
    settings/
    stats/
    summary/
```

The existing `storage` partition at `0xC30000` is mounted at `/www`.
The active application's files are `/www/ota_0/...` or `/www/ota_1/...`;
the other directory is retained for application rollback. Partition offsets,
Wi-Fi credentials, configuration and agent colors in NVS do not change.

CMake builds `main/web_bundle.bin` from `web/` using `build_web_bundle.py`.
This SHA-256-checked seed travels inside the application OTA image. Before
the renderer receives its first control snapshot, boot verifies the on-disk
files and installs the matching tree if needed. After installation the filesystem
is remounted read-only. HTTP streams these actual files through bounded buffers;
templates insert escaped controller data and preserve native HTML forms.

Only completely erased storage may be initially formatted. Unknown/corrupt
nonblank storage is not automatically erased; an OTA image unable to prepare
its web tree is rejected in favor of the previous application. An interrupted
initial format can require explicit recovery. Ordinary partial file installation
is retried on the next boot. The tree has no account tokens or Wi-Fi passwords.
No public filesystem editor, upload route or arbitrary path endpoint is exposed.
`/status` reports `web_filesystem` and the verified `web_version` digest.

Edit the source files, rebuild and publish the usual firmware OTA image; no
separate filesystem flashing step or USB connection is needed. Never run a
separate filesystem upload against the live controller while its renderer runs.

Moving meteor bodies, tails, sparks and dissipating wakes use fractional coordinates
and a four-LED coverage filter. Premultiplied color is accumulated in a bounded
renderer-owned 8 KiB layer and composed once against the original stored GRB bytes.
Per-destination sun/moon/cloud contrast, agent colors and brightness limits remain.
Off-screen samples are clipped, not wrapped. Existing burst counts and spacing stay.

The S3 exporter retains fractional sky phase during startup/reverse playback and
blends adjacent baked frames before output. Remaining-limit changes settle gradually
between those same frames; stars, moon and cloud brightness use fractional time.
This smooths existing animation, not a new free-drifting cloud simulation. Pyramid
geometry and the original physical row spacing remain unchanged. Output is still
30 FPS, not 60 FPS. No per-frame heap allocation or networking is added to rendering.

- Core 1: sole owner of scene state and LED output, 30 FPS, priority 4.
- Core 0: sole owner of agents/configuration, Wi-Fi, UDP telemetry, HTTP and OTA,
  priority 3. SDK HTTP task is also pinned to core 0.
- A separate priority-2 task on core 0 fetches the HTTPS manifest and returns a
  copied result through a bounded queue. Slow GitHub requests do not block agent
  heartbeats. This worker does not mutate configuration or write flash.
- RMT DMA transmits LED frames without software masking interrupts for the
  approximately 30 ms required by 1000 serial WS2812 pixels. DMA completion
  yields the scene task; the wire rate itself cannot be made faster in software.
- Animation and effect masks are copied once into PSRAM. LED output and IPC
  buffers remain in internal RAM.
- Latest-state queues replace unsafe cross-core sharing. Commands are bounded.
- NVS writes and OTA require a matching renderer maintenance acknowledgement.
  Already launched meteors finish first; OTA blanks and drains the LED output.
  A matching release acknowledgement is required before requesting a new epoch.
- Capacity is 12 agents, including the requested eight. The existing multi-agent
  meteor limits and pair spacing are retained.
- Both idle tasks remain watchdog monitored. `/health` exposes runtime counters;
  a successful build is not a substitute for a powered-matrix soak test.
- The renderer also subscribes to the task watchdog before starting RMT and
  feeds it from its own loop. A lost DMA completion cannot leave it blocked
  indefinitely without triggering watchdog recovery. Flash core dumps are
  disabled; large flash erases yield so normal parked rendering can feed.

## Controller Portal

The Ukrainian portal has Overview (`/`), Agents (`/agents`) and Settings
(`/settings`) views. Power, brightness and the single weekly schedule stay on
Overview. Agents retain native color controls and add connected/active filters.
Settings contains Wi-Fi, GitHub OTA, diagnostics and confirmed restart.

Inline assets require no CDN or external service. Native POST forms work without
JavaScript. Optional `/status` polling runs every five seconds while visible,
with a six-second deadline, one in-flight request and bounded error backoff.
Polling never overwrites unsaved form inputs. Brightness and day presets only
edit the form until its save button is pressed. FPS is measured from consecutive
frame/uptime counters, not copied from the configured target. Diagnostics can be
downloaded from `/health`; credentials are not included in `/status`.

Overview also contains an opt-in live matrix canvas. `/frame` sends a bounded
24-byte `LFP1` header followed by row-major RGB bytes from successfully transmitted
LED frames. It uses the renderer's actual physical mapping and brightness, not
a separate browser scene. The optional visibility boost only affects the canvas.
The square 5 mm pixels use 7 mm horizontal pitch and 12 mm vertical pitch.
The screen sits inside a static, front-facing PNG rendered read-only from the
user's `CodexLimiterNewFinal.SLDPRT`, not an invented case or a browser 3D viewer.
The 25 KB image is embedded in flash and served by `/device.png`; no SD card,
external image service or original CAD file is needed on the controller.

Preview colors convert linear LED PWM back to sRGB before drawing. The optional
visibility boost first reverses global strip brightness; both modes perform
display encoding. This preserves RGB ordering and does not change physical LED
output, scene grading or per-agent colors. Low-brightness quantization remains
irreversible. Pausing retains the last frame without artificially dimming it.

The renderer captures at most 10 frames/second into a static 3,024-byte buffer
while a browser lease is active, skipping immediately if its mutex is busy.
HTTP copies a complete snapshot, releases the mutex, then sends it. No network
operation blocks the renderer. The client has one in-flight request, pauses when
collapsed, hidden or off-screen, and backs off on maintenance/errors. Capture
leases expire after 1.5 seconds without requests; stale frames are not served.
Cold HTTP requests yield for up to 60 ms for a new frame without holding any
mutex. This prevents retry backoff from continually outlasting a capture lease.
The physical matrix retains its 30 FPS target. The preview is not a camera and
cannot report failed LEDs or electrical signal corruption after transmission.

## Build and Export

From the LED Forge repository root:

```powershell
C:\src\flutter\bin\flutter.bat test tool/prepare_esp32s3.dart
C:\Users\Vlad\.platformio\penv\Scripts\platformio.exe run -d build/clock-esp32s3 -e esp32s3
C:\src\flutter\bin\dart.bat run tool/package_esp32s3.dart build/clock-esp32s3 build/clock-esp32s3/release
```

The exporter uses the current clock project and keeps the ESP8266 source assets
unchanged. Each export receives a fresh timestamp version; set `CLOCK_OTA_VERSION`
explicitly only when a reproducible release version is needed. OTA requires that
version to increase. The output contains the complete PlatformIO project, embedded scene
assets and the built app image. Initial installation requires the ESP32-S3
bootloader, partition table and app; use PlatformIO upload with the actual board
port, not a standalone ESP8266 OTA binary.

The new chip cannot inherit the old chip's NVS by itself. Configure Wi-Fi on its
setup portal. Existing agents pin the old controller's identity, not just its
IP. Extract the updated agent archive and run `rebind_to_s3.cmd` on each agent
PC as the Windows user who originally installed it. This explicitly verifies
device EB2198 at 10.0.80.162, preserves local metadata and collector/port settings,
backs up the old endpoint cache, and updates autostart. Do not uninstall first.
Use `rebind_to_s3.cmd -DirectHost CURRENT_IP` if its address has already changed.
Subsequent IP changes keep the new identity and use normal bounded DHCP recovery.
Ethernet and Wi-Fi must be mutually reachable; broadcasts do not cross routers by
default. Colors stored only on the old controller must be selected again on S3.
No account
password or bearer token is embedded in the firmware. Telemetry identity is not
an account login and the LAN telemetry protocol must remain on a trusted network.

The exporter uses committed inputs in `firmware/clock-source/`, so a fresh
application checkout can export the same artwork without ignored local build
files. `CLOCK_PROJECT_PATH` and `CLOCK_RENDERER_SOURCE` can select another matching
pair. For a standalone build of the published clock, use `esp32-s3/source` in the
firmware repository instead:
it includes the rendered C++ scene, artwork, project configuration and dependency
lock. Run `pio run -e esp32s3` there. It does not require Flutter or a saved account.

## OTA Separation

The S3 manifest and uncompressed app image are published in the firmware repository
root and mirrored at `esp32-s3/` for the already installed S3 OTA URL. Keep both
copies identical when publishing. The former ESP8266 artifacts are no longer
active. Never flash the current S3 image onto ESP8266. The packager rejects foreign
chip IDs, descriptors and board manifests. Uploading to GitHub and flashing a
connected device remain separate operations, not part of export.

Each OTA slot is 6 MiB. The app contains its artwork; there is no per-frame file
I/O or separate filesystem update needed. Settings remain in the dedicated NVS
partition when updating the app without erasing flash.

## Hardware Acceptance

After initial USB installation, switch safely to the normal external supply.
Check the blue startup marker, IP display, scene entrance, correct colors and
mapping, agent discovery and all eight-agent load levels. Run at least a one-hour
30 FPS test while refreshing the web portal and sending telemetry. Check uptime,
free internal heap, largest block, frame deadline misses and LED errors. Test
schedule transitions and a subsequent S3 OTA with automatic rollback available.
Do not claim hardware stability until these checks pass on the actual S3 board.
