"""Run the firmware's staged update selection and HTTP artifact routing on the host."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/esp32s3/main"


class BridgeTest(unittest.TestCase):
    def test_shared_ip_and_update_convergence(self):
        source = (MAIN / "network/portal_api.inc").read_text(encoding="utf8")
        with tempfile.TemporaryDirectory(prefix="s3-bridge-") as tmp:
            work = Path(tmp)
            (work / "handlers.inc").write_text(
                source[source.index("uint32_t agentPeerAddress("):source.index("esp_err_t agentUpdatePost(")],
                encoding="utf8")
            test = ROOT / "tool/agent_update_bridge_test.cpp"
            if os.name == "nt":
                finder = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
                vs = subprocess.check_output([finder, "-latest", "-products", "*", "-requires",
                    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
                (work / "build.cmd").write_text(
                    f'@echo off\ncall "{vs}\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
                    f'cl /nologo /utf-8 /EHsc /std:c++17 /I. /I"{MAIN}" /Fe:test.exe "{test}"\n')
                subprocess.run(["cmd", "/d", "/c", "build.cmd"], cwd=work, check=True)
                exe = work / "test.exe"
            else:
                exe = work / "test"
                subprocess.run(["c++", "-std=c++17", "-I.", "-I", MAIN, test, "-o", exe], cwd=work, check=True)
            subprocess.run([exe], cwd=work, timeout=15, check=True)


if __name__ == "__main__":
    unittest.main()
