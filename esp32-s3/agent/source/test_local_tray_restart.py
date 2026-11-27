"""Exercise the installed supervisor and tray IPC in a disposable Windows install."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import uuid

HERE = Path(__file__).resolve().parent
PS = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"


def wait(check):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(0.05)
    raise AssertionError("Timed out waiting for fixture")


def main():
    with tempfile.TemporaryDirectory(prefix="tray-restart-") as directory:
        root = Path(directory) / "LED Forge/Codex LAN Agent"
        root.mkdir(parents=True)
        for name in ("start_agent_supervisor.ps1", "agent_chat_ipc.ps1", "agent_chat_peer.ps1"):
            shutil.copyfile(HERE / name, root / name)
        (root / "agent_update.ps1").write_text("""
function Repair-AgentGuiRecoveryTask {}
function Open-AgentManagementGuard { [IO.MemoryStream]::new() }
function Test-AgentUpdateReady { $false }
function Read-AgentActionState {}
function Invoke-AgentPendingAction {}
function Write-AgentDurable($path,$bytes) { [IO.File]::WriteAllBytes($path,$bytes) }
""")
        (root / "codex_lan_agent.ps1").write_text(r"""
param([switch]$EnableChat,$CollectorHost,$Port,$DirectHost,$DirectPort,$DirectHttpPort,$LogPath,$UpdateControlPath)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/agent_chat_ipc.ps1"
$lock = [IO.File]::Open("$PSScriptRoot/worker.lock",'OpenOrCreate','ReadWrite','None')
try {
    $record = @{pid=$PID;event=$env:LED_FORGE_TRAY_RESTART;args=@($CollectorHost,$Port,$DirectHost,$DirectPort,$DirectHttpPort,$LogPath);chat=$EnableChat.IsPresent;cwd=(Get-Location).Path;cmd=[Environment]::CommandLine}
    [IO.File]::WriteAllText("$PSScriptRoot/$PID.tmp",($record | ConvertTo-Json -Compress))
    [IO.File]::Move("$PSScriptRoot/$PID.tmp","$PSScriptRoot/$PID.ready")
    $end = [datetime]::UtcNow.AddSeconds(30)
    while ([datetime]::UtcNow -lt $end -and !(Test-Path "$PSScriptRoot/end")) {
        if (Test-Path "$PSScriptRoot/$PID.click") {
            if (!(Request-LocalAgentRestart) -or !(Request-LocalAgentRestart)) { throw 'Restart signal failed' }
            [IO.File]::Delete("$PSScriptRoot/$PID.click")
        }
        if (Test-Path "$UpdateControlPath.stop") { break }
        Start-Sleep -Milliseconds 50
    }
} finally { $lock.Dispose(); [IO.File]::WriteAllText("$PSScriptRoot/$PID.done",'') }
""")
        args = [str(PS), "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                str(root / "start_agent_supervisor.ps1"), "-InstanceName", "Fixture" + uuid.uuid4().hex,
                "-CollectorHost", "127.0.0.1", "-Port", "5451", "-DirectHost", "127.0.0.2",
                "-DirectPort", "5452", "-DirectHttpPort", "5453", "-LogPath", str(root / "custom.log")]
        opts = dict(env={**os.environ, "LOCALAPPDATA": directory}, creationflags=subprocess.CREATE_NO_WINDOW)
        supervisor = subprocess.Popen(args, **opts)
        try:
            first_path = wait(lambda: next(root.glob("*.ready"), None))
            first = json.loads(first_path.read_text())
            subprocess.run(args, timeout=5, check=True, **opts)
            assert len(list(root.glob("*.ready"))) == 1
            first_path.with_suffix(".click").touch()
            second_path = wait(lambda: next((p for p in root.glob("*.ready") if p != first_path), None))
            second = json.loads(second_path.read_text())
            assert first_path.with_suffix(".done").exists()
            assert first["pid"] != second["pid"] and first["event"] != second["event"]
            assert first["args"] == second["args"] and second["args"][-1] == str(root / "custom.log")
            assert second["cwd"] == str(root) and second["chat"] and "-WindowStyle Hidden" in second["cmd"]
            stale_env = dict(opts["env"], LED_FORGE_TRAY_RESTART=first["event"])
            subprocess.run([str(PS), "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
                            ". ./agent_chat_ipc.ps1; if (Request-LocalAgentRestart) {exit 1}"],
                           cwd=root, env=stale_env, creationflags=subprocess.CREATE_NO_WINDOW, check=True, timeout=5)
            time.sleep(0.6)
            assert len(list(root.glob("*.ready"))) == 2 and supervisor.poll() is None
        finally:
            supervisor.kill()
            supervisor.wait()
            (root / "end").touch()
            wait(lambda: all(p.with_suffix(".done").exists() for p in root.glob("*.ready")))
        print("PASS: hidden replacement, settings, single instance, repeated and stale tray clicks")


if __name__ == "__main__":
    main()
