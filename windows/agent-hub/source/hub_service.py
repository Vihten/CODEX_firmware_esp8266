"""Local operator API and pull-only LF agent update service."""
import base64
from contextlib import closing, contextmanager
import ctypes
from hashlib import sha256
import hmac
import io
import zipfile
import zlib
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import socket
import socketserver
import sqlite3
import threading
import time
import unicodedata
from urllib.parse import parse_qsl
import xml.etree.ElementTree as ET

ACCOUNT = "53197668"
MAX_NUMBER = 2**53 - 1
MAX_CLIENTS = 128
JOB_TTL = 600
MODULE_DIR = Path(__file__).resolve().parent
LAN = tuple(map(ipaddress.IPv4Network, ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")))
CHALLENGE = "/agent-update-challenge"
POLL = "/agent-update-poll"
ARTIFACTS = {"/agent-update/manifest.json": "manifest", "/agent-update/package.zip": "package"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def client_id(value):
    require(isinstance(value, str) and re.fullmatch("[0-9A-Fa-f]{8}", value), "Invalid client ID")
    return value.upper()


def label(value):
    require(isinstance(value, str) and value.strip() and len(value) <= 48 and
            len(value.encode("utf8")) <= 48 and all(unicodedata.category(c) != "Cc" for c in value),
            "Name must contain 1-48 UTF-8 bytes without controls")
    return value


def number(value, minimum=1):
    require(isinstance(value, str) and re.fullmatch(r"0|[1-9][0-9]{0,15}", value), "Invalid integer")
    n = int(value)
    require(minimum <= n <= MAX_NUMBER, "Integer out of range")
    return n


def protected(data, decrypt=False):
    # Outside Windows the test database relies on its private filesystem permissions.
    if os.name != "nt":
        return data

    class Blob(ctypes.Structure):
        _fields_ = [("size", ctypes.c_uint32), ("data", ctypes.c_void_p)]

    buf = ctypes.create_string_buffer(data)
    src = Blob(len(data), ctypes.addressof(buf))
    dst = Blob()
    api = ctypes.windll.crypt32
    fn = api.CryptUnprotectData if decrypt else api.CryptProtectData
    if not fn(ctypes.byref(src), None, None, None, None, 1, ctypes.byref(dst)):
        raise OSError("Windows could not access the invitation secret")
    try:
        return ctypes.string_at(dst.data, dst.size)
    finally:
        ctypes.windll.kernel32.LocalFree(ctypes.c_void_p(dst.data))


def bounded_file(path, limit):
    try:
        require(path.is_file(), f"Missing {path.name}")
        with path.open("rb") as stream:
            data = stream.read(limit + 1)
    except OSError as exc:
        raise ValueError(f"Cannot read {path.name}") from exc
    require(0 < len(data) <= limit, f"Invalid size for {path.name}")
    return data


def unique(pairs):
    result = dict(pairs)
    require(len(result) == len(pairs), "Duplicate field")
    return result


def release_snapshot(directory):
    manifest = bounded_file(directory / "manifest.json", 8192)
    package = bounded_file(directory / "package.zip", 524288)
    try:
        m = json.loads(manifest.decode("utf-8-sig"), object_pairs_hook=unique)
    except RecursionError as exc:
        raise ValueError("Manifest nesting limit exceeded") from exc
    require(isinstance(m, dict) and set(m) == {"schema", "version", "size", "sha256", "signature"},
            "Invalid manifest fields")
    require(type(m["schema"]) is int and m["schema"] == 1 and type(m["version"]) is int and
            0 < m["version"] <= MAX_NUMBER and type(m["size"]) is int and m["size"] == len(package),
            "Invalid manifest version or size")
    digest = sha256(package).hexdigest()
    require(m["sha256"] == digest and isinstance(m["signature"], str), "Invalid package digest or signature")
    xml = bounded_file(MODULE_DIR / "agent-update-public.xml", 8192).decode("utf-8-sig")
    require("<!" not in xml, "Invalid publisher key")
    try:
        key = ET.fromstring(xml)
    except ET.ParseError as exc:
        raise ValueError("Invalid publisher key") from exc
    require(key.tag == "RSAKeyValue" and sorted(x.tag for x in key) == ["Exponent", "Modulus"],
            "Expected publisher public key")
    modulus = int.from_bytes(base64.b64decode(key.findtext("Modulus"), validate=True), "big")
    exponent = int.from_bytes(base64.b64decode(key.findtext("Exponent"), validate=True), "big")
    require(2048 <= modulus.bit_length() <= 8192 and 3 <= exponent <= 0xffffffff and exponent % 2,
            "Invalid RSA key parameters")
    width = (modulus.bit_length() + 7) // 8
    sig = base64.b64decode(m["signature"], validate=True)
    s = int.from_bytes(sig, "big")
    require(len(sig) == width and s < modulus, "Invalid signature size")
    signed = f"LFAU1\n{m['version']}\n{len(package)}\n{digest}\n".encode()
    digest_info = bytes.fromhex("3031300d060960864801650304020105000420") + sha256(signed).digest()
    expected = b"\0\x01" + b"\xff" * (width - len(digest_info) - 3) + b"\0" + digest_info
    require(hmac.compare_digest(pow(s, exponent, modulus).to_bytes(width, "big"), expected),
            "Publisher signature rejected")
    validate_archive(package, m["version"])
    return m["version"], manifest, package


CODE_FILES = set("""codex_lan_agent.ps1 start_agent_supervisor.ps1 agent_metadata.ps1 weekly_tokens.ps1
    agent_chat_ipc.ps1 agent_chat_ui.ps1 agent_update.ps1 agent_update_download.ps1
    agent_update_restart.ps1 agent-version.txt""".split())


def validate_archive(package, version):
    try:
        with zipfile.ZipFile(io.BytesIO(package)) as archive:
            entries = archive.infolist()
            require(len(entries) == len(CODE_FILES) and {e.orig_filename.lower() for e in entries} == CODE_FILES,
                    "Unexpected ZIP entries")
            require(sum(e.file_size for e in entries) <= 2097152, "Expanded package too large")
            for e in entries:
                require(e.file_size > 0 and (e.external_attr >> 16 & 0xf000) in (0, 0x8000) and
                        not e.external_attr & 0x410 and not e.flag_bits & 1 and
                        e.compress_type in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED), "Invalid ZIP entry")
                version_file = e.filename.lower() == "agent-version.txt"
                if version_file:
                    require(e.file_size <= 128, "Packaged version too large")
                count = 0
                with archive.open(e) as stream:
                    while chunk := stream.read(16384):
                        count += len(chunk)
                        if version_file:
                            require(chunk.decode("utf8").strip() == str(version), "Packaged version mismatch")
                require(count == e.file_size, "ZIP entry length mismatch")
    except (zipfile.BadZipFile, EOFError, zlib.error, RuntimeError) as exc:
        raise ValueError("Invalid ZIP package") from exc


def verify_release(directory):
    """Return the version of a freshly verified, publisher-signed agent archive."""
    return release_snapshot(Path(directory))[0]


class Hub:
    def __init__(self, data_dir, host, port=4220, release_dir=None):
        addr = ipaddress.IPv4Address(host)
        require(addr.is_loopback or any(addr in net for net in LAN), "Select a private IPv4 address")
        require(type(port) is int and 0 <= port <= 65535, "Invalid port")
        self.host, self.port = str(addr), port
        self.data_dir = Path(data_dir) if data_dir is not None else Path(os.environ["LOCALAPPDATA"]) / "CodexLanAgent/Hub"
        self.data_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.release_dir = Path(release_dir) if release_dir is not None else None
        self._db = self.data_dir / "hub.sqlite3"
        os.close(os.open(self._db, os.O_CREAT | os.O_RDWR, 0o600))
        self._life = threading.Lock()
        self._server = None
        self._thread = None
        self._challenges = {}
        with self._transaction() as db:
            db.execute("CREATE TABLE IF NOT EXISTS hub(device TEXT NOT NULL, sequence INTEGER NOT NULL)")
            db.execute("INSERT INTO hub SELECT ?,0 WHERE NOT EXISTS(SELECT 1 FROM hub)", (secrets.token_hex(3).upper(),))
            self.device = db.execute("SELECT device FROM hub").fetchone()[0]
            db.execute("""CREATE TABLE IF NOT EXISTS agents(id TEXT PRIMARY KEY, name TEXT NOT NULL,
                       secret BLOB NOT NULL, version INTEGER DEFAULT 0, failed INTEGER DEFAULT 0,
                       last_seen INTEGER DEFAULT 0, status TEXT DEFAULT 'Invited')""")
            db.execute("""CREATE TABLE IF NOT EXISTS jobs(client TEXT PRIMARY KEY, id INTEGER NOT NULL,
                       action TEXT NOT NULL, name TEXT NOT NULL, target INTEGER NOT NULL, expires INTEGER NOT NULL,
                       offered INTEGER DEFAULT 0, grant_ip TEXT, grant_until INTEGER DEFAULT 0)""")
            db.execute("""CREATE TABLE IF NOT EXISTS release(singleton INTEGER PRIMARY KEY CHECK(singleton=1),
                       version INTEGER, manifest BLOB, package BLOB)""")

    @contextmanager
    def _transaction(self):
        with closing(sqlite3.connect(self._db, timeout=3)) as db:
            db.row_factory = sqlite3.Row
            with db:
                db.execute("BEGIN IMMEDIATE")
                yield db

    def start(self):
        with self._life:
            if self._server is not None:
                return
            server = _Server((self.host, self.port), self)
            try:
                thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.15}, daemon=True)
                thread.start()
            except BaseException:
                server.server_close()
                raise
            self.port = server.server_address[1]
            self._server, self._thread = server, thread

    def stop(self):
        with self._life:
            if self._server is not None:
                self._server.shutdown()
                self._server.close_connections()
                self._server.server_close()
                self._thread.join()
                self._server = self._thread = None
                with self._transaction():
                    self._challenges.clear()

    def _invite(self, row):
        require(self.port != 0, "Start the listener before exporting an ephemeral port")
        return dict(Host=self.host, HttpPort=self.port, Device=self.device, Account=ACCOUNT,
                    secret=protected(row["secret"], True).hex(), client=row["id"])

    def invite(self, client, name):
        client, name = client_id(client), label(name)
        with self._transaction() as db:
            row = db.execute("SELECT * FROM agents WHERE id=?", (client,)).fetchone()
            if row is None:
                require(db.execute("SELECT count(*) FROM agents").fetchone()[0] < MAX_CLIENTS, "Agent limit reached")
                db.execute("INSERT INTO agents(id,name,secret) VALUES(?,?,?)",
                           (client, name, protected(secrets.token_bytes(32))))
                row = db.execute("SELECT * FROM agents WHERE id=?", (client,)).fetchone()
            require(row["name"] == name, "Client already invited with another name")
            return self._invite(row)

    def export_invitation(self, client):
        with self._transaction() as db:
            row = db.execute("SELECT * FROM agents WHERE id=?", (client_id(client),)).fetchone()
            require(row is not None, "Unknown client")
            return self._invite(row)

    export_invite = export_invitation

    @staticmethod
    def _expire(db, now):
        db.execute("""UPDATE agents SET status=(SELECT action FROM jobs WHERE client=agents.id) || ' expired'
                   WHERE id IN (SELECT client FROM jobs WHERE expires<=? OR expires>?)""", (now, now + JOB_TTL))
        db.execute("DELETE FROM jobs WHERE expires<=? OR expires>?", (now, now + JOB_TTL))

    def agents(self):
        with self._transaction() as db:
            self._expire(db, int(time.time()))
            return [dict(r) for r in db.execute("SELECT id,name,version,last_seen,status FROM agents ORDER BY id")]

    def queue(self, client, action, name=""):
        client = client_id(client)
        require(action in ("rename", "update", "uninstall"), "Unknown action")
        if action == "rename":
            name = label(name)
        else:
            require(name == "", "Unexpected job name")
        target = 0
        with self._transaction() as db:
            now = int(time.time())
            self._expire(db, now)
            agent = db.execute("SELECT * FROM agents WHERE id=?", (client,)).fetchone()
            require(agent is not None, "Unknown client")
            require(not db.execute("SELECT 1 FROM jobs WHERE client=?", (client,)).fetchone(), "Job already pending")
            if action == "update":
                directory = self.release_dir
                if directory is None:
                    directory = self.data_dir / "release"
                    if not directory.exists():
                        directory = MODULE_DIR / "release"
                target, manifest, package = release_snapshot(directory)
                require(target > max(agent["version"], agent["failed"]), "Release is installed, older or failed")
                old = db.execute("SELECT version,manifest,package FROM release").fetchone()
                if old:
                    require(target >= old["version"], "Release rollback rejected")
                    if target == old["version"]:
                        require(package == old["package"], "Release version reused")
                    else:
                        require(not db.execute("SELECT 1 FROM jobs WHERE action='update'").fetchone(),
                                "Earlier update still pending")
                db.execute("INSERT OR REPLACE INTO release VALUES(1,?,?,?)", (target, manifest, package))
            db.execute("UPDATE hub SET sequence=sequence+1")
            job = db.execute("SELECT sequence FROM hub").fetchone()[0]
            require(job <= MAX_NUMBER, "Job ID limit reached")
            db.execute("INSERT INTO jobs(client,id,action,name,target,expires) VALUES(?,?,?,?,?,?)",
                       (client, job, action, name, target, now + JOB_TTL))
            db.execute("UPDATE agents SET status=? WHERE id=?", (action + " queued", client))
            return job

    def _challenge(self, form):
        require(set(form) == {"client", "account"} and form["account"] == ACCOUNT, "Invalid challenge")
        client = client_id(form["client"])
        with self._transaction() as db:
            if not db.execute("SELECT 1 FROM agents WHERE id=?", (client,)).fetchone():
                raise PermissionError()
            nonce, expires = self._challenges.get(client, ("", 0))
            if time.monotonic() >= expires:
                nonce = secrets.token_hex(16)
                self._challenges[client] = nonce, time.monotonic() + 30
            return {"device": self.device, "nonce": nonce}

    def _poll(self, form, address):
        required = {"account", "client", "version", "nonce", "mac"}
        require(required <= form.keys() <= required | {"manage", "ack", "result", "failed"} and
                form["account"] == ACCOUNT, "Invalid poll")
        client = client_id(form["client"])
        version = number(form["version"])
        nonce = form["nonce"]
        require(re.fullmatch("[0-9a-f]{64}", nonce) and re.fullmatch("[0-9a-f]{64}", form["mac"]), "Invalid poll")
        failed = number(form.get("failed", "0"), 0)
        message = f"LFAU1-POLL\n{self.device}\n{client}\n{nonce}\n{version}\n"
        if failed:
            message += f"FAILED\n{failed}\n"
        manage = "manage" in form
        if manage:
            require(form["manage"] == "1", "Invalid capability")
            message += "MANAGE\n1\n"
        ack = 0
        if "ack" in form or "result" in form:
            require(manage and "ack" in form and form.get("result") in ("ok", "failed"), "Invalid ACK")
            ack = number(form["ack"])
            message += f"ACK\n{ack}\n{form['result']}\n"

        with self._transaction() as db:
            issued, deadline = self._challenges.get(client, ("", 0))
            if issued != nonce[:32] or time.monotonic() >= deadline:
                raise PermissionError()
            row = db.execute("SELECT secret FROM agents WHERE id=?", (client,)).fetchone()
            key = protected(row["secret"], True)
            if not hmac.compare_digest(hmac.digest(key, message.encode(), "sha256").hex(), form["mac"]):
                raise PermissionError()
            del self._challenges[client]
            now = int(time.time())
            self._expire(db, now)
            db.execute("""UPDATE agents SET version=?,failed=?,last_seen=?,
                       status=CASE WHEN status='Invited' THEN 'Connected' ELSE status END WHERE id=?""",
                       (version, failed, now, client))
            job = db.execute("SELECT * FROM jobs WHERE client=?", (client,)).fetchone()
            reply = dict(device=self.device, nonce=nonce, version=0)
            action_message = ""
            if job:
                kind = job["action"]
                done = None
                if kind == "update":
                    target = job["target"]
                    if version >= target:
                        done = "Update confirmed" if job["offered"] and version == target else "Update superseded"
                    elif failed >= target:
                        done = "Update failed" if failed == target else "Update blocked by failed version"
                    else:
                        reply["version"] = target
                        db.execute("UPDATE jobs SET grant_ip=?,grant_until=? WHERE client=?",
                                   (address, min(job["expires"], now + 60), client))
                elif job["offered"] and ack == job["id"]:
                    if form["result"] == "ok":
                        done = kind.capitalize() + " acknowledged"
                        if kind == "rename":
                            db.execute("UPDATE agents SET name=? WHERE id=?", (job["name"], client))
                    else:
                        done = kind.capitalize() + " failed"
                elif manage and ack != job["id"]:
                    name_hex = job["name"].encode().hex()
                    reply.update(action=kind, action_id=job["id"], name_hex=name_hex)
                    action_message = f"ACTION\n{job['id']}\n{kind}\n{name_hex}\n"
                if done:
                    db.execute("DELETE FROM jobs WHERE client=?", (client,))
                    db.execute("UPDATE agents SET status=? WHERE id=?", (done, client))
                elif reply["version"] or action_message:
                    db.execute("UPDATE jobs SET offered=1 WHERE client=?", (client,))
                    db.execute("UPDATE agents SET status=? WHERE id=?", (kind + " offered", client))
            response = f"LFAU1\n{self.device}\n{client}\n{nonce}\n{reply['version']}\n{action_message}"
            reply["mac"] = hmac.digest(key, response.encode(), "sha256").hex()
            return reply

    def _artifact(self, path, address):
        column = ARTIFACTS[path]
        with self._transaction() as db:
            now = int(time.time())
            self._expire(db, now)
            row = db.execute(f"""SELECT {column} FROM release WHERE EXISTS
                (SELECT 1 FROM jobs WHERE action='update' AND offered AND target=release.version
                 AND expires>? AND grant_ip=? AND grant_until>?)""", (now, address, now)).fetchone()
            if not row:
                raise PermissionError()
            return row[0]


class _Server(socketserver.ThreadingMixIn, socketserver.TCPServer):
    request_queue_size = 16
    daemon_threads = False
    max_connections = 8

    def __init__(self, addr, hub):
        self.hub = hub
        self.lock = threading.Lock()
        self.active = {}
        super().__init__(addr, _Handler)

    def process_request(self, request, client_address):
        with self.lock:
            if len(self.active) >= self.max_connections:
                self.shutdown_request(request)
                return
            self.active[request] = time.monotonic() + 5
        try:
            request.settimeout(3)
            super().process_request(request, client_address)
        except BaseException:
            with self.lock:
                self.active.pop(request, None)
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            with self.lock:
                self.active.pop(request, None)

    def close_connections(self, expired_only=False):
        with self.lock:
            for sock, deadline in self.active.items():
                if not expired_only or time.monotonic() >= deadline:
                    try:
                        sock.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass

    def service_actions(self):
        self.close_connections(expired_only=True)

    def handle_error(self, request, client_address):
        # Wire responses and process logs must not disclose packets or invitations.
        pass


class _Handler(socketserver.StreamRequestHandler):
    def handle(self):
        try:
            status, content_type, body = self.dispatch()
        except PermissionError:
            status, content_type, body = "403 Forbidden", "text/plain", b"Forbidden"
        except (ValueError, KeyError):
            status, content_type, body = "400 Bad Request", "text/plain", b"Invalid request"
        except (sqlite3.Error, OSError):
            status, content_type, body = "503 Service Unavailable", "text/plain", b"Unavailable"
        headers = (f"HTTP/1.1 {status}\r\nContent-Type: {content_type}\r\nContent-Length: {len(body)}\r\n"
                   "Cache-Control: no-store\r\nConnection: close\r\n\r\n").encode("ascii")
        self.wfile.write(headers + body)

    def dispatch(self):
        line = self.rfile.readline(513)
        match = re.fullmatch(rb"(GET|POST) (/[!-~]{1,200}) HTTP/1\.[01]\r\n", line)
        require(match, "Invalid request line")
        method, path = (s.decode("ascii") for s in match.groups())
        headers = {}
        remaining = 8192
        while True:
            line = self.rfile.readline(min(remaining + 1, 2049))
            require(line.endswith(b"\r\n") and len(line) <= min(2048, remaining), "Invalid headers")
            remaining -= len(line)
            if line == b"\r\n":
                break
            field, sep, value = line[:-2].partition(b":")
            name = field.decode("ascii").lower()
            require(sep and re.fullmatch("[a-z0-9-]+", name) and name not in headers and len(headers) < 32,
                    "Invalid header")
            headers[name] = value.decode("ascii").strip()
        require("transfer-encoding" not in headers, "Invalid framing")
        hub = self.server.hub
        address = self.client_address[0]
        if method == "GET" and path in ARTIFACTS:
            require(headers.get("content-length", "0") == "0", "Unexpected body")
            mime = "application/json" if path.endswith(".json") else "application/zip"
            return "200 OK", mime, hub._artifact(path, address)
        if method != "POST" or path not in (CHALLENGE, POLL):
            return "404 Not Found", "text/plain", b"Not found"
        length = number(headers.get("content-length", ""))
        require(length <= 1024 and headers.get("content-type", "").split(";")[0].strip().lower() ==
                "application/x-www-form-urlencoded", "Invalid form")
        if "expect" in headers:
            require(headers["expect"].lower() == "100-continue", "Invalid expectation")
            self.wfile.write(b"HTTP/1.1 100 Continue\r\n\r\n")
        body = self.rfile.read(length)
        require(len(body) == length, "Incomplete form")
        form = unique(parse_qsl(body.decode("ascii"), strict_parsing=True, keep_blank_values=True, max_num_fields=10))
        result = hub._challenge(form) if path == CHALLENGE else hub._poll(form, address)
        return "200 OK", "application/json", json.dumps(result, separators=(",", ":")).encode()
