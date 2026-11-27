"""Build the fixed November migration from the signed parent release (Windows)."""
import base64
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET
import zipfile

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import padding, rsa

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT / "build/schedule-ota-publish"
PARENT = "5a3ea7f9fc3dfe114e444eb76f1a1121a3759e2a"
OUT = ROOT / "build/agent-web-bridge-20261127/bridge"
VERSION = 20261126000000


def blob(name):
    return subprocess.check_output(["git", "-C", REPO, "show", f"{PARENT}:esp32-s3/agent/{name}"])


def number(xml, tag):
    return int.from_bytes(base64.b64decode(xml.findtext(tag), validate=True), "big")


def payload(m):
    return "LFAU1\n{version}\n{size}\n{sha256}\n".format(**m).encode("ascii")


def build():
    public = ET.fromstring(blob("agent-update-public.xml"))
    pub = rsa.RSAPublicNumbers(number(public, "Exponent"), number(public, "Modulus"))
    raw = blob("package.zip")
    manifest = json.loads(blob("manifest.json"))
    pub.public_key().verify(base64.b64decode(manifest["signature"]), payload(manifest), padding.PKCS1v15(), hashes.SHA256())
    if (hashlib.sha256(raw).hexdigest(), len(raw), manifest["version"]) != (
        manifest["sha256"], manifest["size"], 20261123090908
    ):
        raise ValueError("Incorrect baseline")
    with zipfile.ZipFile(io.BytesIO(raw)) as archive:
        files = {name: archive.read(name) for name in archive.namelist()}
    helper = files["agent_update.ps1"]
    if hashlib.sha256(helper).hexdigest() != "446d5e0d5c7b63164b06d7230c4dbf4640a1623f53772fc3d794bbdeebacb661":
        raise ValueError("Unexpected previous verifier")
    source = helper.decode("utf-8-sig").replace("\r\n", "\n")
    source = source.replace("'weekly_tokens.ps1', 'agent_update.ps1'", "'weekly_tokens.ps1', 'agent_chat_ipc.ps1', 'agent_chat_ui.ps1', 'agent_update.ps1'", 1)
    start = source.index("function Restore-AgentUpdate(")
    end = source.index("function Invoke-AgentUpdateRequest(", start)
    transaction = Path(__file__).with_name("agent_bridge_transaction.ps1").read_text(encoding="ascii")
    source = source[:start] + transaction + "\n" + source[end:]
    files["agent_update.ps1"] = source.replace("$base/agent-update/", "$base/agent-update-final/").encode("utf-8-sig")
    files["agent-version.txt"] = f"{VERSION}\n".encode()
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as archive:
        for name, data in files.items():
            entry = zipfile.ZipInfo(name)
            entry.create_system = 0
            entry.external_attr = 32
            archive.writestr(entry, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    data = buf.getvalue()
    local = Path(os.environ["LOCALAPPDATA"])
    candidates = [local, local / "Packages/OpenAI.Codex_2p2nqsd0c76g0/LocalCache/Local"]
    for directory in candidates:
        keypath = directory / "LED Forge/Agent Publisher/agent-update-private.xml"
        if keypath.is_file():
            private = ET.parse(keypath).getroot()
            if number(private, "Modulus") == pub.n and number(private, "Exponent") == pub.e:
                break
    else:
        raise ValueError("No publisher key matching the signed baseline")
    key = rsa.RSAPrivateNumbers(*(number(private, tag) for tag in ("P", "Q", "D", "DP", "DQ", "InverseQ")), pub).private_key()
    result = dict(schema=1, version=VERSION, size=len(data), sha256=hashlib.sha256(data).hexdigest())
    result["signature"] = base64.b64encode(key.sign(payload(result), padding.PKCS1v15(), hashes.SHA256())).decode()
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "package.zip").write_bytes(data)
    (OUT / "manifest.json").write_bytes(json.dumps(result, separators=(",", ":")).encode())
    print(result["sha256"], OUT)


if __name__ == "__main__":
    build()
