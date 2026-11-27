# Windows chat mailbox v1

Run agent_chat_ui.ps1 in the interactive user session. Both processes use the same per-user directory, default %LOCALAPPDATA%\CodexLanAgent\chat. Pass -MailboxRoot to override. No network access is required by the UI.

Publish complete UTF-8 JSON using a temporary file in the destination directory and atomic rename (atomic replace for roster updates). Readers ignore temporary files. IDs are opaque, case-insensitive strings. Message IDs are UUIDs in canonical hyphenated form.

| Path | Writer | JSON |
| --- | --- | --- |
| roster.json | worker | {"self_id":"00000001","agents":[{"id":"00000001","name":"Vlad","online":true},{"id":"00000002","name":"Nestor","online":true},{"id":"board","name":"Board","online":true}]} |
| outbox/<id>.json | UI | {"id":"uuid","recipient_ids":["peer-id"],"text":"message"} |
| inbox/<id>.json | worker | {"id":"uuid","sender_id":"peer-id","sender_name":"Nestor","text":"message"} |
| seen/<id>.json | UI | Original inbox file moved after display |

The worker owns outbox consumption, retry, deduplication by message ID, and presence freshness. Include known offline agents in snapshots. The UI enables resolved online recipients only. The worker must revalidate recipients and reject self delivery. Queued indicates local handoff. The UI normalizes Windows newlines to LF before enforcing the 512-byte UTF-8 limit. Keep queued text unchanged. Include the local agent row with online true, or provide self_name alongside self_id for local desk identification. Publish the board with ID board for explicit selection. Use unique message IDs across senders.

Windows transport release handoff: retain the packaged names agent_chat_ipc.ps1 and agent_chat_ui.ps1. The former now owns file mailbox/REST integration. Pairing is an explicit tray action. The transport uses the confirmed telemetry controller endpoint and separate PC-generated chat key.
Pairing launcher contract: transport supplies Show-AgentChatPairing -Root $MailboxRoot -EndpointPath (Join-Path $PSScriptRoot 'controller-endpoint.json'). The tray menu calls this helper on click. The helper loads a per-user DPAPI key, displays the PC ID, key and fingerprint on demand, and opens the controller-hosted approval form with only client ID in the URL. Transport worker owns this helper and the single menu hook. UI worker owns other UI changes. No pairing command files.
