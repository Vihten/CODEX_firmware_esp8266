# Token activity calendar

The Agents page keeps the weekly agent ranking and replaces the lifetime table
with a 366-day calendar of reported daily token totals across agents. Each cell
can be selected to inspect its day and Monday–Sunday week in Kyiv calendar dates.
Daily, weekly and cumulative modes change the color measure, not the source data.

Unknown days are distinct from a reported zero. Partial weeks are labelled;
older weekly/lifetime totals are never distributed across invented daily buckets.
The blue-to-white scale is relative token activity, not account quota consumption.

Daily account quota is not derivable from tokens. The existing agent receives
usage percentages for account limit windows, not an authoritative daily budget.
The `% ліміту` column therefore shows no data unless an explicit daily percentage
source is supplied. It must not display a day's fraction of weekly tokens.

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
