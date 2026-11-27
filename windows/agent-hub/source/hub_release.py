"""Download a matched, publisher-signed Windows agent release."""

import hashlib
import json
import re
import tempfile
import time
from pathlib import Path
from urllib.request import Request, urlopen


REPOSITORY = "Vihten/CODEX_firmware_esp8266"


def _download(url, limit):
    request = Request(url, headers={"User-Agent": "LED-Forge-Agent-Hub"})
    deadline = time.monotonic() + 30
    data = bytearray()
    with urlopen(request, timeout=8) as response:
        while len(data) <= limit and time.monotonic() < deadline:
            chunk = response.read(min(65536, limit + 1 - len(data)))
            if not chunk:
                return bytes(data)
            data.extend(chunk)
    raise ValueError("Перевищено розмір або час завантаження пакета.")


def fetch_published_release(cache_dir):
    from hub_service import verify_release

    ref = json.loads(_download(f"https://api.github.com/repos/{REPOSITORY}/git/ref/heads/main", 16384))
    revision = ref["object"]["sha"]
    if ref["object"]["type"] != "commit" or not re.fullmatch("[0-9a-f]{40}", revision):
        raise ValueError("Некоректна версія публікації GitHub.")
    base = f"https://raw.githubusercontent.com/{REPOSITORY}/{revision}/windows/agent"
    cache = Path(cache_dir)
    cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="download-", dir=cache) as temp:
        stage = Path(temp) / "release"
        stage.mkdir()
        for filename, limit in (("manifest.json", 8192), ("package.zip", 524288)):
            (stage / filename).write_bytes(_download(f"{base}/{filename}", limit))
        verify_release(stage)
        digest = hashlib.sha256((stage / "manifest.json").read_bytes()).hexdigest()
        dest = cache / digest
        try:
            stage.rename(dest)
        except OSError:
            if not dest.is_dir():
                raise
            verify_release(dest)
            if (dest / "manifest.json").read_bytes() != (stage / "manifest.json").read_bytes():
                raise ValueError("Локальна копія пакета відрізняється від публікації.")
        return dest
