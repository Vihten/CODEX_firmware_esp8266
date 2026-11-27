"""Loopback integration of the Python hub and the Windows PowerShell 5.1 client."""

import hashlib
import http.client
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import unittest
import uuid
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs

from hub_service import Hub


CLIENT_SCRIPT = Path(__file__).resolve().parents[1] / "codex-lan-agent" / "agent_update.ps1"
POWERSHELL = Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32/WindowsPowerShell/v1.0/powershell.exe"
LOOPBACK = "127.0.0.1"


def put(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def request(port, path, body=None):
    conn = http.client.HTTPConnection(LOOPBACK, port, timeout=8)
    try:
        headers = {"Content-Type": "application/x-www-form-urlencoded"} if body is not None else {}
        conn.request("GET" if body is None else "POST", path, body, headers)
        reply = conn.getresponse()
        return reply.status, reply.read(524289)
    finally:
        conn.close()


class Capture(BaseHTTPRequestHandler):
    """Forward only to the ephemeral loopback server. Keep wire samples in memory."""

    def log_message(self, *args):
        pass

    def do_POST(self):
        self.server.paths.append(self.path)
        body = self.rfile.read(int(self.headers["Content-Length"]))
        if self.path == "/agent-update-poll" and self.server.reject_next_ack:
            fields = parse_qs(body.decode("ascii"))
            if "ack" in fields:
                self.server.reject_next_ack = False
                body = body.replace(b"result=ok", b"result=failed")
        status, data = request(self.server.upstream, self.path, body)
        if self.path == "/agent-update-poll":
            self.server.poll_statuses.append(status)
        if self.path == "/agent-update-poll" and status == 200:
            if self.server.lose_next_ack_reply and "ack" in parse_qs(body.decode("ascii")):
                self.server.lose_next_ack_reply = False
                self.reply(503, b"synthetic response loss")
                return
            if self.server.stale_response is not None:
                data = self.server.stale_response
                self.server.stale_response = None
            self.server.polls.append((body, data))
        self.reply(status, data)

    def do_GET(self):
        self.reply(*request(self.server.upstream, self.path))

    def reply(self, status, data):
        self.send_response(status)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


@unittest.skipUnless(os.name == "nt", "requires Windows PowerShell 5.1")
class HubIntegration(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="hub-integration-")
        self.addCleanup(tmp.cleanup)
        self.base = Path(tmp.name)
        self.local_app_data = self.base / "local"
        self.root = self.local_app_data / "LED Forge" / "Codex LAN Agent"
        self.root.mkdir(parents=True)
        (self.root / "codex_lan_agent.ps1").write_text("# fixture\n", encoding="ascii")
        self.stage = self.base / "stage"
        self.release = self.base / "fixture-release"
        self.release.mkdir()
        self.machine = "HUBTEST-" + uuid.uuid4().hex.upper()
        self.user = "SyntheticUser"
        self.client = hashlib.sha256((self.machine + "-" + self.user).encode()).hexdigest()[:8].upper()
        self.metadata = {"Name": "Before", "ColorHex6": "AB1200", "other": {"retained": True}}
        put(self.root / "agent-metadata.json", self.metadata)
        (self.root / "agent-version.txt").write_text("1", encoding="ascii")

    def ps(self, code, *, success=True):
        env = {k: v for k, v in os.environ.items() if k.upper() != "PSMODULEPATH"}
        env.update(COMPUTERNAME=self.machine, USERNAME=self.user, HUB_TEST_ROOT=str(self.root),
                   HUB_TEST_STAGE=str(self.stage), HUB_TEST_SCRIPT=str(CLIENT_SCRIPT),
                   HUB_TEST_RELEASE=str(self.release), HUB_TEST_LOCAL=str(self.local_app_data))
        setup = """
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
if ($PSVersionTable.PSVersion.Major -ne 5) { throw 'Windows PowerShell 5.1 required' }
. $env:HUB_TEST_SCRIPT
$env:LOCALAPPDATA = $env:HUB_TEST_LOCAL
$root = $env:HUB_TEST_ROOT
$stage = $env:HUB_TEST_STAGE
"""
        proc = subprocess.run([str(POWERSHELL), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                               "-Command", setup + code],
                              env=env, cwd=self.base, capture_output=True, encoding="utf-8",
                              errors="replace", timeout=45, creationflags=subprocess.CREATE_NO_WINDOW)
        if success:
            self.assertEqual(proc.returncode, 0, proc.stderr)
        else:
            self.assertNotEqual(proc.returncode, 0, "client accepted an invalid response")
        return proc.stdout.strip()

    def poll(self, success=True):
        return self.ps("Save-AgentRemoteUpdate $root $stage", success=success)

    def start_hub(self, hub_class=Hub):
        self.hub = hub_class(self.base / "hub", LOOPBACK, port=0, release_dir=self.release)
        self.hub.start()
        self.addCleanup(self.hub.stop)
        self.capture = ThreadingHTTPServer((LOOPBACK, 0), Capture)
        self.capture.upstream = self.hub.port
        self.capture.polls = []
        self.capture.poll_statuses = []
        self.capture.paths = []
        self.capture.reject_next_ack = False
        self.capture.lose_next_ack_reply = False
        self.capture.stale_response = None
        thread = threading.Thread(target=self.capture.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join)
        self.addCleanup(self.capture.server_close)
        self.addCleanup(self.capture.shutdown)
        invite = self.hub.invite(self.client, "Before")
        # The capture port and every upstream destination are loopback.
        invite["HttpPort"] = self.capture.server_port
        return invite

    def row(self):
        return next(row for row in self.hub.agents() if row["id"] == self.client)

    def bind_client(self, invite):
        put(self.base / "invitation.json", invite)
        self.ps("& (Join-Path (Split-Path $env:HUB_TEST_SCRIPT) 'import_agent_hub.ps1') -InvitationPath (Join-Path (Get-Location) 'invitation.json') -Replace")

    def action(self):
        result = self.ps("ConvertTo-Json -Compress -InputObject (Read-AgentActionState $root)")
        return json.loads(result) if result else None

    def test_authenticated_rename_uninstall_and_replay(self):
        invite = self.start_hub()
        wrong = ("1" if invite["secret"][0] == "0" else "0") + invite["secret"][1:]
        self.bind_client(dict(invite, secret=wrong))
        self.poll(success=False)
        self.assertEqual(self.capture.poll_statuses[-1], 403)
        self.assertEqual(self.row()["last_seen"], 0)
        self.assertEqual(self.row()["version"], 0)
        self.assertNotIn("/agent-update-enroll", self.capture.paths)
        self.bind_client(invite)
        self.assertEqual(self.poll(), "False")
        self.assertEqual(self.row()["version"], 1)
        self.assertGreater(self.row()["last_seen"], 0)
        self.assertNotIn("secret", self.row())

        old_body, old_reply = self.capture.polls[-1]
        status, _ = request(self.hub.port, "/agent-update-poll", old_body)
        self.assertEqual(status, 403)
        name = "Тестовий ПК"
        rename = self.hub.queue(self.client, "rename", name)
        self.capture.stale_response = old_reply
        self.poll(success=False)
        self.assertIsNone(self.action())
        self.assertEqual(self.poll(), "False")
        self.assertEqual(self.action()["action_id"], rename)
        self.assertEqual(self.row()["name"], "Before")
        # The real rename handler can operate with no worker in this temporary root.
        outcome = self.ps("""
$s = Read-AgentActionState $root
if ($s.action -cne 'rename' -or $s.result -cne 'pending') { throw 'Expected only a rename' }
Invoke-AgentPendingAction $root $null (Join-Path $root 'worker')
""")
        self.assertEqual(outcome, "restart")
        self.assertEqual(read(self.root / "agent-metadata.json"), dict(self.metadata, Name=name))
        self.capture.reject_next_ack = True
        self.poll(success=False)
        self.assertEqual(self.capture.poll_statuses[-1], 403)
        self.assertFalse(self.action()["confirmed"])
        self.assertEqual(self.row()["name"], "Before")
        self.assertEqual(self.poll(), "False")
        self.assertTrue(self.action()["confirmed"])
        self.assertEqual(self.row()["name"], name)

        uninstall = self.hub.queue(self.client, "uninstall")
        self.assertNotEqual(rename, uninstall)
        status, _ = request(self.hub.port, "/agent-update-poll", self.capture.polls[-1][0])
        self.assertEqual(status, 403)
        self.assertEqual(self.poll(), "False")
        action = self.action()
        self.assertEqual((action["action_id"], action["action"], action["result"]),
                         (uninstall, "uninstall", "pending"))
        self.assertFalse(action["confirmed"])
        self.assertNotEqual(self.row()["status"], "Uninstall acknowledged")
        # Record only the fixture's completion. Save-AgentRemoteUpdate builds,
        # signs and submits the real ACK. No uninstall handler is invoked.
        self.capture.lose_next_ack_reply = True
        poll_count = len(self.capture.poll_statuses)
        self.ps("""
$s = Read-AgentActionState $root
if ($s.action -cne 'uninstall' -or $s.result -cne 'pending') { throw 'Expected pending uninstall' }
$context = Get-AgentRemovalContext $root $s
$helper = [IO.File]::ReadAllText($env:HUB_TEST_SCRIPT)
$s.result = 'ok'
Write-AgentActionState $root $s
# Allow the production 15-second retry interval after the first reply is lost.
Wait-AgentUninstallAck $root $context $helper -TimeoutMilliseconds 30000
""")
        self.assertFalse(self.capture.lose_next_ack_reply)
        self.assertEqual(self.capture.poll_statuses[poll_count:], [200, 200])
        self.assertTrue(self.action()["confirmed"])
        self.assertEqual(self.row()["status"], "Uninstall acknowledged")
        self.assertTrue((self.root / "agent-version.txt").is_file())
        self.assertFalse(self.stage.exists())

    def make_signed_fixture(self):
        names = json.loads(self.ps("ConvertTo-Json -InputObject @(Get-AgentCodeFiles)"))
        with zipfile.ZipFile(self.release / "package.zip", "w", zipfile.ZIP_DEFLATED) as package:
            for name in names:
                if name == "agent-version.txt":
                    package.writestr(name, "2")
                else:
                    (self.root / name).write_text("# original fixture file\n", encoding="ascii")
                    package.writestr(name, "# inert integration fixture\n")
        self.ps("""
$rsa = [Security.Cryptography.RSACryptoServiceProvider]::new(2048)
$rsa.PersistKeyInCsp = $false
try {
    [IO.File]::WriteAllText((Join-Path $root 'agent-update-public.xml'),$rsa.ToXmlString($false))
    $zip = Get-Item -LiteralPath (Join-Path $env:HUB_TEST_RELEASE 'package.zip')
    $hash = (Get-FileHash -LiteralPath $zip.FullName).Hash.ToLowerInvariant()
    $signed = [Text.Encoding]::UTF8.GetBytes(("LFAU1","2",$zip.Length,$hash,"" -join [char]10))
    $manifest = @{schema=1;version=2;size=$zip.Length;sha256=$hash;signature=[Convert]::ToBase64String($rsa.SignData($signed,'SHA256'))}
    [IO.File]::WriteAllText((Join-Path $env:HUB_TEST_RELEASE 'manifest.json'),($manifest | ConvertTo-Json -Compress))
} finally { $rsa.Dispose() }
""")

    def test_signed_update_stages_and_reports_version(self):
        self.make_signed_fixture()
        service_copy = self.base / "service"
        service_copy.mkdir()
        shutil.copyfile(Path(__file__).with_name("hub_service.py"), service_copy / "hub_service.py")
        shutil.copyfile(self.root / "agent-update-public.xml", service_copy / "agent-update-public.xml")
        spec = importlib.util.spec_from_file_location("fixture_hub_service", service_copy / "hub_service.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.bind_client(self.start_hub(module.Hub))
        self.assertEqual(self.poll(), "False")
        original = (self.release / "package.zip").read_bytes()
        manifest = (self.release / "manifest.json").read_bytes()
        (self.release / "package.zip").write_bytes(original + b"broken")
        with self.assertRaises(ValueError):
            self.hub.queue(self.client, "update")
        (self.release / "package.zip").write_bytes(original)
        self.hub.queue(self.client, "update")
        # A later file replacement must not change the admitted package.
        (self.release / "package.zip").write_bytes(b"replaced after admission")
        self.assertEqual(self.poll(), "True")
        self.assertEqual((self.stage / "package.zip").read_bytes(), original)
        self.assertEqual(read(self.stage / "manifest.json"), json.loads(manifest))
        self.assertEqual(self.row()["version"], 1)
        self.ps("Install-AgentStagedUpdate $root $stage; Complete-AgentUpdate $root")
        self.assertEqual((self.root / "agent-version.txt").read_text(), "2")
        self.assertEqual(self.poll(), "False")
        self.assertEqual(self.row()["version"], 2)
        self.assertEqual(self.row()["status"], "Update confirmed")
        self.assertEqual(read(self.root / "agent-metadata.json"), self.metadata)


if __name__ == "__main__":
    unittest.main()
