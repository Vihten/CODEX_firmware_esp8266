"""Build a standalone Windows hub archive from an already signed agent release."""
import argparse
import os
import tempfile
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile

from hub_service import verify_release


SOURCE = Path(__file__).resolve().parent
PROGRAM = ("hub_ui.py", "hub_service.py", "hub_release.py", "agent-update-public.xml",
           "configure_hub_firewall.ps1", "configure_hub_firewall.cmd", "install_hub.ps1",
           "install_hub.cmd", "start_hub.vbs", "README_UK.md")


def package(release, output):
    version = verify_release(release)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temp:
        archive = Path(temp) / "hub.zip"
        with ZipFile(archive, "w", compression=ZIP_DEFLATED) as bundle:
            for name in PROGRAM:
                bundle.write(SOURCE / name, name)
            for name in ("manifest.json", "package.zip"):
                bundle.write(release / name, f"release/{name}")
        with ZipFile(archive) as check:
            if check.testzip():
                raise ValueError("Incomplete hub archive")
        os.replace(archive, output)
    return version


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    print(package(args.release, args.output))
