"""Refresh filesystem seed before PlatformIO scans embedded-file dependencies."""

from pathlib import Path
import subprocess
import sys

Import("env")

root = Path(env.subst("$PROJECT_DIR"))
subprocess.check_call([
    sys.executable,
    str(root / "build_web_bundle.py"),
    str(root / "web"),
    str(root / "main" / "web_bundle.bin"),
])
