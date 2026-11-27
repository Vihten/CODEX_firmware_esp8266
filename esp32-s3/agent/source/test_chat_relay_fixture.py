"""Loopback-only fixture for test_agent_chat_ipc.ps1."""
import hashlib
import hmac
import json
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
import sys

root = Path(sys.argv[1])
key = bytes.fromhex((root / "fixture.key").read_text())
client = "01020304"
boot = "0123456789abcdef"
incoming = "12345678-1234-4123-8123-123456789abc"
sequence = 0


def mac(value):
    return hmac.new(key, value.encode(), hashlib.sha256).hexdigest()


class Relay(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        global sequence
        body = self.rfile.read(int(self.headers["Content-Length"])).decode()
        op = self.path.removeprefix("/chat/")
        with (root / "calls").open("a") as output:
            print(op, file=output)
        mode = (root / "mode").read_text().strip()
        current_boot = "aaaaaaaaaaaaaaaa" if mode == "reboot" else boot
        seq = self.headers.get("X-Chat-Seq", "")
        mid = self.headers.get("X-Chat-Id", "")
        to = self.headers.get("X-Chat-To", "")
        if op != "state":
            value = "\n".join(["LFCHAT1", current_boot, client, seq, self.path, mid, to, body])
            if (self.headers.get("X-Chat-Mac") != mac(value) or
                    self.headers.get("X-Chat-Client") != client or int(seq) <= sequence):
                self.send_error(403)
                return
            sequence = int(seq)
        if mode == "redirect":
            self.send_response(302)
            self.send_header("Location", "/unexpected")
            self.end_headers()
            return
        result = {"ok": True}
        if op == "state":
            result = {"boot": current_boot}
        elif op == "roster":
            result = {"self_id": client, "agents": [
                {"id": "board", "name": "Board", "online": True},
                {"id": "AABBCCDD", "name": "Fixture", "online": True}]}
        elif op == "inbox":
            result = {"messages": []}
            if mode in ("delivery", "ackfail"):
                result["messages"].append({"id": incoming, "sender_id": "AABBCCDD",
                                          "sender_name": "Fixture", "text": "hello\nworld"})
        elif op == "ack":
            if mid != incoming or not any((root / sub / (mid + ".json")).exists()
                                          for sub in ("inbox", "seen")):
                self.send_error(400)
                return
            if mode == "ackfail":
                self.send_error(503)
                return
        elif op == "send":
            if body != "test\n\u2603" or to != "board,AABBCCDD":
                self.send_error(400)
                return
            if mode == "uncertain":
                self.send_error(503)
                return
        else:
            self.send_error(404)
            return
        data = json.dumps(result, separators=(",", ":"))
        if mode == "oversize":
            data = "x" * 8193
        self.send_response(200)
        self.send_header("Content-Length", str(len(data.encode())))
        if op != "state" and mode != "unsigned":
            self.send_header("X-Chat-Reply", mac("\n".join(["LFCHAT1-REPLY", current_boot, client, seq, data])))
        self.end_headers()
        self.wfile.write(data.encode())


with HTTPServer(("127.0.0.1", 0), Relay) as server:
    (root / "port").write_text(str(server.server_port))
    server.serve_forever()
