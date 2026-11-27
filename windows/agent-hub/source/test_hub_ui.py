import json
from pathlib import Path
import tempfile
import threading
import tkinter as tk
import unittest
from unittest.mock import patch
from tkinter import font

import hub_ui as ui


class FixtureHub:
    def __init__(self, data_dir, host, port, release_dir):
        self.port, self.host, self.release_dir = port, host, release_dir
        self.records = {}
        self.commands = []
        self.started = False

    def start(self):
        self.started = True

    def stop(self):
        self.started = False

    def agents(self):
        return list(self.records.values())

    def invite(self, client, name):
        self.records[client] = dict(id=client, name=name, version=0, last_seen=0, status="Invited")
        return self.export_invite(client)

    def export_invite(self, client):
        return {"client": client, "secret": "fixture-only", "Host": self.host}

    def queue(self, client, action, name=""):
        self.commands.append((client, action, name))
        return len(self.commands)


class DesktopTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)
        self.data = self.base / "state"
        self.data.mkdir()
        self.release = self.base / "release"
        self.release.mkdir()
        for name in ("manifest.json", "package.zip"):
            (self.release / name).write_bytes(b"fixture")
        self.root = tk.Tk()
        self.addresses = {"LAN 1": "10.4.0.7", "LAN 2": "192.168.1.6"}
        self.fetch_thread = None
        self.checked_on = None

        def fetch(cache):
            self.fetch_thread = threading.get_ident()
            self.assertEqual(cache, self.data / "release-cache")
            return self.release

        def verify(path):
            self.checked_on = threading.get_ident()
            self.assertEqual(path, self.release)
            return 17

        self.panel = ui.Panel(self.root, self.data, FixtureHub, lambda: self.addresses,
                              fetch, self.base / "roster.json", verify)
        self.root.update_idletasks()

    def tearDown(self):
        self.root.after_cancel(self.panel.after_id)
        self.root.destroy()
        self.tmp.cleanup()

    def test_interface_selection_and_safe_auto_start(self):
        p = self.panel
        self.assertIsNone(p.hub)
        self.assertEqual(p.endpoint.get(), "LAN 1")
        p.auto_start()
        self.assertIsNone(p.hub)
        p.endpoint.set("LAN 2")
        p.toggle()
        self.assertEqual((p.hub.host, p.hub.port), ("192.168.1.6", 4220))
        self.assertTrue(p.address.instate(["disabled"]))
        p.toggle()
        p.reload_interfaces()
        p.auto_start()
        self.assertTrue(p.hub.started)
        p.toggle()
        self.addresses.clear()
        p.reload_interfaces()
        p.auto_start()
        self.assertIsNone(p.hub)
        self.assertTrue(p.switch.instate(["disabled"]))
        self.addresses["Ethernet"] = "10.55.5.6"
        p.reload_interfaces()
        p.auto_start()
        self.assertEqual(p.hub.host, "10.55.5.6")

    def test_invitation_cancel_save_failure_reexport_and_actions(self):
        p = self.panel
        client = "0000ABCD"
        p.toggle()
        self.assertTrue(p.buttons[-1].instate(["disabled"]))
        with patch.object(ui.filedialog, "asksaveasfilename", return_value=""):
            p.save_invite(client, "Влад", new=True)
        self.assertFalse(p.rows)
        target = self.base / "invite.json"
        with patch.object(ui.filedialog, "asksaveasfilename", return_value=str(target)):
            with patch.object(ui, "write_json", side_effect=OSError):
                p.save_invite(client, "Влад", new=True)
            self.assertEqual(p.tree.selection(), (client,))
            self.assertFalse(target.exists())
            p.act("export")
        self.assertEqual(json.loads(target.read_text())["secret"], "fixture-only")
        self.assertNotIn("fixture-only", p.notice.get() + str(p.tree.item(client)))
        with patch.object(ui.simpledialog, "askstring", return_value=""):
            p.act("rename")
        with patch.object(ui.simpledialog, "askstring", return_value="00000001"):
            p.act("uninstall")
        with patch.object(ui.messagebox, "askyesno", return_value=False):
            p.act("update")
        self.assertFalse(p.hub.commands)
        with patch.object(ui.simpledialog, "askstring", return_value="Новий ПК"):
            p.act("rename")
        with patch.object(ui.messagebox, "askyesno", return_value=True):
            p.act("update")
        with patch.object(ui.simpledialog, "askstring", return_value=client.lower()):
            p.act("uninstall")
        self.assertEqual(p.hub.commands, [(client, "rename", "Новий ПК"), (client, "update", ""),
                                          (client, "uninstall", "")])

    def test_roster_is_only_a_picker(self):
        p = self.panel
        p.roster.write_text(json.dumps({"self_id": "00000001", "agents": [
            {"id": "00000001", "name": "Self"}, {"id": "board", "name": "Board"},
            {"id": "ABCDEF01", "name": "Влад", "fingerprint": "unused"}]}))
        self.assertEqual(list(ui.roster_hints(p.roster).values()), [("ABCDEF01", "Влад")])
        self.assertIsNone(p.hub)
        p.roster.write_text("{}")
        self.assertEqual(ui.roster_hints(p.roster), {})

    def wait_for_fetch(self):
        # Wait for the worker's result, then dispatch through the normal Tk timer.
        result = self.panel.jobs.get(timeout=5)
        self.panel.jobs.put(result)
        self.root.after_cancel(self.panel.after_id)
        self.panel.tick()

    def test_release_switch_and_download_failure_preserve_jobs(self):
        p = self.panel
        p.toggle()
        service = p.hub
        service.commands.append(("ABCDEF01", "update", ""))
        p.download_release()
        self.assertTrue(p.fetching)
        self.wait_for_fetch()
        self.assertNotEqual(threading.get_ident(), self.fetch_thread)
        self.assertEqual(threading.get_ident(), self.checked_on)
        self.assertEqual(service.release_dir, self.release)
        self.assertEqual(json.loads(p.config.read_text())["release"], str(self.release))
        def fail(_cache):
            raise OSError("fixture offline")
        p.fetch = fail
        p.download_release()
        self.wait_for_fetch()
        self.assertFalse(p.fetching)
        self.assertEqual(service.release_dir, self.release)
        self.assertIs(service, p.hub)
        self.assertEqual(len(service.commands), 1)
        self.assertIn("не вдалося", p.notice.get())

    def test_expiry_after_offer_is_unconfirmed(self):
        p = self.panel
        p.toggle()
        p.hub.invite("0000ABCD", "Влад")
        row = p.hub.records["0000ABCD"]
        for action in ("rename", "update", "uninstall"):
            row["status"] = action + " offered"
            p.refresh_agents()
            row["status"] = action + " expired"
            p.refresh_agents()
            self.assertEqual("Час сплив · результат не підтверджено", p.tree.set(row["id"], "status"))
        self.root.geometry("780x470")
        self.root.update_idletasks()
        needed = font.nametofont("TkDefaultFont").measure(p.tree.set(row["id"], "status")) + 8
        self.assertGreaterEqual(p.tree.column("status", "width"), needed)

    def test_minimum_window_layout(self):
        self.root.geometry("780x470")
        self.root.update_idletasks()
        def check(parent):
            for widget in parent.winfo_children():
                if widget.winfo_ismapped():
                    self.assertLessEqual(widget.winfo_x() + widget.winfo_width(), parent.winfo_width() + 1)
                    self.assertLessEqual(widget.winfo_y() + widget.winfo_height(), parent.winfo_height() + 1)
                    check(widget)
        check(self.root)


if __name__ == "__main__":
    unittest.main()
