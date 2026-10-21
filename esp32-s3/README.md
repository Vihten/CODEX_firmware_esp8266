# LED Forge / ESP32-S3 N16R8

This is a separate ESP-IDF firmware target for ESP32-S3-WROOM-1-N16R8:
16 MiB quad flash and 8 MiB octal PSRAM. The firmware repository now publishes
S3 as its primary target; ESP8266 releases remain only in Git history.

## Quota Recovery Motion

A fresh report from an already live source changing its remaining quota from
below 100 percent to 100 percent triggers recovery once the combined minimum
also reaches 100 percent. First reports, expired sources, unknown quota and
source disappearance do not count as resets. The visible sky returns to noon
with a small damped overshoot over 2.6 seconds, using the existing fractional
frame sampling; the pyramid and brightness remain unchanged. An existing
meteor flight finishes first. Startup, off, memorial and maintenance suppress
new recovery events; an in-progress recovery pauses during maintenance.

## OTA Matrix Progress

During firmware installation the renderer replaces the scene with a clockwise
perimeter indicator: cyan fill tracks successfully written bytes (capped at
99 percent), a short brighter ribbon moves around the outline, and the full
border turns green only after SHA-256, SDK image and boot-partition checks.
The existing brightness and manual/scheduled off state remain authoritative.

The renderer may animate continuously during network reads. Before every
subsequent flash operation the writer revokes that permission and waits for
an exact one-shot sequence and maintenance epoch acknowledged only after
synchronous RMT completion. Flash erase/validation may briefly hold the last
frame. The parked renderer never reads artwork or PSRAM for these frames.
Missing ACKs keep maintenance sealed and defer cleanup until the renderer
proves completion; elapsed time alone is not DMA-safety evidence.
The new indicator is available for updates performed by this firmware, not
while an older installed version is still downloading it.

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

### Autonomous Air-Alert Indication

The controller polls `https://neptun.in.ua/api/v1/alerts` directly over
certificate-verified HTTPS for Vinnytskyi district (`вінницький`), including
an applicable whole-oblast alert (`вінницька`). No personal API token,
LAN agent, running PC, phone or user-operated relay is required. Successful
polls are spaced 15 seconds apart; failures back off and HTTP 429 respects
Retry-After. Networking runs on core 0, separate from the 30 FPS renderer.
ESP-IDF cross-signed certificate bundle verification is enabled for NEPTUN's
Google Trust Services certificate chain. The full trusted CA bundle and
hostname validation remain required; certificate verification is not bypassed.

The bounded 64 KiB response buffer is allocated explicitly in PSRAM, not
internal RAM needed by mbedTLS. TLS resources are released before JSON parsing.
No internal-RAM fallback is attempted if PSRAM allocation fails. The status
object exposes `failure`, `http_status` and `poll_ms` for transport diagnosis.

The Settings page enables the feature and offers bounded manual previews.
Explicit drone/missile reasons select the existing yellow drone/red rocket;
a verified active alert with an unspecified cause has a separate generic
indicator. A color level alone is not treated as proof of a weapon type.
Only a complete, validated alert snapshot may clear a warning. Wi-Fi, DNS,
TLS, HTTP, payload and rate-limit failures retain the last active warning;
the portal reports missing/stale data separately. Empty threat-track lists
are never used to infer an all-clear. Schedule and manual matrix-off remain
authoritative, and disabling the feature is an explicit user override.

The old provider's saved enabled preference is retained. Its retired API-token
bytes are cleared in RAM and rewritten empty in the existing NVS record;
Wi-Fi credentials, agent metadata, colors and weekly statistics are unchanged.
NEPTUN attribution remains visible beside the alert controls. It is a third-party
information aggregator, without availability or accuracy guarantees, and this
display does not replace official air-alert notifications or sirens.

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

## Local Agent Updates

Remote updates are separate from controller OTA. The local publisher
builds a signed release using `tool/build_agent_release.ps1`. Export firmware
with `CLOCK_AGENT_RELEASE_DIR` pointing to that release directory to embed its
manifest and ZIP in the verified LittleFS web tree. No agent package means the
update controls remain disabled. The controller never downloads executable
code from a user-supplied URL and never runs Windows commands.

Each controller keeps an internal 128-bit channel seed in NVS, generated before
rendering or Wi-Fi starts using an explicitly enabled hardware entropy source.
It is no longer an operator password and is not printed or requested in the UI.
Existing NVS seeds are preserved for compatibility with already enrolled agents.
An NVS read/commit failure disables updates instead of guessing a seed.

First install or update the bootstrap agent archive once on each PC. The agent
automatically enrolls its update channel using its normally discovered, pinned
controller identity and matching account. No setup helper or operator key is
needed. Its per-agent credential is cached privately. The agent needs no inbound
listening port. Request an update with the visible button under that agent's
portal section. Only known online agents with a recent authenticated poll are
eligible. Requests expire after ten minutes and are not persisted across a
controller restart. Queued is not installed: the portal waits for a committed
new version to be reported.

Anyone who can reach the portal on the LAN can request an available signed
update; there is intentionally no operator authentication. Automatic enrollment
also has no operator authentication. HTTP does not encrypt the channel secret.
Operate only on a trusted, isolated LAN; never expose these routes to the Internet.
Polls and responses use HMAC-SHA256 with a fresh controller boot/window challenge,
fresh agent random suffix and consumed monotonic counter. This is not a defense
against a hostile LAN client obtaining an enrollment secret. Captured polls cannot renew capability after
the challenge window or after reboot. Signed RSA-SHA256 packages, pinned publisher
keys, strict file allowlists and version checks are additionally enforced by the
Windows updater. The publisher private key stays on the publishing PC, outside
the workspace, firmware and archives. Never lose it without planning a manual
trust-key replacement on every enrolled agent.

The controller only serves `/agent-update/manifest.json` and
`/agent-update/package.zip`; management endpoints are explicit POST routes.
Capability polling does not change meteor activity, weekly counters or agent
metadata. Update job state is owned by the network task, not the HTTP task.
The Windows updater owns staging, code-only replacement, health confirmation
and rollback; user metadata, endpoint, configuration and weekly ledger are not
package targets.

## Agent Management

The Agents page can rename an agent on its PC or uninstall its executable code
and Startup shortcut. Uninstall always requires explicit confirmation in the UI;
metadata, configuration, endpoint, enrollment and weekly statistics remain for
reinstallation. It does not erase the controller's historical weekly ranking.
Both commands require an online agent advertising fresh management support.
Older agents can still update normally before advertising that capability.

Management uses the existing bounded polling connection, not an inbound PC port
or arbitrary shell command. A poll opts in with `manage=1`, appending exactly
`MANAGE\n1\n` after the existing optional FAILED block in its request MAC text.
An acknowledgment adds `ack=<id>&result=ok|failed` and appends exactly
`ACK\n<id>\n<result>\n`. Unknown acknowledgments after a controller restart are
authenticated no-ops, allowing the agent to clear its old acknowledgment record.

Only pending actions add `action_id`, `action` (`rename` or `uninstall`) and
`name_hex` to the response. The response MAC appends exactly
`ACTION\n<id>\n<action>\n<lowercase UTF-8 name hex>\n`; uninstall has an empty
name. Names are bounded to 48 UTF-8 bytes. Action IDs are boot-randomized safe
integers, not a global monotonic sequence across controller restarts. Queues
expire after ten minutes and never overlap firmware-package updates for that
agent. The PC stores an idempotent command record and also expires pending work.
The UI distinguishes queued, confirmed, failed and expired commands.

After stopping its telemetry worker, an uninstall may acknowledge within its
bounded retry budget even though the agent is no longer considered online.
The controller accepts only the authenticated poll; enrollment and new command
requests still require an online agent. Reinstalling explicitly resets the local
uninstall command record without deleting user data.

There is intentionally no operator password. Anyone on the trusted LAN who can
reach this portal can request rename/uninstall, just as they can request an
available signed update. Do not expose the controller HTTP server to the Internet.

## Weekly Agent Tokens

The Agents page ranks up to 12 agent IDs by reported input plus output tokens
for the current Monday-Sunday week in Kyiv time. Cached input is a subset of
input, not added a second time. These are local Codex session counters, not an
account billing report. Agents without the updated reporter do not contribute.

Updated agents send an additional bounded datagram over the existing transport:
`LFT1,ACCOUNT_HEX,CLIENT_HEX,YYYYMMDD,TOKENS,RGB6,UTF8_NAME_HEX`.
The date is the Monday of the week. The controller requires its NTP-synchronized
current week and matching account fingerprint. It acknowledges with
`LFT1_ACK,CONTROLLER_HEX,CLIENT_HEX`. This report never refreshes meteor activity.
Repeated or lower cumulative totals cannot inflate or reduce an agent's total.
Old-week and future-week reports are rejected. Full tables never evict another
agent's weekly contribution. The LAN protocol is unauthenticated; use a trusted
network and do not expose it to the Internet.

A separate checksummed NVS blob (`weekly_tokens`) preserves the leaderboard
without altering the Wi-Fi/settings schema. Checkpoints occur every five minutes
and before a requested restart or OTA, behind the existing renderer/preview
maintenance gate. Abrupt power loss can lose the most recent checkpoint interval;
the next cumulative report from each updated agent restores its current total.
Without those agents reconnecting, only the last committed totals are available.
Rollover waits for valid controller time. A backward clock correction does not
roll the stored week backward. The portal distinguishes pending data from a
failed persistence attempt; failed commits are retried, not marked saved.

## Hardware Acceptance

After initial USB installation, switch safely to the normal external supply.
Check the blue startup marker, IP display, scene entrance, correct colors and
mapping, agent discovery and all eight-agent load levels. Run at least a one-hour
30 FPS test while refreshing the web portal and sending telemetry. Check uptime,
free internal heap, largest block, frame deadline misses and LED errors. Test
schedule transitions and a subsequent S3 OTA with automatic rollback available.
Do not claim hardware stability until these checks pass on the actual S3 board.
