"""Isolated service tests. All sockets use loopback and all state is temporary."""
from contextlib import ExitStack
from hashlib import sha256
import hmac
import http.client
import io
import json
from pathlib import Path
import secrets
import socket
import tempfile
import time
import unittest
from unittest.mock import patch
from urllib.parse import urlencode
import zipfile

import hub_service as service


class ServiceTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.hub = service.Hub(self.root, "127.0.0.1", 0, self.root / "release")
        self.hub.start()
        self.addCleanup(self.hub.stop)
        self.a = self.hub.invite("AABBCCDD", "One")
        self.b = self.hub.invite("01020304", "Two")

    def request(self, path, form=None, *, method=None):
        conn = http.client.HTTPConnection(self.hub.host, self.hub.port, timeout=6)
        try:
            body = urlencode(form) if form is not None else None
            conn.request(method or ("POST" if body is not None else "GET"), path, body,
                         {"Content-Type": "application/x-www-form-urlencoded"})
            response = conn.getresponse()
            return response.status, response.read()
        finally:
            conn.close()

    def challenge(self, invite):
        status, data = self.request(service.CHALLENGE, dict(account=service.ACCOUNT, client=invite["client"]))
        self.assertEqual(status, 200)
        return json.loads(data)["nonce"]

    def poll(self, invite, version=1, failed=0, ack=0, result="ok", *, secret=None, manage=True):
        nonce = self.challenge(invite) + secrets.token_hex(16)
        fields = dict(account=service.ACCOUNT, nonce=nonce, version=str(version), client=invite["client"])
        content = f"LFAU1-POLL\n{invite['Device']}\n{invite['client']}\n{nonce}\n{version}\n"
        if failed:
            fields["failed"] = str(failed)
            content += f"FAILED\n{failed}\n"
        if manage:
            fields["manage"] = "1"
            content += "MANAGE\n1\n"
        if ack:
            fields.update(ack=str(ack), result=result)
            content += f"ACK\n{ack}\n{result}\n"
        key = bytes.fromhex(secret or invite["secret"])
        fields["mac"] = hmac.digest(key, content.encode(), "sha256").hex()
        status, data = self.request(service.POLL, fields)
        if secret:
            self.assertEqual(status, 403)
            return
        self.assertEqual(status, 200)
        response = json.loads(data)
        signed = f"LFAU1\n{invite['Device']}\n{invite['client']}\n{nonce}\n{response['version']}\n"
        if "action" in response:
            signed += f"ACTION\n{response['action_id']}\n{response['action']}\n{response['name_hex']}\n"
        self.assertEqual(response["mac"], hmac.digest(key, signed.encode(), "sha256").hex())
        self.assertEqual(self.request(service.POLL, fields)[0], 403)
        return response

    def row(self):
        return next(r for r in self.hub.agents() if r["id"] == self.a["client"])

    def test_manual_invitation_survives_restart(self):
        self.assertNotEqual(self.a["secret"], self.b["secret"])
        self.assertEqual(self.hub.invite(self.a["client"], "One"), self.a)
        with self.assertRaises(ValueError):
            self.hub.invite(self.a["client"], "Different")
        for path in ["/agent-update-enroll", "/agent-uninstall", "/agent-rename", "/agent-upgrade", "/invite"]:
            self.assertEqual(self.request(path, {})[0], 404)
        self.assertEqual(self.request(service.CHALLENGE, dict(client="99999999", account=service.ACCOUNT))[0], 403)
        self.assertEqual(set(self.row()), {"id", "name", "status", "last_seen", "version"})
        self.poll(self.a, secret=self.b["secret"])
        self.assertEqual(self.row()["last_seen"], 0)
        self.hub.stop()
        restored = service.Hub(self.root, self.hub.host, self.hub.port)
        self.assertEqual(restored.export_invite(self.a["client"]), self.a)
        self.assertEqual(restored.export_invitation(self.b["client"]), self.b)
        if service.os.name == "nt":
            self.assertNotIn(bytes.fromhex(self.a["secret"]), (self.root / "hub.sqlite3").read_bytes())

    def test_jobs_require_matching_authenticated_ack(self):
        client = self.a["client"]
        rename = self.hub.queue(client, "rename", "New")
        with self.assertRaises(ValueError):
            self.hub.queue(client, "uninstall")
        self.assertNotIn("action", self.poll(self.a, manage=False))
        self.poll(self.a, ack=rename)
        self.assertEqual(self.row()["name"], "One")
        self.assertEqual(self.poll(self.a)["action_id"], rename)
        self.poll(self.b, ack=rename)
        self.poll(self.a, ack=rename + 1)
        self.assertEqual(self.row()["name"], "One")
        self.poll(self.a, ack=rename)
        self.assertEqual(self.row()["name"], "New")
        remove = self.hub.queue(client, "uninstall")
        self.assertEqual(self.poll(self.a)["action"], "uninstall")
        self.poll(self.a, ack=remove, result="failed")
        self.assertEqual(self.row()["status"], "Uninstall failed")
        second = self.hub.queue(client, "uninstall")
        self.poll(self.a, ack=remove)
        self.assertNotEqual(self.row()["status"], "Uninstall acknowledged")
        self.poll(self.a, ack=second)
        self.assertEqual(self.row()["status"], "Uninstall acknowledged")
        self.assertEqual(self.poll(self.a, ack=second)["version"], 0)

    def test_offline_and_offered_jobs_expire(self):
        self.hub.queue(self.a["client"], "rename", "Unused")
        remove = self.hub.queue(self.b["client"], "uninstall")
        self.poll(self.b)
        with patch.object(service.time, "time", return_value=time.time() + service.JOB_TTL + 5):
            self.assertNotIn("action", self.poll(self.a))
            self.assertNotIn("action", self.poll(self.b, ack=remove))
            self.assertEqual({a["status"] for a in self.hub.agents()}, {"rename expired", "uninstall expired"})
        self.assertEqual(self.row()["name"], "One")

    def test_update_offers_and_artifact_grants(self):
        release = (17, b"fixture manifest", b"fixture package")
        path = "/agent-update/package.zip"
        with patch.object(service, "release_snapshot", return_value=release):
            self.hub.queue(self.a["client"], "update")
            self.assertEqual(self.request(path)[0], 403)
            self.assertEqual(self.poll(self.b)["version"], 0)
            self.assertEqual(self.poll(self.a)["version"], 17)
            self.assertEqual(self.request(path), (200, release[2]))
            self.assertEqual(self.poll(self.a, failed=12)["version"], 17)
            self.assertEqual(self.poll(self.a, failed=17)["version"], 0)
            self.assertEqual(self.row()["status"], "Update failed")
            self.assertEqual(self.request(path)[0], 403)
            with self.assertRaises(ValueError):
                self.hub.queue(self.a["client"], "update")
            self.hub.queue(self.b["client"], "update")
            self.poll(self.b)
            self.poll(self.b, version=17)
            self.assertEqual(next(r for r in self.hub.agents() if r["id"] == self.b["client"])["status"],
                             "Update confirmed")
            with self.assertRaises(ValueError):
                self.hub.queue(self.b["client"], "update")
        with patch.object(service, "release_snapshot", return_value=(16, b"x", b"y")):
            self.poll(self.a)
            with self.assertRaisesRegex(ValueError, "rollback"):
                self.hub.queue(self.a["client"], "update")

    def test_bounds_and_lifecycle(self):
        for host in ["0.0.0.0", "8.8.8.8", "localhost", "::1"]:
            with self.assertRaises(ValueError):
                service.Hub(self.root, host)
        with self.assertRaises(ValueError):
            self.hub.invite("ABCDEF01", "é" * 25)
        self.assertEqual(self.request(service.CHALLENGE, {"client": "x" * 2000})[0], 400)
        with ExitStack() as stack:
            for _ in range(8):
                stack.enter_context(socket.create_connection((self.hub.host, self.hub.port), timeout=2))
            extra = stack.enter_context(socket.create_connection((self.hub.host, self.hub.port), timeout=2))
            self.assertEqual(extra.recv(1), b"")
            self.hub.stop()
        self.hub.start()
        self.assertEqual(self.poll(self.a)["version"], 0)

    def test_absolute_connection_deadline(self):
        # Incomplete bounded request whose header bytes keep the idle timer alive.
        with socket.create_connection((self.hub.host, self.hub.port), timeout=2) as conn:
            conn.sendall(b"GET / HTTP/1.1\r\nX-Fixture: ")
            start = time.monotonic()
            conn.settimeout(0.8)
            while time.monotonic() - start < 7:
                try:
                    data = conn.recv(256)
                except socket.timeout:
                    conn.sendall(b"x")
                    continue
                except ConnectionError:
                    break
                self.assertFalse(data.startswith(b"HTTP/1.1 200"))
                break
            else:
                self.fail("Incomplete HTTP request exceeded absolute deadline")
        self.assertEqual(self.poll(self.a)["version"], 0)

    def test_archive_checks(self):
        def make(files):
            output = io.BytesIO()
            with zipfile.ZipFile(output, "w") as archive:
                for name, value in files.items():
                    archive.writestr(name, value)
            return output.getvalue()
        files = {name: b"# fixture" for name in service.CODE_FILES}
        files["agent-version.txt"] = b"3"
        good = make(files)
        service.validate_archive(good, 3)
        with self.assertRaises(ValueError):
            service.validate_archive(good, 2)
        with self.assertRaises(ValueError):
            service.validate_archive(make(dict(files, unexpected=b"data")), 3)
        self.root.joinpath("package.zip").write_bytes(good)
        m = dict(schema=1, version=3, size=len(good), sha256=sha256(good).hexdigest(), signature="AAAA")
        self.root.joinpath("manifest.json").write_text(json.dumps(m))
        with self.assertRaises(ValueError):
            service.verify_release(self.root)


if __name__ == "__main__":
    unittest.main()
