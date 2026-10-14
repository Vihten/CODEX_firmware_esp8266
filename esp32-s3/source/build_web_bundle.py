"""Build a deterministic, bounded filesystem seed from the editable web tree."""

import hashlib
from pathlib import Path
import struct
import sys


def build(root: Path) -> bytes:
    files = sorted(
        (path for path in root.rglob("*") if path.is_file()),
        key=lambda path: path.relative_to(root).as_posix().encode("ascii"),
    )
    if not 1 <= len(files) <= 256:
        raise ValueError("Web bundle must contain 1..256 files")
    records = bytearray()
    for path in files:
        name = path.relative_to(root).as_posix()
        if len(name) > 127 or any(part in ("", ".", "..") for part in name.split("/")):
            raise ValueError(f"Invalid bundle path: {name}")
        encoded = name.encode("ascii")
        if any(not (char.isalnum() or char in "/._-") for char in name):
            raise ValueError(f"Invalid bundle characters: {name}")
        data = path.read_bytes()
        if len(data) > 256 * 1024:
            raise ValueError(f"Web file exceeds 256 KiB: {name}")
        records += struct.pack("<HI", len(encoded), len(data))
        records += hashlib.sha256(data).digest() + encoded + data
    if len(records) > 1024 * 1024:
        raise ValueError("Web tree exceeds 1 MiB")
    return b"LFWEB01\0" + struct.pack("<I", len(files)) + hashlib.sha256(records).digest() + records


if __name__ == "__main__":
    root, output = map(Path, sys.argv[1:])
    data = build(root)
    if not output.exists() or output.read_bytes() != data:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(data)
    print(f"Web bundle: {len(data)} bytes, {data[12:44].hex()}")
