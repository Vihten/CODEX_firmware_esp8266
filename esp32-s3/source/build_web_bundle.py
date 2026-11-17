"""Build a deterministic, bounded filesystem seed from the editable web tree."""

import hashlib
import gzip
import importlib.util
import re
from pathlib import Path
import struct
import sys


def build(root: Path) -> bytes:
    # Standalone exports keep generator/fragments beside web/. Test-only web
    # trees without a generator remain self-contained inputs to this builder.
    generator = root.parent / "portal" / "generator.py"
    if generator.is_file():
        spec = importlib.util.spec_from_file_location("portal_generator", generator)
        module = importlib.util.module_from_spec(spec)
        previous_bytecode = sys.dont_write_bytecode
        try:
            sys.dont_write_bytecode = True
            spec.loader.exec_module(module)
        finally:
            sys.dont_write_bytecode = previous_bytecode
        module.sync(root)
    files = sorted(
        (path for path in root.rglob("*") if path.is_file()),
        key=lambda path: path.relative_to(root).as_posix().encode("ascii"),
    )
    if not 1 <= len(files) <= 256:
        raise ValueError("Web bundle must contain 1..256 files")
    entries = {}
    for path in files:
        name = path.relative_to(root).as_posix()
        if len(name) > 127 or any(part in ("", ".", "..") for part in name.split("/")):
            raise ValueError(f"Invalid bundle path: {name}")
        name.encode("ascii")
        if any(not (char.isalnum() or char in "/._-") for char in name):
            raise ValueError(f"Invalid bundle characters: {name}")
        entries[name] = path.read_bytes()
    # HTML parsing normalizes CRLF; hash exactly the boot script text it executes.
    if "assets/js/boot.js" in entries:
        entries["assets/js/boot.js"] = entries["assets/js/boot.js"].replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    # Only public static assets are compressed/versioned, never private HTML pages.
    assets = {name: data for name, data in entries.items()
              if name.startswith(("assets/", "images/")) and name.endswith((".js", ".css", ".svg", ".webp"))}
    revisions = {name: hashlib.sha256(data).hexdigest()[:16] for name, data in assets.items()}
    aliases = {"/device.png": "images/device.webp", "/favicon.svg": "images/favicon.svg"}
    aliases.update({"/" + name: name for name in assets})
    for name, data in list(entries.items()):
        if name.startswith("templates/") and name.endswith(".html"):
            text = data.decode("utf-8")
            if name == "templates/shell/document-head.html":
                # No whitespace outside boot.js may enter the hashed script body.
                text = text.rstrip("\r\n")
            for url, asset in aliases.items():
                if asset in revisions:
                    text = re.sub(r"([\"'])" + re.escape(url) + r"([\"'])",
                                  lambda m: m[1] + url + "?v=" + revisions[asset] + m[2], text)
            entries[name] = text.encode("utf-8")
    for name, data in assets.items():
        if name.endswith((".js", ".css", ".svg")):
            compressed = gzip.compress(data, compresslevel=9, mtime=0)
            if len(compressed) < len(data):
                entries[name + ".gz"] = compressed
    if not 1 <= len(entries) <= 256:
        raise ValueError("Web bundle must contain 1..256 files after compression")
    records = bytearray()
    for name, data in sorted(entries.items()):
        if len(name) > 127 or any(part in ("", ".", "..") for part in name.split("/")):
            raise ValueError(f"Invalid bundle path: {name}")
        encoded = name.encode("ascii")
        if any(not (char.isalnum() or char in "/._-") for char in name):
            raise ValueError(f"Invalid bundle characters: {name}")
        if len(data) > 256 * 1024:
            raise ValueError(f"Web file exceeds 256 KiB: {name}")
        records += struct.pack("<HI", len(encoded), len(data))
        records += hashlib.sha256(data).digest() + encoded + data
    if len(records) > 1024 * 1024:
        raise ValueError("Web tree exceeds 1 MiB")
    return b"LFWEB01\0" + struct.pack("<I", len(entries)) + hashlib.sha256(records).digest() + records


if __name__ == "__main__":
    root, output = map(Path, sys.argv[1:])
    data = build(root)
    if not output.exists() or output.read_bytes() != data:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(data)
    print(f"Web bundle: {len(data)} bytes, {data[12:44].hex()}")
