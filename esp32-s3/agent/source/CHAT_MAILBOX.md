# Windows chat mailbox v2

The tray and background worker share the signed-in user's mailbox at
`%LOCALAPPDATA%\CodexLanAgent\chat`. The tray accepts `-MailboxRoot` for an
isolated session. Publish complete UTF-8 JSON using an atomic move or
replacement in the destination directory. Readers ignore temporary files.

| Path | Writer | Contract |
| --- | --- | --- |
| `roster.json` | Worker | Local `self_id`, public `self_fingerprint`, verified `agents`, and discovered `candidates`. A verified PC row carries its ID, trusted name, fingerprint, and online state. |
| `trusted/<ID>.json` | Tray | Immutable, user-approved `id`, `name`, and SHA-256 `fingerprint`. Each side verifies the other independently. |
| `outbox/<uuid>.json` | Tray | Original `id`, `recipient_ids`, and `text`. The UI normalizes newlines to LF and enforces the 512-byte UTF-8 limit. |
| `deliveries/<uuid>.json` | Worker | Draft digest, queue time, and per-recipient `targets` with `id` and `state`. States are `pending`, `delivered`, `failed`, and `uncertain`. |
| `board-attempts/<uuid>.json` | Board worker | Independent progress for an explicitly selected board. The peer worker merges the board result into the delivery journal. |
| `inbox/<uuid>.json` | Peer receiver | Durable message containing its UUID, sender ID, trusted sender name, sender fingerprint, and text. Receipt follows durable storage. |
| `seen/<uuid>.json` | Tray | Original inbox record moved after display. The receiver consults both directories when handling a retry. |
| `failed/<uuid>.json` | Worker | Original draft retained when one or more deliveries failed or became uncertain. The delivery journal identifies recipients that already received it. |

Peer IDs are eight uppercase hexadecimal characters. Message IDs are lowercase
hyphenated UUIDs. A worker rejects self-delivery and duplicate recipients.
Discovery supplies candidates. The tray requires explicit fingerprint
approval before a discovered PC can receive text. Trust changes require a new
verification. See [Office chat](OFFICE_CHAT.md) for the reimage procedure.

The board recipient has ID `board`. Board pairing uses the confirmed telemetry
endpoint and a separate per-user key. The tray displays the PC ID and key on
request and opens the controller approval page with only the client ID in its
URL. Board HTTP runs independently of direct peer delivery. Legacy
`attempts/<uuid>.json` records yield uncertain results so an upgrade does not
silently replay previously attempted messages.

The signed web package carries peer code inside `agent_chat_ipc.ps1`. The
editable source loads the adjacent `agent_chat_peer.ps1`. The user-facing tray
remains `agent_chat_ui.ps1` in both layouts.
