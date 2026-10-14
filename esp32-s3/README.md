# ESP32-S3 N16R8 Firmware

- Module: ESP32-S3-WROOM-1-N16R8, 16 MiB flash, 8 MiB octal PSRAM.
- Version: `1791958778913`.
- LED data: GPIO18, 50 x 20 WS2812 GRB, target 30 FPS.
- Render task: core 1. Network/configuration owner and HTTP: core 0.
- Telemetry capacity: 12 agents. LAN telemetry is not authenticated.

`manifest.json` and `firmware.bin` are only for the S3 OTA client. Identical copies
are published in the repository root, replacing the retired ESP8266 release.
This directory remains compatible with the URL in already installed S3 firmware.
Never flash these images onto ESP8266. They do not erase NVS settings.
No Wi-Fi password, Codex login token or account password is included; the existing
account fingerprint is a non-secret routing identifier, not an authentication key.

## Build and Initial Installation

The complete PlatformIO ESP-IDF project is in `source/`, including embedded artwork
and dependency lock. From that directory:

```powershell
pio run -e esp32s3
pio run -e esp32s3 -t upload --upload-port YOUR_COM_PORT
```

Install using the complete PlatformIO upload for the first flash: the OTA app image
alone does not install the bootloader and partition table. Use a data-capable USB
cable and confirm the selected physical device. Do not erase flash to update.

Never power the 1000-LED matrix from a PC USB port. Do not connect USB and the shared
external supply together unless board power isolation is verified. Shared signal
ground and a suitable 3.3 V to 5 V buffer are recommended for the data connection.

Configure 2.4 GHz Wi-Fi through the controller setup portal. Subsequent S3 OTA
checks use this directory's manifest and accept only a newer matching-board image
with the expected size/hash/project. This release's installed version equals the
manifest, so a controller already running it has nothing newer to install.

## Agents

Download `downloads/codex-lan-agent.zip` from
[CODEX_app](https://github.com/Vihten/CODEX_app).
Extract all files and run `rebind_to_s3.cmd` on each existing agent PC, under its
original Windows user. Do not uninstall or delete metadata first. The helper
checks device ID `EB2198`, currently at `10.0.80.162`, before replacing its pin.
If DHCP changed the address, supply `-DirectHost CURRENT_IP`. Normal DHCP recovery
then follows that same device within reachable known/current private subnets.
Names, local metadata and collector/port settings are retained. Old-controller-only
color overrides are not transferred: select them on the new `/agents` page.

## Verification Limits

The image built successfully, was USB-installed, and served both portal pages with
one real agent. Application tests passed (304 passed, three optional skipped).
Long-duration PSU/matrix stability, eight simultaneous agents and an end-to-end
S3 OTA installation/rollback still require hardware acceptance testing. Do not
treat these build checks as a guarantee of wall-mounted operational stability.
