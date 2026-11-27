CODEX LAN ACTIVITY AGENT

DIRECT PEER CHAT

After a web update, extract the matching agent bootstrap. In an Administrator
terminal in that folder, run .\configure_peer_firewall.cmd once on each PC
that will receive peer messages. See DIRECT_PEER_INSTALL.md for manual setup
and removal. OFFICE_CHAT.md explains peer fingerprint verification.

PAIRED-ACCOUNT LIMITS (LFA3)

Activity and token history continue for every local account. Sky/quota reports
are enabled only when account/read before and after account/rateLimits/read
both match the configured paired-account fingerprint. Account lookup failures
and other accounts clear the quota to unknown; an earlier value is not reused.
Only the account fingerprint is transmitted, never the login, password or token.
The controller accepts older agents for activity, but ignores their unverified
quota values. Update the agents to enable paired-account sky reports.

CLAUDE CODE ACTIVITY

Agent 20261112160000 also reports the two activity sources separately through
LFX1. The updated controller uses compact, slower comets for Claude and the
existing meteor style for Codex, in the agent's selected color. Update both the
controller and agents to enable the distinction. Earlier controllers keep LFA3.

Local Claude Code session events contribute to this agent's activity percentage
and meteor count using its existing name and color. Activity is shared with
Codex using the larger current value. Recent assistant/tool activity expires
automatically when the session becomes idle. Updating the agent is required.

CLAUDE CODE FIVE-HOUR LIMITS (LFC1)

When Claude Code is signed in locally as marketing@tiras.ua, the agent reads
its five-hour remaining quota in a background worker every 60 seconds.
The live OAuth profile must match the local account and organization.
Credentials stay on this PC and are never included in telemetry or archives.
The agent leaves credential refresh to Claude Code. Missing, expired or
unavailable data becomes unknown after at most 90 seconds or the quota reset.
The controller alternates Codex and Claude every five minutes when both have
fresh data. If only one source is current, it displays that source.
Update an existing installation with update_codex_lan_agent.cmd to preserve
its name, configuration and token history. Claude quota does not change
Codex token accounting.

QUOTA RESET METADATA (LFR1)

For the verified marketing@tiras.ua account, the heartbeat sends the actual
quota reset epoch separately from existing balance/activity/token packets:

  LFR1,<paired fingerprint>,<client>,<codex|claude>,<quota fingerprint>,<epoch seconds>,<observation age seconds>

Codex reads the authenticated app-server account/rateLimits/read response,
including rateLimitsByLimitId and legacy rateLimits. Codex prefers a window with
an explicit duration of 300 minutes and uses an explicit 10080-minute weekly
window if no five-hour candidate exists. The duration comes from
windowDurationMins or window_duration_mins. Within the selected duration, the
highest usedPercent wins and ties retain source order. The epoch comes from the
selected window's resetsAt, resets_at, or reset_at field. If that field is
missing or invalid, the agent sends reset epoch zero to clear the previous window. The Codex remaining balance
uses the most exhausted window across all limits.

Claude uses the authenticated usage response's five_hour.resets_at, preserving
its explicit timezone when converting to Unix seconds. The existing utilization
rounding, identity verification, expiry, and credential handling are unchanged.

Known reset epochs are integer seconds in [1700000000, 4102444800].
A verified observation with an unknown reset carries zero and its actual age.
The agent omits observations with an unverified identity or an age outside [0, 86400] seconds.
The same ASCII packet is sent over existing UDP and bounded HTTP /agent paths,
at most once per provider every five seconds. Metadata never refreshes balance
TTL. The separate updater HMAC request/reply formats are unchanged. Credentials
and raw authenticated responses are never added to packets or status logs.

Controller integration: existing firmware accepts LFR1 and exposes reset_at in
provider status. LFR1 carries the epoch without the window duration. Label the
Codex timestamp as a reset time, since it may describe a five-hour or weekly
window. Claude's timestamp describes its five-hour window.

Focused offline tests (mock responses/transports; no installed/live agent):

  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_quota_reset.ps1
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_limit_app_server.ps1
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_claude_quota.ps1
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_agent_management.ps1

Release integration requires the usual strictly increasing agent release version
and regenerated package/manifest/bootstrap via tool\build_agent_release.ps1,
followed by the normal release verification and separately authorized signing/
publication process. Source edits alone do not update installed agents. No
release generation, installation, signing, or publication is part of this change.

REMOTE MANAGEMENT (WINDOWS)

Management uses the existing authenticated update poll; there is no key UI.
The poll advertises manage=1. Its HMAC appends MANAGE\n1\n after any FAILED
block, then ACK\n<id>\n<ok|failed>\n when sending ack=<id>&result=<result>.
An optional action reply contains action_id, action (rename or uninstall),
and name_hex; its HMAC appends ACTION\n<id>\n<action>\n<name_hex>\n.
Here \n means a single LF byte. MAC inputs are UTF-8. Base response bytes
are unchanged when action fields are absent. IDs are positive safe integers;
IDs from different controller boots are not numerically ordered.
Names are strict UTF-8, 1-48 bytes, lowercase hex, nonblank and without control
characters. Uninstall has empty name_hex. An action and update never coexist.

Actions are durably recorded in .agent-action.json with received/expires UTC
timestamps. Pending actions expire locally after ten minutes, including after
restart; expired actions report failed without changing the installation.
Rename changes only the Name metadata field and gracefully restarts the worker.
Unknown metadata fields, color, identity, configuration and statistics remain.

Remote uninstall first requests a graceful worker stop (15-second bound).
It removes only the exact verified per-user recovery task, legacy Startup shortcut and the
allowlisted remote code files (plus their atomic-write .new siblings), from
the exact LOCALAPPDATA\LED Forge\Codex LAN Agent install directory. Reparse
paths and shortcuts owned by another installation are rejected. There is no
arbitrary command/path input or recursive installation-directory deletion.
Settings, statistics, keys, logs, rollback/staging data and manual support
scripts remain. This is not a complete folder purge. Other agents are untouched.
The result is persisted after removal; ok is never reported before removal
succeeds. Loaded code attempts the authenticated ack for at most 60 seconds,
then the supervisor exits even if the network is unavailable. An unconfirmed
durable result does not mean the controller observed success. Failed removal
reports failed. Ack retries stop only after an authenticated successful reply.
An explicit manual reinstall clears a successful uninstall tombstone after
stopping the existing processes, without clearing settings/statistics.

Older agents with the eight-file updater require the signed migration release
before installing the expanded chat package. The controller sequences both
steps from one update request. For manual recovery, extract the trusted new
bootstrap.zip into a separate folder and run
update_codex_lan_agent.cmd as the installing Windows user. For a preview, run
.\update_codex_lan_agent.ps1 -WhatIf from PowerShell in that folder.
Once bootstrapped, the new updater accepts packages matching its fixed file set.
Manual support scripts remain bootstrap-only. Management and supervisor tests
use temporary fixtures and mocked HTTP/process/removal operations; they never
uninstall the running local agent.

UPDATE AN EXISTING AGENT WITHOUT RESETTING SETTINGS

Extract the entire new ZIP into a separate folder. Run
update_codex_lan_agent.cmd as the same Windows user who installed the agent.
Do not uninstall the old agent or delete its AppData folder first.
The update wrapper forwards saved collector/controller hosts and all ports;
it leaves the saved name, metadata, controller discovery identity and weekly
token checkpoint untouched. The installer replaces the worker/helper scripts,
restarts the supervisor, and replaces the legacy Startup shortcut with a
per-user Windows scheduled recovery task. It starts at sign-in and checks every
minute while that user is signed in, including on battery power. No password or
administrator session is stored. The supervisor mutex prevents duplicate agents.
An intentional uninstall removes recovery too; settings and statistics are not
reset by recovery. This does not run while the Windows user is signed out.
Controller-managed agent colors remain on the controller. The agent ID remains
stable provided the computer name and Windows username do not change.
If settings are absent or invalid, the update stops instead of using defaults.
For a genuinely new installation, use install_codex_lan_agent.cmd below.

Install on every Windows PC that runs Codex with the monitored account:

  1. Extract this entire ZIP archive.
  2. Run install_codex_lan_agent.cmd.
  3. Enter an agent display name (Enter uses the computer name).
  4. Set the agent color on the controller web page at its current IP /agents.

Names are trimmed and bounded to 48 UTF-8 bytes without splitting a Unicode
character. Control characters (Unicode Cc, including tabs/newlines, NUL, DEL,
and C1 controls) are rejected before trimming or truncation. Until a color is
selected on the controller web page, the agent uses a stable fallback palette:

  FF4545,FFE04A,39D965,35DDE0,4A78FF,DF55E8

Exact fallback: SHA-256 of UTF-8 COMPUTERNAME-USERNAME, take the first four bytes
in digest order as eight hex digits (the wire client ID), parse as an unsigned
32-bit hex integer, then choose palette[clientId % 6], with zero-based indexing.
Renaming the display name does not change the default. Example: client BA7816BF
selects index 3, color 35DDE0. The controller's saved web color takes precedence
over this fallback. Invalid names fail validation before installed files or
processes are changed.

For unattended installation, supply the agent name:

  install_codex_lan_agent.cmd -AgentName "Office PC"

The display name is stored in agent-metadata.json beside the installed script.
Reinstalling retains the saved name unless -AgentName is supplied. Color is
configured on the controller web page, not during installation.

The installer starts a hidden supervisor and adds it to the current user's
Startup folder. The supervisor captures worker stdout/stderr in worker-output.log
and worker-error.log, records exit codes in agent.log, and restarts an exited
worker after ten seconds. One supervisor mutex prevents duplicate supervisors;
the existing worker mutex prevents duplicate telemetry. Every PC sends its own heartbeat directly to the
controller at the configured search seed (currently 10.0.80.227). HTTP is the primary routed-network transport and UDP
4210 remains as a low-latency fallback on flat networks. There is no main relay
PC: if any computer is
off or unavailable, agents on the other computers continue working normally.

The installer checks the launched supervisor for two seconds before reporting
success. An early exit, including exit 0 due to an owned instance mutex, fails
installation startup with a diagnostic. If an old agent could not be stopped,
stop it manually and rerun the installer. Processes with inaccessible command
lines are not guessed or terminated. Startup errors are captured in
startup-error.log beside agent.log. Passing this check confirms process survival
during that interval, not worker health or controller connectivity. Check agent.log
for a matching controller acknowledgement and worker-error.log for worker failures.

Telemetry is also copied to the optional LED Forge preview at 10.0.0.97:4212.
That preview PC may be switched off and does not participate in controller
operation. LED Forge can remain closed after the controller has been flashed.

For immediate activity detection on another PC, install this agent on that PC
too. Each agent polls the shared account limit every 15 seconds for remaining
percent only. Local task events and token activity determine activity intensity;
account-wide usage changes never activate or boost this PC's activity.

Activity is normalized local intensity, not an exact token count or percentage
of account quota consumed. One, two, three, four, and five-or-more active tasks
produce 10, 30, 50, 70, and 90 percent respectively. The 10-percent minimal tier
targets approximately one meteor per 20 seconds with the controller's five-grade
mapping; rendering cadence is controlled by firmware, not by this agent.
New local output + reasoning-output token samples boost intensity for 8 seconds:
1..49 tokens -> 10%, 50..249 -> 30%, 250..999 -> 50%, 1000..1999 -> 70%,
2000+ -> 90%. The greater of task intensity and the held token boost is sent.
Zero-work or missing-usage token events do not create or extend a boost.

To use another collector address:

  install_codex_lan_agent.cmd -CollectorHost 10.0.0.97

To use another controller address:

  install_codex_lan_agent.cmd -DirectHost 10.0.80.227

The agent log reports "direct controller connected" after the controller
acknowledges this PC. If it reports "direct controller unavailable", verify
that both devices are on the same routed network. After ten seconds without an
acknowledgement, the agent searches the known controller's private IPv4
/24 and current private adapter /24s (at most 1024 unique addresses): at most
eight asynchronous HTTP GETs per iteration with a shared 1500 ms header deadline
and bounded incremental page reads (2048 characters, 750 ms). No empty TCP
probe is opened before the GET. It sends telemetry only to pages identified as LED Forge controllers,
then requires an acknowledgement for this client and the saved device ID.
The confirmed address/device are cached in controller-endpoint.json. Searching
stops while connected; an unsuccessful full pass retries after thirty seconds.
This is DHCP recovery within known/current private /24s, not discovery across arbitrary
subnets, public IPs, hostnames, or VLANs. A separate UDP 4210 listener also
accepts LF1_HELLO announcements from a disconnected controller, verifies the
saved device ID when present, and requires the usual telemetry ACK before
switching permanently. This can recover across subnets if router/firewall rules
deliver the announcement (the firmware also announces to its saved bridge PC).
Listener conflicts do not stop HTTP subnet discovery. A DHCP reservation in the router is
still recommended. Device-ID pinning prevents accidental switching, not spoofing;
the existing LAN protocol is not authenticated. To replace the controller,
use the explicit replacement procedure below rather than deleting the cache.

The agent sends client identity, activity intensity, remaining account-limit
percent, and the display name and fallback color. It never sends prompts, responses, file paths, source
code, authentication tokens, or token contents.

WIRE PROTOCOL

Direct UDP (default port 4210) and HTTP POST /agent (default port 80) send the
same ASCII body, without a trailing newline:

  LFA2,<account>,<client>,<activity>,<remaining>,<colorHex6>,<name encoded UTF8 hex>

account: existing eight-hex-digit account fingerprint.
client: existing eight uppercase hex digits derived from COMPUTERNAME-USERNAME.
activity: integer 0..100, from local session activity only.
remaining: integer 0..100, or -1 when unknown; this value is account-wide.
colorHex6: exactly six uppercase hexadecimal digits, without #.
name encoded UTF8 hex: uppercase hex of the bounded UTF-8 name, at most 96 hex
digits (48 bytes), no BOM. Commas and quotes in names are safely hex encoded.

The original five fields retain their order and meaning. Existing controller
parsers can read the five-field prefix and ignore appended display metadata.
Controller acknowledgements accept LFA2_ACK,<device id>,<client> or
LFA2_ACK,<device id>,<client>,<effectiveColorHex6>. A valid optional six-hex
color updates the preview JSON color in memory. Direct LFA2 continues sending
the installed color so the controller can resolve its own override or collision.

Preview UDP (default port 4212) remains JSON version 1 with existing client,
activity (0..1), and remaining fields, plus name (plain JSON string) and color
(six uppercase hex digits without #). Receivers may ignore the new properties.

WEEKLY AND LIFETIME LOCAL TOKENS (LFT2)

See TOKEN_ACCOUNTING_V2.md for accounting, migration, recovery and limitations.
weekly_tokens.ps1 remains required beside the worker. The Version 2 state stores
Total (current Kyiv Monday week), LifetimeTotal (available local session logs),
Revision and PublishedSnapshot, plus incremental file checkpoints and identities.
These totals are not account billing. Cached/reasoning subsets are not added twice.

Every 15 seconds, a committed snapshot is sent over UDP and HTTP /agent:
  LFT2,ACCOUNT,CLIENT,WEEK,WEEKLY,LIFETIME,REVISION,COLOR,NAMEHEX

Nine ASCII fields, fewer than 200 bytes including the maximum name. The controller
must support LFT2 and a 200-byte transport limit. There is no LFT1 fallback.
LFT2_ACK,DEVICE,CLIENT acknowledges weekly reports. LFA2 heartbeats and their ACK,
connectivity, discovery and color semantics are unchanged. HTTP activity remains
on its two-second cadence; requests retain the 750 ms deadline and a 200-byte
response bound. Token retries never run every heartbeat.

Revision is at most 281474976710655 and advances only on first commit or when
(Week, Total, LifetimeTotal, ReportIdentity) changes. Unchanged scans persist checkpoints without
revision-only controller writes. Controller ordering must use revisions, allowing
corrected totals to decrease: never sum retransmissions or retain an inflated max.
ReportIdentity includes transmitted account/client/color/name. Metadata changes
advance revision once; unchanged scans remain stable. Sends wait for the matching
committed identity, so an old revision never carries newly changed metadata.

The first V1 migration preserves the exact original checkpoint and metadata,
then rebuilds from local logs. Explicit session lineage and exact retained
cross-file predecessors anchor inherited initial usage; ordinary cumulative jumps
use counter differences. Rollbacks reset only the affected channel. Copied
prefixes are deduplicated within known families (globally for metadata-free logs).
Without lineage or an exact predecessor, inherited first totals remain ambiguous.
Deleted logs cannot be recovered by migration. See the full token document.

Scanning remains off the heartbeat thread, with shared reads, bounded records,
partial/future-record replay, atomic commits, and no overlapping worker scans.
Weekly rollover retains lifetime history. Only the current committed week is sent.

Focused tests (temporary files only, no installed worker or controller):
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_weekly_tokens.ps1
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_weekly_token_v2.ps1
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_weekly_token_backlog.ps1
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_weekly_token_http.ps1

CLAUDE CODE DAILY LOCAL TOKENS (LFD2)

The agent counts local Claude Code assistant usage by Kyiv calendar day,
including uncached input, output, cache writes and cache reads. A response
counts once by message ID across session, fork and subagent logs. It sends
absolute daily totals under the existing paired account and client over both
UDP and HTTP /agent. Claude and Codex daily reports share one bounded send
schedule. LFD2 requires controller support. See TOKEN_ACCOUNTING_V2.md for
coverage, checkpoint behavior and the packet and ACK formats.

  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_claude_daily_tokens.ps1
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_daily_token_http.ps1

REPLACE A CONTROLLER (run separately on each agent PC)

QUICK MIGRATION TO THE NEW S3 (EB2198)

1. Extract the entire updated ZIP to a new folder, not the installed agent folder.
2. Sign in as the Windows user who originally installed this PC's agent.
3. Double-click rebind_to_s3.cmd. Current target: 10.0.80.162, ID EB2198.
4. Wait for completion. Open http://10.0.80.162/agents and check this agent's name.

Do not uninstall the existing agent first. The migration retains the original
agent-metadata.json bytes (name, local fallback color, extra metadata), client ID,
account fingerprint, preview collector address and configured ports. It replaces
the controller address/device pin, updates the agent scripts and restarts the
agent. Autostart is updated too. It does not ask for a new name or color.

To preview without changing anything, open PowerShell in the extracted folder:

  .\rebind_to_s3.cmd -WhatIf

If the S3 address changes before migration:

  .\rebind_to_s3.cmd -DirectHost 10.0.80.162

Replace the example IP with the current S3 IP. Its expected ID remains EB2198.
Once rebound, normal DHCP recovery searches for this same device if its IP changes.
Recovery is limited to reachable known/current private subnets; routing/firewall
isolation can still prevent discovery. No continuously running relay PC is needed.

An unreachable controller or mismatched ID is rejected before installed changes.
If startup fails after files are updated, retain the displayed logs and rerun the
command after resolving the error. Do not delete your metadata. The endpoint cache
backup alone is not a complete rollback of the updated scripts/autostart settings.

Colors saved only in the OLD controller's NVS are not in the agent metadata and
cannot be recovered by this archive. Select them again on the new S3 /agents page.
No Codex login credentials, installed per-user metadata or logs are in the ZIP.

MANUAL CONTROLLER SELECTION

For a deliberately selected replacement, verify its IP and six-digit controller
ID on the controller page, then preview the installer plan:

  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\install_codex_lan_agent.ps1 -CollectorHost 255.255.255.255 -DirectHost 10.0.80.162 -ControllerDeviceId EB2198 -WhatIf

Run the same command without -WhatIf to install/restart this PC's agent. The IP
and ID above are this replacement's current values; use its current IP if DHCP
changes it. The installer performs only GET /health before making changes and
rejects an offline, redirecting, or wrong-device endpoint. It does not scan.
WhatIf also performs this read-only identity check, but changes no files or
processes. A six-digit ID is the existing device pin, not authentication.

Existing agent metadata is retained unchanged unless -AgentName or -ColorHex6
is explicitly supplied. For a controller-only custom color, supply its known
six-digit color with -ColorHex6 to migrate that fallback color. Controller NVS
overrides are not copied automatically. Client identity and account stay the
same. The previous endpoint cache is backed up as controller-endpoint.json.*.bak;
the new cache is explicitly pinned to the selected device before startup.
Normal DHCP recovery continues to reject other device IDs. No synthetic
activity or pairing heartbeat is generated by the selection helper.

TESTS (source scripts only; no installation or real account polling)

  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_controller_rebind.ps1
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_s3_migration.ps1
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_codex_lan_agent.ps1
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_codex_lan_agent.ps1 -ExplicitMetadataPath -MutexState Abandoned
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_controller_discovery.ps1

Runs metadata and local-activity regression checks, then isolated loopback UDP
and HTTP checks with temporary sessions, config, ports, and a separate mutex.
The first command launches a temporary script copy via powershell.exe -File
without -MetadataPath, checking default config discovery beside the script from
a different working directory. The second checks an explicit config override.
Startup exits fail the test with the child process exit code and error output.
Both commands verify that an actively owned mutex blocks duplicate telemetry.
They also check that the installer's startup check rejects that exit 0 and
accepts the running test agent, without running the installer itself.
The first then checks an existing unowned mutex; the second retains an abandoned
mutex after killing a temporary owner. Both must allow the agent to start.
The agent acquires with WaitOne(0), treats abandonment as acquired ownership,
and releases only its own acquisition in finally. Existing handles alone do
not mean another agent is running.

The discovery regression uses a loopback single-client HTTP server to ensure
the first connection carries GET, plus controlled DHCP migration, foreign-device
rejection, persisted endpoint, private-address bounds, and retry backoff checks.
Loopback UDP checks reject delayed/wrong-port ACKs and validate HELLO recovery:
foreign announcements and announcements without a valid ACK never link an account.

Installed files and log:

  %LOCALAPPDATA%\LED Forge\Codex LAN Agent

To remove the agent, run uninstall_codex_lan_agent.cmd from this package.

For optional live preview, run configure_collector_firewall.cmd once on the LED
Forge preview PC and accept the Windows administrator prompt. It allows inbound
UDP 4212 only from the private local subnet.

SIGNED REMOTE AGENT UPDATES (manual bootstrap required)

Build locally from the repository root:
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File tool\build_agent_release.ps1 -Version 1
Use strictly increasing positive integer releases, at most 9007199254740991.
The outputs under build\agent-release are manifest.json, package.zip,
agent_release.hpp, agent-update-public.xml, and bootstrap.zip. The header exposes only
agent_release::version (uint64_t). The publisher private RSA XML key stays under
%LOCALAPPDATA%\LED Forge\Agent Publisher\agent-update-private.xml, with access
restricted to the current Windows user. Keep that key backed up privately;
losing or changing it requires manual public-key bootstrap on every agent.
Never put the private key or installed identity cache into a distribution.

Concrete manual bootstrap directory layout:
  Extract generated bootstrap.zip and run its install/update wrapper. This archive
  includes the exact signed-release code/version, generated public key, and the
  explicit installer/support file set; it never takes version 1
  from the source directory when building release N. It contains no private keys
  or installed state. Manual staging equivalent:
  Extract package.zip's flat code/version files into a new directory.
  Copy install_codex_lan_agent.cmd/.ps1, update_codex_lan_agent.cmd/.ps1,
  and controller_rebind.ps1 into that directory.
  Copy agent-update-public.xml from the generated release. The installer scripts
  come from tools\codex-lan-agent; only the public XML comes from the release.
  Optional rebind/uninstall wrappers may also be copied from the source directory.
  Do not copy any existing installed agent state into this bootstrap directory.
  Install/update manually once. The extracted agent-version.txt must be kept
  beside the matching extracted release scripts, never replaced with a source
  version file. The default repository source version is 1.

The source directory deliberately contains no public XML. The installer also
looks for ../../build/agent-release/agent-update-public.xml when run from source,
or accepts -AgentUpdatePublicKeyPath. Missing public keys safely disable updates.
Public-key replacement is permitted only by this manual installer, never by a
remote package. A manual installer run preserves existing config bytes; explicit
name/color/controller changes retain their existing manual semantics.

Before enrollment, controller-endpoint.json must already pin Host, HttpPort,
Account 53197668 and Device through ordinary controller discovery/binding.
Telemetry discovery and pinning retain their existing behavior, including DHCP
host refresh for the pinned device. Update readiness requires helpers, public key,
version and pinned endpoint; it does not require a cached enrollment identity.

After installing the bootstrap once, use the controller web update button.
No operator key, enrollment command, or additional setup step is required.
The downloader automatically POSTs to the pinned /agent-update-enroll when its
identity cache is missing or invalid:
  client=<8HEX>&account=53197668
The controller requires a known online agent. The returned {device,secret} must
match the pinned device and contain a secret of exactly 64 lowercase hex characters.
The secret is cached privately in agent-update-identity.json; a valid cache is reused.
Enrollment uses the existing trusted LAN HTTP connection. If challenge/poll returns
HTTP 403, the downloader refreshes enrollment once and retries with a fresh challenge.
A second 403 stops that attempt; other failures do not trigger enrollment refresh.
Readiness is checked every 15 seconds, so the normal downloader can enroll after the
agent comes online. No controller discovery or binding changes are required.
The identity is never distributed, packaged, backed up or rolled back by updates.
The installer preserves existing identity state, metadata and configuration.

Every 15 seconds a separate downloader process first POSTs /agent-update-challenge:
  client=<8HEX>&account=53197668
The response {device,nonce} must match the pinned device and contain exactly
32 lowercase hex nonce characters. This opaque controller-issued challenge binds
the current boot and 10-second monotonic window. The controller enforces its
roughly 20-second expiry, boot identity, and per-client accepted-window replay
limit. The agent appends a fresh RNGCryptoServiceProvider-generated 32-lowerhex
suffix on EVERY invocation, forming the 64-lowerhex poll nonce. A replayed server
challenge plus an old valid response fails because that response cannot echo
the new client random suffix. A challenge alone
does not authorize an update: the subsequent response MAC remains mandatory.
The downloader then POSTs /agent-update-poll:
  client=<8HEX>&version=<committed decimal>&account=53197668&nonce=<64lowerhex>&mac=<64lowerhex>
Request MAC = HMAC-SHA256(secret decoded as bytes, UTF8 exact):
  LFAU1-POLL\n<DEVICE uppercase>\n<CLIENT uppercase>\n<nonce lowercase>\n<installed decimal>\n
When a recorded failed release is newer than the committed installed version,
the poll also carries failed=<failed decimal>, and the request MAC message
appends exactly FAILED\n<failed decimal>\n. The response MAC stays unchanged.
This authenticated status lets the controller clear pending work and expose a
rollback/manual-check state. A successfully committed version is not reported
as failed, even though its conservative local attempt marker remains present.
Response JSON: {device,version,nonce,mac}, with target 0 for no pending update.
Response MAC uses UTF8 exact:
  LFAU1\n<DEVICE uppercase>\n<CLIENT uppercase>\n<nonce lowercase>\n<target decimal>\n
Nonce echo, device pin and constant-time MAC comparison are mandatory before action.
Polls never transmit a secret and never report a provisional version. Network
redirects, proxy routing, URLs/commands supplied by a response are not supported.
Downloads use only /agent-update/manifest.json and /agent-update/package.zip at
the same pinned host/port captured for that poll.

Manifest: {schema:1,version:<safe integer>,size:<ZIP bytes>,sha256:<lowerhex>,signature:<base64>}.
Signature: RSA-SHA256/PKCS#1 v1.5 over UTF8 exact:
  LFAU1\n<version decimal>\n<size decimal>\n<sha256 lowercase>\n
The agent requires a newer version, a valid pinned-key signature and exact size
and SHA256. ZIP <=512KB, expanded <=2MB. Only the following flat filenames are allowed:
  codex_lan_agent.ps1, start_agent_supervisor.ps1, agent_metadata.ps1,
  weekly_tokens.ps1, agent_chat_ipc.ps1, agent_chat_ui.ps1,
  agent_update.ps1, agent_update_download.ps1,
  agent_update_restart.ps1, agent-version.txt
Names compare case-insensitively; duplicates, missing entries, links/reparse
points, directories, alternate streams, traversal and extra files are rejected.

The running worker remains active while polling/downloading/verifying. The
supervisor verifies again, requests graceful stop, waits at most 15 seconds,
then installs only code. If graceful stop times out, installation is abandoned.
Each file replacement is atomic and flushed, backed by a durable journal and
complete code backup. A pending journal forces startup recovery before launch.
The replacement worker must produce fresh health heartbeats for 15 continuous
seconds within a 60-second probation. Early exit or timeout restores old code.
Polling pauses throughout probation. After health commit, the old supervisor
stops the child within 15 seconds (bounded forced termination if needed), starts
the fixed restart helper and exits. The helper validates typed parameters,
waits for that exact old process (PID plus start time) to exit, then launches the
known supervisor file. This reloads the updated supervisor/helpers immediately
without racing the old mutex. No command text comes from a controller response.
Weekly state, all metadata/config, endpoint, public key and identity are never
rewritten by update/rollback. Normal worker telemetry/scanning still updates its
own operational state as usual.

Each attempted install is recorded in .agent-update-failed-version.txt before
code changes. A failed/interrupted release cannot retry or accept any older
release; only a newer target is allowed. For deliberate manual retry, stop the
supervisor, resolve the cause, ensure pending-journal recovery completes, remove
that file from the installed directory, and restart. A successful commit retains
the marker harmlessly because installed-version checks already reject it.

Claude quota recovery
---------------------
The agent reads the five-hour quota only for marketing@tiras.ua and verifies
the account and organization with Anthropic before reporting a percentage.
When the Claude Code credential has expired, an existing matching Claude
Desktop session can supply a current credential. Desktop storage is read-only;
the agent never changes its session or copies its refresh token into CLI files.
If no matching Desktop session is available, the agent reports unavailable
quota until Claude renews its own session. The agent does not rotate refresh
tokens or overwrite another application's credential files. Authorization
failures and recovery are recorded without credentials.
Ordinary failures retry after 15 seconds with bounded exponential backoff;
server rate-limit delays are respected. Unknown or stale quota is never 100%.

Isolated updater tests (no installed-agent access or upgrade payload execution):
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_remote_update.ps1
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_update_supervisor.ps1
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_update_supervisor.ps1 -Scenario Healthy
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_update_supervisor.ps1 -Scenario LateEnrollment
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_update_supervisor.ps1 -Scenario NoIdentity
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_update_restart.ps1
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_update_enrollment.ps1
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test_release_builder.ps1
