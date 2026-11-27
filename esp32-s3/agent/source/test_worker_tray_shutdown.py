"""Run the PS5.1 worker against a stalled fixture tray and an isolated mailbox."""
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import unittest
import uuid


@unittest.skipUnless(os.name == "nt", "requires Windows PowerShell")
class WorkerShutdown(unittest.TestCase):
    def test_stalled_tray_exits_after_worker_cleanup(self):
        source = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory(prefix="worker-stop-") as tmp:
            root = Path(tmp)
            for name in ("codex_lan_agent.ps1", "agent_metadata.ps1", "agent_update.ps1",
                         "agent_chat_ipc.ps1", "weekly_tokens.ps1"):
                shutil.copyfile(source / name, root / name)
            # No certificate store, LAN discovery or live user data. The real transport
            # and board runspaces still start and stop via the shared event.
            (root / "agent_chat_peer.ps1").write_text(
                "function Start-ChatPeerRuntime { throw 'fixture offline' }\n")
            (root / "agent_chat_ui.ps1").write_text(
                "param($MailboxRoot,$ChatStopEvent)\n"
                "[IO.File]::WriteAllText((Join-Path $MailboxRoot 'entered'),'1')\n"
                "[Threading.Thread]::Sleep(60000)\n")
            sessions = root / "sessions"
            sessions.mkdir()
            mailbox = root / "CodexLanAgent/chat"
            env = dict(os.environ, LOCALAPPDATA=tmp, USERPROFILE=tmp, COMPUTERNAME="Fixture", USERNAME="Fixture")
            env.pop("PSMODULEPATH", None)
            exe = Path(env["SYSTEMROOT"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                probe.bind(("127.0.0.1", 0))
                port = str(probe.getsockname()[1])
            process = subprocess.Popen([
                str(exe), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                "-File", str(root / "codex_lan_agent.ps1"), "-InstanceName", uuid.uuid4().hex,
                "-EnableChat", "-DisableLimitPolling", "-UpdateControlPath", str(root / "control"),
                "-DirectHost", "127.0.0.1", "-DirectPort", port, "-DirectHttpPort", "1",
                "-CollectorHost", "127.0.0.1", "-Port", port, "-SessionsRoot", str(sessions),
                "-ClaudeRoot", str(root / "missing"), "-PollMilliseconds", "250",
            ], env=env, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                deadline = time.monotonic() + 15
                while not ((mailbox / "entered").is_file() and (root / "control.health").is_file()):
                    self.assertIsNone(process.poll(), "Fixture worker exited before tray startup")
                    self.assertLess(time.monotonic(), deadline, "Fixture tray failed to start")
                    time.sleep(0.05)
                (root / "control.stop").write_bytes(b"1")
                output, _ = process.communicate(timeout=15)
                self.assertEqual(process.returncode, 0, output)
                self.assertIn(b"Chat tray missed shutdown deadline", output)
                self.assertTrue((mailbox / "worker.lock").exists())
                (mailbox / "worker.lock").unlink()  # No worker handle survives process exit.
            finally:
                if process.poll() is None:
                    process.kill()
                process.communicate()


if __name__ == "__main__":
    unittest.main()
