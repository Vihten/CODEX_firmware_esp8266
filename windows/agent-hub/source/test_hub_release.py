import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from hub_release import fetch_published_release


class DownloadTest(unittest.TestCase):
    def test_release_from_single_revision_and_failed_refresh_keeps_cache(self):
        revision = "a" * 40
        calls = []
        files = {"manifest.json": b"manifest", "package.zip": b"archive"}

        def download(url, limit):
            calls.append(url)
            if url.endswith("/ref/heads/main"):
                return json.dumps({"object": {"sha": revision, "type": "commit"}}).encode()
            self.assertIn("/" + revision + "/windows/agent/", url)
            return files[url.rsplit("/", 1)[1]]

        def verify(directory):
            if (directory / "package.zip").read_bytes() != b"archive":
                raise ValueError("bad signature")

        with tempfile.TemporaryDirectory() as folder, patch("hub_release._download", download), \
                patch("hub_service.verify_release", side_effect=verify):
            cache = Path(folder)
            ready = fetch_published_release(cache)
            self.assertEqual(fetch_published_release(cache), ready)
            files["package.zip"] = b"broken"
            with self.assertRaises(ValueError):
                fetch_published_release(cache)
            self.assertEqual(list(cache.iterdir()), [ready])
            self.assertEqual((ready / "package.zip").read_bytes(), b"archive")
            self.assertEqual(len(calls), 9)
