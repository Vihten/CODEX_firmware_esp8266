# Token activity calendar

The Agents page keeps the weekly agent ranking and shows exactly seven token
activity squares for the current Monday–Sunday calendar week, using the
controller's Kyiv date. The heading shows the full week range and the summary
adds only known daily totals. There is no year grid, aggregation-mode switch,
or weekly details table. The underlying history still retains 366 dates.

Unknown days are distinct from a reported zero; future days stay unknown.
Older weekly/lifetime totals are never distributed across invented daily buckets.
The blue-to-white scale is relative daily token activity within the visible week,
not account quota consumption.

Hover, keyboard focus, or click opens the day's tooltip with its date, token
total, and account quota availability. Pointer leave hides an unpinned tooltip
unless its day remains focused; click pins it and Escape closes it. Arrow Left
and Right move one day, and Home/End select Monday/Sunday. Polls preserve focus;
a new calendar week replaces all seven visible dates.

Daily account quota is not derivable from tokens. The existing agent receives
usage percentages for account limit windows, not an authoritative daily budget.
The tooltip therefore reports daily quota as unavailable. Incidental percentage
fields are ignored, including zero and 100; a day's fraction of weekly tokens
must never be presented as quota consumption.

Updated agents send independent, date-keyed LFD1 reports through the existing
agent transport. Daily corrections use revisions and replace a day's value;
repeated reports do not add tokens. Existing identities, settings and weekly
statistics are retained. Update agents through the controller's existing signed
update mechanism to populate the calendar; old agents continue weekly reporting.
The release is signed by the current local publisher key. Agents trusting only
the different key in release 20261104163700 must first run the bootstrap archive's
update command once; signature validation is not bypassed. Preserve the existing
installation folder to retain identity, settings, and statistics.

The controller holds the bounded history in PSRAM and checkpoints each agent's
CRC-protected compact blob to NVS during maintenance, normally every five minutes
and before a requested restart or OTA. A sudden power cut can lose the latest
uncommitted interval. Agents retain their own daily evidence and replay reports
to repair it after reconnection. Corrupt controller history is reported as
unavailable rather than silently overwritten with empty data.
