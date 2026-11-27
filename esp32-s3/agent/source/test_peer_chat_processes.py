"""Windows integration test for peer chat. All sockets and identities are disposable."""

import contextlib
import hashlib
import hmac
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = Path(__file__).resolve().parent
BOOT = "0123456789abcdef"
TEXT = "literal chat: $(1+1) <button> snowman ☃"


def put(path, data):
    temp = path.with_name(uuid.uuid4().hex + ".tmp")
    temp.write_text(json.dumps(data), encoding="utf-8")
    os.replace(temp, path)


def read(path):
    import _winapi
    import msvcrt

    try:
        # Match the production mailbox reader. Sharing delete allows the
        # agent to atomically replace a journal while this observer reads it.
        handle = _winapi.CreateFile(str(path), _winapi.GENERIC_READ, 1 | 2 | 4,
                                    0, _winapi.OPEN_EXISTING, 0, 0)
        with os.fdopen(msvcrt.open_osfhandle(handle, os.O_RDONLY), encoding="utf-8-sig") as stream:
            return json.load(stream)
    except FileNotFoundError:
        return {}


def until(label, check, children, seconds=25):
    deadline = time.monotonic() + seconds
    while True:
        for child in children:
            if child.poll() is not None:
                raise RuntimeError(f"Agent process exited with {child.returncode} during {label}")
        try:
            result = check()
        except PermissionError:
            # Durable writers hold a brief exclusive handle on Windows.
            result = False
        if result:
            return result
        if time.monotonic() >= deadline:
            raise AssertionError("Timeout waiting for " + label)
        time.sleep(0.08)


class Board(ThreadingHTTPServer):
    def get_request(self):
        connection = super().get_request()
        self.connections += 1
        return connection


class Request(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        server = self.server
        server.paths.append(self.path)
        if not server.available:
            if server.hold_next:
                server.hold_next = False
                server.waiting.set()
                server.release.wait(timeout=2.4)
            self.send_error(503)
            return
        client = self.headers.get("X-Chat-Client", "")
        sequence = self.headers.get("X-Chat-Seq", "")
        mid = self.headers.get("X-Chat-Id", "")
        target = self.headers.get("X-Chat-To", "")
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
        data = {"boot": BOOT}
        if self.path != "/chat/state":
            key = server.keys[client]
            fields = ["LFCHAT1", BOOT, client, sequence, self.path, mid, target, body]
            signature = hmac.new(key, "\n".join(fields).encode(), hashlib.sha256).hexdigest()
            if not hmac.compare_digest(signature, self.headers.get("X-Chat-Mac", "")):
                self.send_error(403)
                return
            if self.path != "/chat/send" or target != "board":
                self.send_error(400)
                return
            server.accepted.append((mid, body))
            data = {"ok": True}
        reply = json.dumps(data, separators=(",", ":"))
        self.send_response(200)
        self.send_header("Content-Length", str(len(reply)))
        if self.path != "/chat/state":
            fields = ["LFCHAT1-REPLY", BOOT, client, sequence, reply]
            self.send_header("X-Chat-Reply", hmac.new(key, "\n".join(fields).encode(), hashlib.sha256).hexdigest())
        self.end_headers()
        self.wfile.write(reply.encode())


def queue(root, targets, text=TEXT):
    mid = str(uuid.uuid4())
    put(root / "outbox" / (mid + ".json"), {"id": mid, "recipient_ids": targets, "text": text})
    return mid


def states(root, mid):
    return {row["id"]: row["state"] for row in read(root / "deliveries" / (mid + ".json")).get("targets", [])}


def trust(root, other_id, name, pin):
    put(root / "trusted" / (other_id + ".json"), {"id": other_id, "name": name, "fingerprint": pin})


def verify(base, board, children, logs):
    import base64
    ids = [secrets.token_hex(4).upper() for _ in range(2)]
    assert ids[0] != ids[1]
    roots = [base / "alpha", base / "bravo"]
    pins = []
    # Exercise the production session and transport runspace without opening
    # a tray window or racing a popup that consumes the observable inbox.
    headless = base / "agent"
    headless.mkdir()
    for script in ("agent_update.ps1", "agent_chat_ipc.ps1", "agent_chat_peer.ps1"):
        shutil.copyfile(HERE / script, headless / script)
    (headless / "agent_chat_ui.ps1").write_text(
        "param($MailboxRoot,$ChatStopEvent)\n[void]$ChatStopEvent.WaitOne()\n", encoding="ascii")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.2", 0))
        port = probe.getsockname()[1]
    for index, root in enumerate(roots):
        root.mkdir()
        put(root / "endpoint.json", {"Host": "127.0.0.1", "HttpPort": board.server_port,
                                     "Account": "53197668", "Device": "123456"})
        log = logs.enter_context((base / (root.name + ".log")).open("w+b"))
        args = ["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                "-File", str(HERE / "test_peer_chat_process_host.ps1"), "-Root", str(root),
                "-Client", ids[index], "-Name", root.name, "-BindAddress", f"127.0.0.{2 + index}",
                "-DiscoveryAddress", f"127.0.0.{3 - index}", "-DiscoveryPort", str(port),
                "-HeadlessDirectory", str(headless)]
        if index:
            args.append("-Session")
        # Let Windows PowerShell discover its own inbox modules, including its
        # certificate provider, even when the test runner uses PowerShell 7.
        env = {k: v for k, v in os.environ.items() if k.upper() != "PSMODULEPATH"}
        child = subprocess.Popen(args, env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
        children.append(child)
        ready = until(root.name + " startup", lambda: read(root / "ready.json"), children, 45)
        assert ready["pid"] == child.pid
        pins.append(ready["pin"])
        board.keys[ids[index]] = base64.b64decode(ready["key"])
    a, b = roots
    aid, bid = ids

    def roster(root, key):
        return read(root / "roster.json").get(key, [])

    def visible(root, key, peer_id):
        return next((r for r in roster(root, key) if r["id"] == peer_id), {})

    def pending_or_failed(root, mid, target):
        return states(root, mid).get(target) in ("pending", "failed", "uncertain")

    def retry(mid, text):
        response = a / "reply.json"
        response.unlink(missing_ok=True)
        put(a / "retry.json", {"to": bid, "id": mid, "text": text})
        return until("direct send result", lambda: read(response), children)

    until("untrusted discovery", lambda: visible(a, "candidates", bid) and visible(b, "candidates", aid), children)
    assert not visible(a, "agents", bid).get("trusted")
    trust(a, bid, "bravo", pins[1])
    trust(b, aid, "alpha", pins[0])
    until("bilateral trust", lambda: visible(a, "agents", bid).get("online") and visible(b, "agents", aid).get("online"), children)
    time.sleep(2)
    assert board.connections == 0, ("idle HTTP", board.paths)

    mid = queue(a, [bid])
    inbox = b / "inbox" / (mid + ".json")
    received = until("peer inbox", lambda: read(inbox), children)
    assert received["id"] == mid and received["sender_id"] == aid and received["text"] == TEXT
    assert received["sender_fingerprint"] == pins[0]
    until("peer receipt", lambda: states(a, mid).get(bid) == "delivered", children)
    seen = b / "seen" / inbox.name
    inbox.rename(seen)
    original = seen.read_bytes()
    assert retry(mid, TEXT).get("receipt") == mid
    assert seen.read_bytes() == original and not inbox.exists()
    assert "error" in retry(mid, "different body") and seen.read_bytes() == original
    blocked = str(uuid.uuid4())
    destination = b / "inbox" / (blocked + ".json")
    destination.mkdir()
    try:
        assert "error" in retry(blocked, TEXT), "acknowledged a failed mailbox write"
    finally:
        destination.rmdir()
    assert retry(blocked, TEXT).get("receipt") == blocked
    assert read(destination)["text"] == TEXT
    reply = queue(b, [aid])
    until("reverse peer receipt", lambda: states(b, reply).get(aid) == "delivered", children)
    assert read(a / "inbox" / (reply + ".json"))["sender_id"] == bid
    assert board.connections == 0, ("peer-only HTTP", board.paths)

    self_id = queue(a, [aid])
    until("self rejection", lambda: (a / "failed" / (self_id + ".json")).exists(), children)
    assert not (a / "inbox" / (self_id + ".json")).exists() and board.connections == 0
    board.hold_next = True
    mixed = queue(a, ["board", bid])
    until("peer delivered with board offline", lambda: states(a, mixed).get(bid) == "delivered", children)
    until("board request in flight", board.waiting.is_set, children)
    try:
        incoming = queue(b, [aid])
        until("incoming delivery during stalled board HTTP",
              lambda: read(a / "inbox" / (incoming + ".json")), children, seconds=2.1)
    finally:
        board.release.set()
    assert pending_or_failed(a, mixed, "board")
    assert read(b / "inbox" / (mixed + ".json"))["text"] == TEXT

    # A receiver with a different independently provisioned identity must not
    # yield a receipt or persist the message under the old trust binding.
    trust(b, aid, "alpha", "0" * 64 if pins[0] != "0" * 64 else "1" * 64)
    until("receiver rejects old fingerprint", lambda: not visible(b, "agents", aid).get("online"), children)
    rejected = str(uuid.uuid4())
    assert "error" in retry(rejected, TEXT)
    assert not (b / "inbox" / (rejected + ".json")).exists()
    board.available = True
    reverse = queue(a, [bid, "board"])
    until("board delivered with peer rejected", lambda: states(a, reverse).get("board") == "delivered", children)
    assert pending_or_failed(a, reverse, bid)
    assert board.accepted.count((reverse, TEXT)) == 1
    assert not (b / "inbox" / (reverse + ".json")).exists()
    # Switch the sender from the wire probe into its normal production
    # transport runspace too, and verify the session-to-session path.
    trust(b, aid, "alpha", pins[0])
    (a / "use-session").touch()
    until("sender production session", lambda: (a / "worker.lock").exists(), children)
    session_id = queue(a, [bid])
    until("session-to-session receipt", lambda: states(a, session_id).get(bid) == "delivered", children)
    assert read(b / "inbox" / (session_id + ".json"))["text"] == TEXT
    until("earlier board retry settled", lambda: states(a, mixed).get("board") == "delivered", children)
    board.waiting.clear()
    board.release.clear()
    board.available, board.hold_next = False, True
    queue(a, ["board"])
    until("production board worker waiting", board.waiting.is_set, children)
    try:
        independent = queue(a, [bid])
        until("outgoing peer delivery during stalled board HTTP",
              lambda: read(b / "inbox" / (independent + ".json")), children, seconds=2.1)
    finally:
        board.release.set()
    print("Two Windows processes passed loopback-only peer chat, deduplication, trust and independent target checks.")


def main():
    if os.name != "nt":
        raise SystemExit("Windows PowerShell 5.1 required")
    with tempfile.TemporaryDirectory(prefix="peer-chat-process-") as temp, Board(("127.0.0.1", 0), Request) as board:
        base = Path(temp)
        board.connections, board.paths, board.accepted, board.keys, board.available = 0, [], [], {}, False
        board.hold_next, board.waiting, board.release = False, threading.Event(), threading.Event()
        worker = threading.Thread(target=board.serve_forever, daemon=True)
        worker.start()
        children = []
        with contextlib.ExitStack() as logs:
            try:
                verify(base, board, children, logs)
            except Exception:
                for log in base.glob("*.log"):
                    print(log.name, log.read_bytes()[-2400:].decode(errors="replace"))
                for root in (base / "alpha", base / "bravo"):
                    for pattern in ("roster.json", "deliveries/*.json", "failed/*.json", "outbox/*.json"):
                        for path in root.glob(pattern):
                            print(path.relative_to(base), read(path))
                raise
            finally:
                for name in ("alpha", "bravo"):
                    if (base / name).is_dir():
                        (base / name / "stop").touch()
                for child in children:
                    try:
                        child.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait()
                board.shutdown()
                worker.join()
        for child in children:
            if child.returncode != 0:
                raise RuntimeError(f"Agent shutdown failed with exit code {child.returncode}")


if __name__ == "__main__":
    main()
