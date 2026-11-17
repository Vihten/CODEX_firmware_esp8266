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
