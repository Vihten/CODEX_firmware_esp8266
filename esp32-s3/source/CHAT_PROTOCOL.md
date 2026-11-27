# Chat relay v1

Windows agents with direct LAN messaging use this protocol for the optional
matrix destination. Only an explicit board selection needs a controller boot
nonce and an authenticated send with `X-Chat-To: board`. PC discovery, trust,
acknowledgments and desktop messages are handled between Windows agents.
The roster, inbox and acknowledgment endpoints below remain available to
older agents while installations are upgraded.

Machine paths are exact POSTs. The board serves HTTP on the existing trusted LAN. Provision only on a protected link. Chat bodies are plaintext. Enrollment uses the existing logged-in portal session and its same-origin check. Generate a fresh random 32-byte chat-only key on the PC. POST /chat/enroll with application/x-www-form-urlencoded fields client (8 uppercase hex) and key (64 lowercase hex). The operator must approve the actual PC/key pair. The client must be in current telemetry. Repeating the same binding is safe. A different key cannot replace a live binding. Approved bindings persist in NVS. Enrollment returns 202 while the guarded flash commit is pending and 200 for an already durable binding. Signed operations require the durable binding. Boot clears sequence counters, inboxes, receipts, and board deliveries. Never retry an uncertain send across boot automatically.

POST /chat/state with empty body returns {"boot":"16 lowercase hex"}. Every other machine request uses X-Chat-Client, X-Chat-Seq (positive decimal integer up to 9007199254740991, strictly increasing per PC per boot), X-Chat-Mac (lowercase HMAC-SHA256 hex with raw enrollment key). Keep sequence locally, serialize requests; UTC milliseconds with monotonic increment is suitable. Sign the UTF-8 bytes of LFCHAT1\n{boot}\n{client}\n{seq}\n{path}\n{id}\n{to}\n{body} with LF separators, no implicit final LF. X-Chat-Id and X-Chat-To absent mean empty. Fresh sequence on every retry. Verify X-Chat-Reply on every success: HMAC over LFCHAT1-REPLY\n{boot}\n{client}\n{seq}\n{response body}. Ignore unsigned successes.

* /chat/roster empty request returns {"self_id":"00000001","agents":[{"id":"00000002","name":"...","online":true},{"id":"board","name":"Табло","online":true}]}. Includes the authenticated PC with online true and enrolled peers. Board is always an explicit selection.
* /chat/send uses X-Chat-Id canonical lowercase UUID, X-Chat-To comma-separated distinct uppercase 8-hex IDs and/or board. Raw UTF-8 text 1..512 bytes, LF allowed. Keep ID, text and recipient set on retry. Atomic all-or-none. Returns {"ok":true}. Unknown/offline/self recipients fail. Exact retries succeed even after ACK/offline. UUID reuse with different content or sender conflicts.
* /chat/inbox empty request returns {"messages":[]} or one oldest pending {"messages":[{"id":"uuid","sender_id":"00000001","sender_name":"...","text":"..."}]}. Persist locally by UUID before ACK.
* /chat/ack empty request with X-Chat-Id clears only caller's copy. Repeated/unknown ACK succeeds.
* 400 invalid, 403 authentication, 409 replay/conflict/unavailable recipient, 413 oversized, 429 rate or capacity, 503 maintenance. Retry transient errors with backoff. 12 PCs, 8 pending deliveries per recipient including board, 24 shared message/receipt slots, 6 new sends per PC/minute, 60 authenticated requests per PC/10 seconds, 128 chat requests globally/second, ten-minute retention from acceptance. Deduplication expires with retention. Do not retry past retention. Boot/expiry before ACK can lose a message.
* Renderer includes chat_board.hpp. Call chatBoardTake(ChatBoardMessage&) only when ready to accept the next message. Nonblocking FIFO; successful take acknowledges board alone. Do not log chat text.

## Pair a Windows PC

1. Start pairing from the Windows tray. It opens `GET /chat/pair?client=XXXXXXXX` on the controller, where `XXXXXXXX` is the PC’s eight-digit uppercase hexadecimal telemetry ID. If prompted, sign in at `/login` on that controller, then reopen the pairing page from the tray.
2. Check the displayed PC ID, enter the PC-generated key in the password field, and approve the pair. The form submits `client` and `key` in the body of a same-origin `POST /chat/enroll` using the portal session. Only the PC ID appears in the pairing URL. The PC must have current telemetry.
3. A new pair receives `202 Accepted`. The controller stages the binding for a guarded NVS commit. A repeat submission with the same key receives `202` while pending and `200 OK` once durable. Signed chat operations become available after commit. A different key for the same PC receives `409 Conflict`.

The controller reloads committed bindings from NVS after restart. Each boot starts fresh sequence counters, inboxes, deduplication receipts, and board deliveries.
