# Token accounting V2

`weekly_tokens.ps1` publishes `Total` as the sum of known dated daily observations
from the current Europe/Kyiv Monday through today, `LifetimeTotal` as retained V2
cumulative local history, and `Revision`. These are local-log totals, not account
billing or an account-wide usage API. Calendar and weekly may be incomplete.
Cached input and reasoning output are subsets; only input + output are counted.

## Shared dated weekly/calendar ledger

The publishing worker embeds `DailyLedger` in the V2 weekly checkpoint. Weekly,
lifetime, daily rows and their revisions are saved in one atomic replacement,
then returned for transmission. Weekly is calculated from the same daily rows;
there is no independently committed calendar/weekly window. Ledger corruption,
per-day overflow, weekly-sum overflow or commit failure prevents the new shared
snapshot from being published. Heartbeats remain independent. Failed workers are
recreated so subsequent polling can recover after the underlying error is fixed.

On the first observation of a session, only valid `last_token_usage` is attributed
to that date. An unknown initial cumulative baseline is excluded from dated usage.
Subsequent observations use cumulative differences, including multiple rounds;
channels reset independently. Copied prefixes deduplicate within a known family.
This is the existing daily rule, now also the published weekly rule. It is not a
size threshold: a large independently observed delta still counts in full.

The low-level `Update-WeeklyTokenState`/`Add-WeeklyTokenLine` helpers retain V2
counter arithmetic for lifetime and legacy regression coverage. Their intermediate
`Total` is historical cumulative attribution; only the worker's shared committed
snapshot defines published weekly usage. No other production caller uses that
intermediate total.

### Deployment and migration

An existing V2 checkpoint without `DailyLedger` is migrated on its next successful
worker scan. The exact checkpoint is retained as `.pre-dated-weekly.bak` (never
overwritten). Existing `.daily-v1.json` evidence is imported, including cached
observations from missing/rotated logs. If absent, dated evidence is rebuilt from
retained logs; if corrupt, migration fails closed and preserves the old checkpoint.
The sidecar is then obsolete and retained unchanged; the embedded ledger is
authoritative. Do not delete checkpoints/sidecars to force this migration.

Weekly can decrease during this explicit recount and after evidence corrections.
A changed report tuple advances the normal LFT2 revision, including lower totals.
Daily correction rows retain their own revisions and zero tombstones. The wire
protocol and account/client identity behavior are unchanged. Receivers must accept
newer-revision lower totals; an old receiver's monotonic maximum will need its own
migration. Keep firmware diagnostic cumulative values separate from dated UI totals.

Healthy V2 lifetime, file offsets, counter epochs and dedup evidence are preserved;
the weekly recount does not reset or decrement lifetime. Existing lifetime
inflation cannot be inferred from a calendar discrepancy. Correcting it requires
an explicitly reviewed recount from sufficient historical evidence. Consequently
weekly/calendar equality does not certify lifetime accuracy. The initial V1
migration still rebuilds V2 lifetime from retained logs as described below.

Deploy the verified helper as part of the signed agent bundle using the project's
existing release process. This source task does not sign, package, install or
publish agents, firmware or release/version files. An older agent must not write
the migrated checkpoint: it does not understand/preserve the embedded ledger.

## Lineage and recovery

The scanner reads session_meta.id and session_meta.forked_from_id, scans available
parents first, and deduplicates timestamped cumulative prefixes within a known
family. Metadata-free copies retain the legacy global timestamp/counter identity.
A child's initial inherited baseline is total minus valid last_token_usage only
when explicit fork lineage or an exact earlier cross-file predecessor supports it.
Known incompatible families cannot supply that predecessor. Ordinary cumulative
jumps always use counter differences, including jumps spanning multiple rounds.
Each counter channel resets independently when it decreases. Repeated snapshots
from the same source retain the earliest timestamp, so a later status echo cannot
erase a valid earlier cross-file predecessor and replay a fork's inherited baseline.

Without both lineage and an exact retained predecessor, inheritance cannot be
proven. The first observed cumulative value is retained; migration cannot promise
to remove inherited inflation for such incomplete logs. Metadata-free unrelated
sessions with identical timestamp/counters are also inherently ambiguous.
Header timestamps order metadata-free files; absent/malformed headers can prevent
cross-file predecessor inference. Explicit parent graphs take precedence.
Available initial cumulative history remains part of lifetime even if earlier
individual events are absent. It is not dated weekly/calendar evidence.

First V1 migration rebuilds from raw logs, ignoring old inflated totals/checkpoints.
The exact original state is retained as .pre-v2.bak (unique suffix if already
present), and its fields are preserved in Metadata. Corrupt states get a unique
.corrupt.*.bak and a rebuild. Rebuild results depend on retained logs; removed logs
cannot be recovered from an inflated/corrupt checkpoint. Healthy V2 checkpoints
retain already counted lifetime usage across deletion, truncation and week rollover.
File reads allow shared writers/deletion. Incomplete records and after-cutoff
records are replayed; oversized records are bounded and skipped. Only a durably
committed full scan is published. PowerShell 5.1 and 7 date representations work.

## Wire contract

`LFT2,ACCOUNT,CLIENT,WEEK,WEEKLY,LIFETIME,REVISION,COLOR,NAMEHEX`

Nine fields, ASCII, fewer than 200 bytes including the longest 48-byte UTF-8 name
encoded as 96 hex digits. Both UDP and HTTP /agent carry the same committed body.
The receiving controller must support LFT2 and the new 200-byte transport capacity.
There is no LFT1 fallback. Only `LFT2_ACK,DEVICE,CLIENT` is a token acknowledgement;
it does not alter activity, discovery, connection state or color. Existing LFA2
heartbeats remain unchanged. Unsupported controllers must ignore LFT2 packets.
Token reports retry every 15 seconds; failed HTTP bodies have a bounded deadline.

Revision is a committed reporting snapshot ID in [1, 281474976710655], using
max(UTC milliseconds, previous revision + 1) only on first commit or a changed
(Week, Total, LifetimeTotal, ReportIdentity) tuple. PublishedSnapshot stores that tuple. Checkpoints
still persist on unchanged scans, but their revision stays stable: no revision-only
controller NVS writes every 15 seconds. Week rollover changes revision even at zero
usage. Older V2 checkpoints missing PublishedSnapshot get a fresh revision once.
Failed commits restore the previous in-memory revision and published tuple.
ReportIdentity contains the transmitted account, client, color and name hex. Metadata
changes advance revision once; unchanged metadata does not. The sender waits for a
committed matching identity before sending, avoiding stale-revision metadata races.
A lost/corrupt checkpoint cannot preserve an unknown future revision from that
checkpoint; backups retain diagnostic evidence.

## Verification

Run test_weekly_tokens.ps1, test_weekly_token_v2.ps1, test_shared_token_ledger.ps1,
test_daily_tokens.ps1, test_daily_token_http.ps1, test_weekly_token_http.ps1 and
test_weekly_token_backlog.ps1 with Windows PowerShell. Unit suites also support
PowerShell 7. Shared tests cover V2 downward weekly migration, exact backup,
sidecar import, lifetime/checkpoint preservation, replay, reset, identity, rewrite,
missing logs, week rollover, corruption and overflow failure gating/recovery.
An optional local token-only audit fixture checks the actual publishing worker
against independent arithmetic; no dialogs are read and private audit files are
not distributed with the release. A partial audit cannot certify full-account
usage.

Deploy only to a controller with LFT2 transport/ACK support. Updating an installed
agent preserves its identity, settings and checkpoint backups.

## Claude Code daily totals

The Windows agent reads newline-terminated JSONL records under
`%USERPROFILE%\.claude\projects`, or `projects` under `CLAUDE_CONFIG_DIR`
when set. The agent's `-ClaudeRoot` parameter overrides that path. The
recursive scan covers local sessions, forks and nested subagent transcripts.
Coverage depends on the logs present under that root and the usage each record
contains. Local totals cannot measure cloud sessions, other machines, missing
logs, or account-wide usage. Quota polling and credentials are independent of
this scanner.

A valid assistant response contributes `input_tokens + output_tokens +
cache_creation_input_tokens + cache_read_input_tokens`. The two cache fields
default to zero if absent or null. Cache TTL breakdowns are already included in
cache creation, and other nested counters are excluded. Claude's uncached input
and cache counters are separate categories. [Anthropic token breakdown](https://platform.claude.com/docs/en/build-with-claude/prompt-caching#understanding-the-token-breakdown)

Each case-sensitive `message.id` counts once across this agent's transcripts.
Repeated content blocks and copied fork prefixes can carry the same response
usage even when transcript UUIDs differ. The largest valid complete usage
snapshot wins, and the earliest observed response timestamp places it on a
Europe/Kyiv calendar day, including daylight saving changes. Records with no
usable message ID, synthetic responses, invalid timestamps or invalid counters
are skipped. Timestamps up to a day ahead are held until their time arrives.
An earlier copy found later can move usage to a different day and cause a lower
revisioned total or a zero correction. IDs regenerated by a transcript producer
cannot be linked to their originals.

The separate `<TokenStatePath>.claude-daily-v1.json` checkpoint stores byte
cursors, hashed message IDs, timestamps, counts and daily revisions for up to
366 Kyiv dates. It keeps observed usage when logs rotate, disappear or are
rewritten. Truncation, a changed file creation time, same-size edits with a new
write time, and changed sampled bytes around the cursor trigger a reread.
In-place changes outside the sampled bytes during file growth may go undetected.
The checkpoint contains
local transcript paths and scanner metadata. Transcript content is confined to
the local reader. Only the day, total, revision and existing pairing/display
fields go over the wire.

The background worker attempts a scan every 15 seconds. It skips reparse points,
uses at most eight directory levels, and processes at most 128 changed
transcripts and 8 MiB of transcript data per scan, plus small cursor samples.
An individual file gets at most a 1 MiB slice per scan. Each slice checkpoints
its progress, so initial backfill can span multiple scans. Unchanged scans keep
the existing checkpoint and revisions. A record over 256 KiB is skipped, and
an incomplete trailing record waits for its newline. The worker
fails closed if the checkpoint is corrupt, a total overflows, or discovery or
evidence exceeds the safety bounds of 65,536 directory entries, 8,192
transcripts, 65,536 retained message IDs, and a 32 MiB checkpoint size. Codex's
worker runs independently.

```text
LFD2,ACCOUNT,CLIENT,DAY,TOKENS,REVISION,COLOR,NAMEHEX
LFD2_RAM_ACK,DEVICE,CLIENT,DAY,REVISION
```

LFD2 uses the same paired account, client, six-hex RGB and UTF-8 name hex as
LFD1. DAY is `YYYYMMDD` in Europe/Kyiv, and TOKENS is the agent's absolute
Claude total for that day. Each packet is ASCII and under 200 bytes. The agent
sends identical bodies over UDP and HTTP `/agent`. The Claude and Codex daily
backlogs alternate through one two-second daily send slot. A due weekly report
gets the first turn, and an overdue daily report gets the next. The sender uses
the controller's 28 Kyiv dates for Claude and 366 for Codex. Within each
provider, changed revisions and new dates get priority turns interleaved with
chronological replay from newest to oldest. The ACK confirms RAM acceptance for the matching client,
controller, day and revision. Both daily backlogs keep replaying cyclically to
recover from loss or controller reboot. Claude totals require a receiver that
supports LFD2.

Run `test_claude_daily_tokens.ps1` for checkpoint and accounting coverage and
`test_daily_token_http.ps1` for Claude/Codex scheduling, UDP/HTTP parity and
acknowledgement coverage.
