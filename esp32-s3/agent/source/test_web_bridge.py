"""Windows integration test. Run with python tools/codex-lan-agent/test_web_bridge.py."""
import base64
import hashlib
import hmac
import io
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tool"))
from build_agent_web_bridge import OUT, ROOT, VERSION, blob, build, payload
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import padding, rsa

OLD = 20261123090908
CHAT = 20261127120000
FINAL = ROOT / "build/agent-release-chat-20261127120000"
PS = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass"]
SECRET = bytes(range(32))
DEVICE = "EB2198"


class Controller(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send(self, data):
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        routes = {"/agent-update/": OUT, "/agent-update-final/": FINAL}
        for prefix, directory in routes.items():
            for name in ("manifest.json", "package.zip"):
                if self.path == prefix + name:
                    self.server.downloads.append(self.path)
                    return self.send((directory / name).read_bytes())
        self.send_error(404)

    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"])).decode("ascii")
        if self.path == "/agent":
            return self.send(f"LFA2_ACK,{DEVICE},{self.server.client}".encode())
        fields = dict(urllib.parse.parse_qsl(body))
        if self.path == "/agent-update-challenge":
            self.server.nonce = secrets.token_hex(16)
            reply = dict(device=DEVICE, nonce=self.server.nonce)
        elif self.path == "/agent-update-poll":
            nonce, client, version = fields["nonce"], fields["client"], int(fields["version"])
            message = f"LFAU1-POLL\n{DEVICE}\n{client}\n{nonce}\n{version}\nMANAGE\n1\n"
            if (not nonce.startswith(self.server.nonce) or fields["account"] != "53197668"
                    or fields["mac"] != hmac.digest(SECRET, message.encode(), "sha256").hex()):
                return self.send_error(403)
            target = {OLD: VERSION, VERSION: CHAT, CHAT: 0}[version]
            message = f"LFAU1\n{DEVICE}\n{client}\n{nonce}\n{target}\n"
            reply = dict(device=DEVICE, nonce=nonce, version=target,
                         mac=hmac.digest(SECRET, message.encode(), "sha256").hex())
            self.server.polls.append(version)
        else:
            return self.send_error(404)
        self.send(json.dumps(reply).encode())


def fixture(root):
    with zipfile.ZipFile(io.BytesIO(blob("package.zip"))) as archive:
        archive.extractall(root)
    (root / "agent-update-public.xml").write_bytes(blob("agent-update-public.xml"))


def future_fixture(directory):
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    public = key.public_key().public_numbers()
    def enc(n):
        return base64.b64encode(n.to_bytes((n.bit_length() + 7) // 8, "big")).decode()
    (directory / "agent-update-public.xml").write_text(
        f"<RSAKeyValue><Modulus>{enc(public.n)}</Modulus><Exponent>{enc(public.e)}</Exponent></RSAKeyValue>")
    with zipfile.ZipFile(FINAL / "package.zip") as src, zipfile.ZipFile(directory / "package.zip", "w", zipfile.ZIP_DEFLATED) as dst:
        for name in src.namelist():
            dst.writestr(name, str(CHAT + 1).encode() if name == "agent-version.txt" else src.read(name))
    data = (directory / "package.zip").read_bytes()
    m = dict(version=CHAT + 1, schema=1, size=len(data), sha256=hashlib.sha256(data).hexdigest())
    m["signature"] = base64.b64encode(key.sign(payload(m), padding.PKCS1v15(), hashes.SHA256())).decode()
    (directory / "manifest.json").write_text(json.dumps(m))


def web_chain(tmp):
    root = tmp / "installed"
    fixture(root)
    server = ThreadingHTTPServer(("127.0.0.1", 0), Controller)
    server.polls, server.downloads = [], []
    identity = os.environ["COMPUTERNAME"] + "-" + os.environ["USERNAME"]
    server.client = hashlib.sha256(identity.encode()).hexdigest()[:8].upper()
    port = server.server_port
    (root / "controller-endpoint.json").write_text(json.dumps(dict(
        Device=DEVICE, Account="53197668", Host="127.0.0.1", HttpPort=port)))
    (root / "agent-update-identity.json").write_text(json.dumps(dict(device=DEVICE, secret=SECRET.hex())))
    # Only redirect child launch paths into the fixture and isolate worker inputs.
    # Both supervisors, both restart helpers and all three workers execute signed bytes.
    proxy = tmp / "launch.ps1"
    proxy.write_text(r"""
function Start-Process {
    param($FilePath,$ArgumentList,$WorkingDirectory,$WindowStyle,$RedirectStandardOutput,$RedirectStandardError,[switch]$PassThru)
    if ($ArgumentList -like '*codex_lan_agent.ps1*') {
        $PSBoundParameters.ArgumentList += " -InstanceName Worker_$env:BRIDGE_TEST_ID -DisableLimitPolling"
    } else {
        foreach ($name in @('start_agent_supervisor.ps1','agent_update_restart.ps1')) {
            $PSBoundParameters.ArgumentList = $PSBoundParameters.ArgumentList.Replace((Join-Path $WorkingDirectory $name),(Join-Path $env:BRIDGE_TEST_TMP $name))
        }
    }
    Microsoft.PowerShell.Management\Start-Process @PSBoundParameters
}
""")
    for name, args in [("start_agent_supervisor.ps1", "CollectorHost Port DirectHost DirectPort DirectHttpPort LogPath InstanceName"),
                       ("agent_update_restart.ps1", "OldProcessId OldStartTicks ParametersBase64")]:
        (tmp / name).write_text("param(" + ",".join("$" + a for a in args.split()) + ")\n"
            + ". (Join-Path $PSScriptRoot 'launch.ps1')\n"
            + f"& (Join-Path $PSScriptRoot 'installed/{name}') @PSBoundParameters\n")
    env = dict(os.environ, BRIDGE_TEST_ID=secrets.token_hex(8), BRIDGE_TEST_TMP=str(tmp),
               USERPROFILE=str(tmp), LOCALAPPDATA=str(tmp), CODEX_HOME=str(tmp / ".codex"),
               CLAUDE_CONFIG_DIR=str(tmp / ".claude"))
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.bind(("127.0.0.1", 0))
        udp_port = udp.getsockname()[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    args = PS + ["-File", str(tmp / "start_agent_supervisor.ps1"), "-CollectorHost", "127.0.0.1",
                 "-DirectHost", "127.0.0.1", "-Port", str(udp_port), "-DirectPort", str(udp_port),
                 "-DirectHttpPort", str(port), "-InstanceName", "Sup_" + env["BRIDGE_TEST_ID"]]
    proc = subprocess.Popen(args, env=env, creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        until = time.monotonic() + 150
        while CHAT not in server.polls and time.monotonic() < until:
            time.sleep(.25)
        if CHAT not in server.polls:
            raise RuntimeError(f"Health/handoff timeout: polls={server.polls} downloads={server.downloads} "
                               + (root / "agent.log").read_text(encoding="utf-8-sig", errors="replace")[-1500:])
        assert server.polls.index(OLD) < server.polls.index(VERSION) < server.polls.index(CHAT)
        assert server.downloads == [p + n for p in ("/agent-update/", "/agent-update-final/") for n in ("manifest.json", "package.zip")]
        assert not (root / ".agent-update-journal.json").exists()
        with zipfile.ZipFile(FINAL / "package.zip") as archive:
            for name in archive.namelist():
                assert (root / name).read_bytes() == archive.read(name), name
        print("PASS loopback signed downloads, real worker health, two restart handoffs, final authenticated poll")
    finally:
        server.shutdown()
        server.server_close()
        # Limit cleanup to PowerShell children whose command line names this unique fixture.
        subprocess.run(PS + ["-Command", """
$needle = $env:BRIDGE_TEST_TMP + '\\'
Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
    Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -and $_.CommandLine.Contains($needle) } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
"""], env=env, check=True)
        proc.wait(timeout=10)


if __name__ == "__main__":
    # An invoking PowerShell 7 session can export incompatible module paths to 5.1.
    os.environ["PSModulePath"] = str(Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/Modules")
    build()
    pair = [(OUT / n).read_bytes() for n in ("manifest.json", "package.zip")]
    build()
    assert pair == [(OUT / n).read_bytes() for n in ("manifest.json", "package.zip")]
    with tempfile.TemporaryDirectory(prefix="web-bridge-") as tmpdir:
        tmp = Path(tmpdir)
        fixture(tmp / "old")
        future = tmp / "future"
        future.mkdir()
        future_fixture(future)
        subprocess.run(PS + ["-File", str(Path(__file__).with_name("test_web_bridge_recovery.ps1")),
                       str(tmp), str(OUT), str(FINAL)], check=True)
        web_chain(tmp)
