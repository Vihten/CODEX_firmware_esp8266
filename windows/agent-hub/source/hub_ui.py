"""Local Windows management panel. All agent operations go through hub_service.Hub."""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import queue
import re
import subprocess
import tempfile
import threading
from datetime import datetime
import tkinter as tk
from tkinter import filedialog, messagebox, simpledialog, ttk


TOOL = Path(__file__).resolve().parent
DEFAULT_RELEASE = TOOL / "release"
DEFAULT_PORT = 4220
PRIVATE = tuple(map(ipaddress.ip_network, ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")))
CLIENT_ID = re.compile(r"[0-9A-F]{8}")


def windows_ps(script, extra_env=None):
    exe = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
    result = subprocess.run(
        [str(exe), "-NoProfile", "-NonInteractive", "-Command",
         "$ErrorActionPreference='Stop';[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false);" + script],
        capture_output=True, encoding="utf-8", timeout=15, check=True,
        creationflags=subprocess.CREATE_NO_WINDOW, env={**os.environ, **(extra_env or {})},
    )
    return result.stdout.strip()


def available_interfaces():
    output = windows_ps("""
        $cost = @{}
        Get-NetIPInterface -AddressFamily IPv4 -ConnectionState Connected | ForEach-Object {
            $cost[$_.InterfaceIndex] = $_.InterfaceMetric
        }
        $routes = @(Get-NetRoute -AddressFamily IPv4 -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue |
                    Where-Object { $cost.ContainsKey($_.InterfaceIndex) } |
                    Sort-Object { $_.RouteMetric + $cost[$_.InterfaceIndex] })
        $best = if ($routes.Count) { $routes[0].InterfaceIndex }
        $rows = @(Get-NetIPAddress -AddressFamily IPv4 -AddressState Preferred |
                  Where-Object { $cost.ContainsKey($_.InterfaceIndex) } |
                  Sort-Object @{Expression={ $_.InterfaceIndex -ne $best }},InterfaceIndex,IPAddress |
                  Select-Object IPAddress,InterfaceAlias)
        ConvertTo-Json -InputObject $rows -Compress
    """)
    choices = {}
    for row in json.loads(output):
        address = ipaddress.IPv4Address(row["IPAddress"])
        if any(address in net for net in PRIVATE):
            choices[f"{row['InterfaceAlias']}  ·  {address}"] = str(address)
    return choices


def private_state_dir():
    base = Path(os.environ["LOCALAPPDATA"]).resolve()
    path = base / "LED Forge" / "Agent Hub"
    source = TOOL.parent.parent if TOOL.parent.name.lower() == "tools" else TOOL
    if path.resolve().is_relative_to(source) or path.resolve() != path:
        raise ValueError("Небезпечний шлях до локальних даних.")
    path.mkdir(parents=True, exist_ok=True)
    windows_ps("""
        $p=$env:AGENT_HUB_PRIVATE_DIR
        $me=[Security.Principal.WindowsIdentity]::GetCurrent().User
        $acl=[IO.Directory]::GetAccessControl($p)
        if ($acl.GetOwner([Security.Principal.SecurityIdentifier]) -ne $me) { throw 'Foreign state directory' }
        $acl.SetAccessRuleProtection($true,$false)
        foreach ($rule in @($acl.Access)) { [void]$acl.RemoveAccessRuleAll($rule) }
        foreach ($id in @($me.Value,'S-1-5-18','S-1-5-32-544')) {
            $sid=[Security.Principal.SecurityIdentifier]::new($id)
            $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
                $sid,'FullControl','ContainerInherit,ObjectInherit','None','Allow'))
        }
        [IO.Directory]::SetAccessControl($p,$acl)
    """, {"AGENT_HUB_PRIVATE_DIR": str(path)})
    return path


def write_json(path, value):
    """Commit a complete document on the same volume, including on retry."""
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False, mode="w", encoding="utf-8") as stream:
        temporary = Path(stream.name)
    try:
        with temporary.open("w", encoding="utf-8") as stream:
            json.dump(value, stream, ensure_ascii=False, indent=2)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def roster_hints(path):
    try:
        if path.stat().st_size > 131072:
            return {}
        snapshot = json.loads(path.read_text(encoding="utf-8-sig"))
        hints = {}
        for row in snapshot.get("agents", []) + snapshot.get("candidates", []):
            client, name = row.get("id"), row.get("name")
            if (isinstance(client, str) and CLIENT_ID.fullmatch(client)
                    and client != snapshot["self_id"] and isinstance(name, str)
                    and name.strip() and name.isprintable() and len(name) <= 128):
                hints.setdefault(f"{name}  ·  {client}", (client, name))
        return hints
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return {}


class InviteDialog(simpledialog.Dialog):
    def __init__(self, parent, hints):
        self.hints = hints
        super().__init__(parent, "Запросити ПК")

    def body(self, frame):
        ttk.Label(frame, text="Виберіть ПК з офісного чату або введіть його ID та назву.").grid(
            row=0, column=0, columnspan=2, sticky="w", pady=(2, 12))
        picker = ttk.Combobox(frame, values=list(self.hints), state="readonly", width=40)
        ttk.Label(frame, text="ПК з чату").grid(row=1, column=0, sticky="w", padx=(0, 12))
        picker.grid(row=1, column=1, sticky="ew")
        self.client = tk.StringVar(frame)
        self.name = tk.StringVar(frame)
        for i, (caption, variable) in enumerate((("ID ПК (8 hex)", self.client), ("Назва ПК", self.name)), 2):
            ttk.Label(frame, text=caption).grid(row=i, column=0, sticky="w")
            entry = ttk.Entry(frame, textvariable=variable)
            entry.grid(row=i, column=1, sticky="ew", pady=4)
            if i == 2:
                self.id_field = entry

        def pick(_event):
            client, name = self.hints[picker.get()]
            self.client.set(client)
            self.name.set(name)

        picker.bind("<<ComboboxSelected>>", pick)
        return picker if self.hints else self.id_field

    def validate(self):
        client, name = self.client.get().strip().upper(), self.name.get().strip()
        if not CLIENT_ID.fullmatch(client) or not name or not name.isprintable() or len(name.encode("utf-8")) > 48:
            messagebox.showerror("Запрошення", "Введіть ID із 8 цифр або літер A–F та назву до 48 байтів UTF-8.", parent=self)
            return False
        self.result = client, name
        return True

    def buttonbox(self):
        box = ttk.Frame(self)
        box.pack(fill="x", padx=10, pady=8)
        ttk.Button(box, text="Далі…", command=self.ok).pack(side="right")
        ttk.Button(box, text="Скасувати", command=self.cancel).pack(side="right", padx=6)
        self.bind("<Return>", self.ok)
        self.bind("<Escape>", self.cancel)


def production_hub(*args):
    from hub_service import Hub
    return Hub(*args)


def fetch_release(cache):
    from hub_release import fetch_published_release
    return fetch_published_release(cache)


def verified_version(path):
    from hub_service import verify_release
    return verify_release(path)


def status_text(status):
    known = {"Invited": "Запрошено", "Connected": "На зв’язку", "Update confirmed": "Версію підтверджено",
             "Update superseded": "Оновлення замінено новішою версією",
             "Update blocked by failed version": "Оновлення заблоковано після помилки версії"}
    if status in known:
        return known[status]
    action, _, state = status.lower().partition(" ")
    actions = {"rename": "Перейменування", "update": "Оновлення", "uninstall": "Видалення"}
    if action in actions and state == "expired":
        return "Час сплив · результат не підтверджено"
    states = {"queued": "у черзі", "offered": "запропоновано агенту",
              "acknowledged": "підтверджено агентом", "failed": "помилка"}
    if action in actions and state in states:
        return f"{actions[action]} — {states[state]}"
    return status


class Panel:
    def __init__(self, root, data_dir, factory=production_hub, interfaces=available_interfaces,
                 fetch=fetch_release, roster=None, verify=verified_version):
        self.root, self.data = root, Path(data_dir)
        self.factory, self.interfaces, self.fetch, self.verify = factory, interfaces, fetch, verify
        self.roster = roster or Path(os.environ["LOCALAPPDATA"]) / "CodexLanAgent/chat/roster.json"
        self.config = self.data / "panel.json"
        self.hub = None
        self.rows = {}
        self.jobs = queue.Queue()
        self.fetching = False
        try:
            prefs = json.loads(self.config.read_text(encoding="utf-8"))
            if not isinstance(prefs, dict):
                prefs = {}
        except (OSError, ValueError):
            prefs = {}
        self.preferred = prefs.get("host")
        self.saved_label = prefs.get("interface")
        self.saved_port = prefs.get("port", DEFAULT_PORT)
        self.release = Path(str(prefs.get("release", DEFAULT_RELEASE)))
        self.port = tk.StringVar(root, str(prefs.get("port", DEFAULT_PORT)))
        self.endpoint = tk.StringVar(root)
        self.state = tk.StringVar(root, "Зупинено")
        self.notice = tk.StringVar(root)
        self.folder = tk.StringVar(root, str(self.release))
        root.title("Керування агентами")
        root.minsize(780, 470)
        root.geometry("940x550")
        frame = ttk.Frame(root, padding=16)
        frame.pack(fill="both", expand=True)
        frame.columnconfigure(0, weight=1)
        frame.rowconfigure(4, weight=1)
        bar = ttk.Frame(frame)
        bar.grid(row=0, column=0, sticky="ew")
        ttk.Label(bar, text="Керування агентами", font=("Segoe UI", 16, "bold")).pack(side="left")
        ttk.Label(bar, textvariable=self.state).pack(side="right")
        server = ttk.Frame(frame)
        server.grid(row=1, column=0, sticky="ew", pady=(16, 6))
        server.columnconfigure(1, weight=1)
        ttk.Label(server, text="Інтерфейс").grid(row=0, column=0, padx=(0, 6))
        self.address = ttk.Combobox(server, textvariable=self.endpoint, state="readonly")
        self.address.grid(row=0, column=1, sticky="ew")
        self.scan = ttk.Button(server, text="Оновити", command=self.reload_interfaces)
        self.scan.grid(row=0, column=2, padx=6)
        ttk.Label(server, text="Порт").grid(row=0, column=3)
        self.port_entry = ttk.Entry(server, width=6, textvariable=self.port)
        self.port_entry.grid(row=0, column=4, padx=6)
        self.switch = ttk.Button(server, command=self.toggle)
        self.switch.grid(row=0, column=5)
        releases = ttk.Frame(frame)
        releases.grid(row=2, column=0, sticky="ew", pady=(0, 14))
        releases.columnconfigure(1, weight=1)
        ttk.Label(releases, text="Реліз").grid(row=0, column=0, padx=(0, 6))
        ttk.Entry(releases, textvariable=self.folder, state="readonly").grid(row=0, column=1, sticky="ew")
        self.choose = ttk.Button(releases, text="Вибрати теку…", command=self.choose_release)
        self.choose.grid(row=0, column=2, padx=6)
        self.download = ttk.Button(releases, text="Завантажити з GitHub", command=self.download_release)
        self.download.grid(row=0, column=3)
        toolbar = ttk.Frame(frame)
        toolbar.grid(row=3, column=0, sticky="ew", pady=(0, 6))
        ttk.Label(toolbar, text="Комп’ютери", font=("Segoe UI", 12, "bold")).pack(side="left")
        self.invite_btn = ttk.Button(toolbar, text="Запросити ПК…", command=self.new_invite)
        self.invite_btn.pack(side="right")
        table = ttk.Frame(frame)
        table.grid(row=4, column=0, sticky="nsew")
        self.tree = ttk.Treeview(table, columns=("name", "id", "version", "seen", "status"),
                                 show="headings", selectmode="browse")
        for col, label, width in zip(self.tree["columns"], ("Назва", "ID ПК", "Версія", "Останній зв’язок", "Стан"),
                                     (130, 90, 115, 150, 280)):
            self.tree.heading(col, text=label, anchor="w")
            self.tree.column(col, width=width, minwidth=65, anchor="w", stretch=col in ("name", "status"))
        scroll = ttk.Scrollbar(table, command=self.tree.yview)
        scroll.pack(side="right", fill="y")
        self.tree.configure(yscrollcommand=scroll.set)
        self.tree.pack(fill="both", expand=True)
        self.tree.bind("<<TreeviewSelect>>", lambda _e: self.controls())
        actions = ttk.Frame(frame)
        actions.grid(row=5, column=0, sticky="ew", pady=8)
        self.buttons = []
        for title, action in (("Перейменувати…", "rename"), ("Оновити агента", "update"),
                              ("Зберегти запрошення…", "export"), ("Видалити агента…", "uninstall")):
            btn = ttk.Button(actions, text=title, command=lambda a=action: self.act(a))
            btn.pack(side="right" if action == "uninstall" else "left", padx=(0, 4))
            self.buttons.append(btn)
        ttk.Label(frame, textvariable=self.notice, wraplength=735).grid(row=6, column=0, sticky="ew")
        self.reload_interfaces()
        self.after_id = root.after(800, self.tick)
        root.protocol("WM_DELETE_WINDOW", self.close)


    def controls(self):
        live = self.hub is not None
        self.address.configure(state="disabled" if live else "readonly")
        self.port_entry.configure(state="disabled" if live else "normal")
        self.scan.state(["disabled" if live else "!disabled"])
        self.switch.configure(text="Зупинити хаб" if live else "Запустити хаб")
        self.switch.state(["!disabled" if live or self.endpoint.get() in self.choices else "disabled"])
        self.invite_btn.state(["!disabled" if live else "disabled"])
        for button in self.buttons:
            button.state(["!disabled" if live and self.tree.selection() else "disabled"])
        for button in (self.choose, self.download):
            button.state(["disabled" if self.fetching else "!disabled"])

    def reload_interfaces(self):
        old = getattr(self, "choices", {}).get(self.endpoint.get(), self.preferred)
        try:
            self.choices = self.interfaces()
        except (OSError, ValueError, subprocess.SubprocessError):
            self.choices = {}
        self.address.configure(values=list(self.choices))
        labels = {ip: name for name, ip in self.choices.items()}
        preferred_label = labels.get(old)
        if self.saved_label in self.choices and self.choices[self.saved_label] == old:
            preferred_label = self.saved_label
        self.endpoint.set(preferred_label or next(iter(self.choices), ""))
        self.notice.set("Перевірте адресу й запустіть хаб." if self.choices else
                        "Приватної IPv4-адреси не знайдено. Під’єднайте ПК до локальної мережі й натисніть «Оновити».")
        self.controls()

    def save_config(self, release):
        write_json(self.config, {"host": self.preferred, "port": self.saved_port, "interface": self.saved_label,
                                 "release": str(release)})

    def auto_start(self):
        selected = self.endpoint.get()
        same_binding = (selected in self.choices and self.choices[selected] == self.preferred and
                        (selected == self.saved_label or
                         (self.saved_label is None and list(self.choices.values()).count(self.preferred) == 1)))
        if len(self.choices) == 1 or same_binding:
            self.toggle()
        elif self.choices:
            self.notice.set("Доступно кілька адрес. Виберіть інтерфейс і натисніть «Запустити хаб».")

    def toggle(self):
        if self.hub is not None:
            try:
                self.hub.stop()
            except Exception:
                self.notice.set("Не вдалося зупинити хаб. Повторіть спробу.")
                return
            self.hub = None
            self.state.set("Зупинено")
            self.notice.set("Агенти зможуть зв’язатися з хабом після наступного запуску.")
            self.tree.delete(*self.tree.get_children())
            self.rows.clear()
        else:
            service = None
            try:
                host = self.choices[self.endpoint.get()]
                port = int(self.port.get())
                if not 1 <= port <= 65535:
                    raise ValueError
            except (ValueError, KeyError):
                self.notice.set("Виберіть інтерфейс і порт від 1 до 65535.")
                return
            try:
                service = self.factory(self.data, host, port, self.release)
                service.start()
            except Exception:
                if service is not None:
                    service.stop()
                self.notice.set("Запуск не вдався. Перевірте адресу, вільний порт і доступ до локальних даних.")
                return
            self.hub = service
            self.preferred, self.saved_label, self.saved_port = host, self.endpoint.get(), port
            self.state.set(f"Працює  ·  {host}:{service.port}")
            self.notice.set("Щоб додати комп’ютер, натисніть «Запросити ПК…».")
            try:
                self.save_config(self.release)
            except OSError:
                self.notice.set("Хаб працює, але зберегти параметри запуску не вдалося.")
            self.refresh_agents()
        self.controls()

    def refresh_agents(self):
        if self.hub is None:
            return
        try:
            rows = {row["id"]: row for row in self.hub.agents()}
            for client, row in rows.items():
                seen = datetime.fromtimestamp(row["last_seen"]).strftime("%d.%m %H:%M:%S") if row["last_seen"] else "Ще не виходив"
                values = (row["name"], client, row["version"] or "—", seen, status_text(row["status"]))
                if self.tree.exists(client):
                    self.tree.item(client, values=values)
                else:
                    self.tree.insert("", "end", iid=client, values=values)
            for client in self.rows.keys() - rows.keys():
                self.tree.delete(client)
            self.rows = rows
        except Exception:
            self.notice.set("Список ПК не оновлено. Перевірте локальний стан хаба.")
            self.rows = {}
            self.tree.delete(*self.tree.get_children())
        self.controls()

    def set_release(self, path):
        path = Path(path).resolve()
        if not all((path / name).is_file() for name in ("manifest.json", "package.zip")):
            raise ValueError("У теці мають бути manifest.json та package.zip.")
        version = self.verify(path)
        self.save_config(path)
        if self.hub is not None:
            self.hub.release_dir = path
        self.release = path
        self.folder.set(str(path))
        self.notice.set(f"Реліз {version} перевірено й вибрано для наступних оновлень.")

    def choose_release(self):
        path = filedialog.askdirectory(parent=self.root, title="Тека розпакованого релізу",
                                       initialdir=self.release if self.release.is_dir() else TOOL, mustexist=True)
        if path:
            self.apply_release(path)

    def apply_release(self, path):
        try:
            self.set_release(path)
        except (OSError, ValueError):
            self.notice.set("Реліз не змінено. Перевірте manifest.json, package.zip і доступ до теки.")

    def download_release(self):
        if self.fetching:
            return
        self.fetching = True
        self.controls()
        self.notice.set("Завантаження релізу з GitHub…")
        def work():
            try:
                self.jobs.put((True, self.fetch(self.data / "release-cache")))
            except Exception:
                self.jobs.put((False, None))
        threading.Thread(target=work, name="hub-release-download", daemon=True).start()

    def tick(self):
        try:
            success, path = self.jobs.get_nowait()
        except queue.Empty:
            pass
        else:
            self.fetching = False
            if success:
                self.apply_release(path)
            else:
                self.notice.set("Завантаження не вдалося. Попередній реліз доступний.")
            self.controls()
        self.refresh_agents()
        self.after_id = self.root.after(1500, self.tick)

    def new_invite(self):
        if self.hub is None:
            return
        dialog = InviteDialog(self.root, roster_hints(self.roster))
        if dialog.result:
            client, name = dialog.result
            if client in self.rows:
                self.notice.set("Цей ID уже додано. Виберіть ПК і натисніть «Зберегти запрошення…».")
                return
            self.save_invite(client, name, new=True)

    def save_invite(self, client, name, new=False):
        selected = filedialog.asksaveasfilename(parent=self.root, title=f"Приватне запрошення для {name} ({client})",
                                                initialdir=Path.home(), initialfile=f"hub-{client}.json", defaultextension=".json",
                                                filetypes=[("Запрошення JSON", "*.json")])
        if not selected:
            return
        path = Path(selected).resolve()
        source = TOOL.parent.parent if TOOL.parent.name.lower() == "tools" else TOOL
        if path.suffix.lower() != ".json" or any(path.is_relative_to(d) for d in (source, self.data, self.release)):
            self.notice.set("Виберіть особисту теку поза проєктом, релізом та локальними даними хаба.")
            return
        try:
            invite = self.hub.invite(client, name) if new else self.hub.export_invite(client)
        except Exception:
            self.notice.set(f"Запрошення для {client} не отримано. Перевірте ID та список агентів.")
        else:
            try:
                write_json(path, invite)
            except (ValueError, OSError, TypeError):
                self.notice.set(f"Файл для {client} не збережено. Виберіть цей ПК і натисніть «Зберегти запрошення…».")
            else:
                self.notice.set(f"Запрошення для {name} ({client}) збережено. Передайте його власнику ПК приватно.")
        self.refresh_agents()
        if self.tree.exists(client):
            self.tree.selection_set(client)
            self.tree.see(client)
            self.controls()

    def act(self, action):
        selected = self.tree.selection()
        if self.hub is None or not selected or selected[0] not in self.rows:
            return
        client = selected[0]
        name = self.rows[client]["name"]
        target = f"{name} ({client})"
        if action == "export":
            self.save_invite(client, name)
            return
        new_name = ""
        if action == "rename":
            new_name = simpledialog.askstring("Перейменувати агента", f"Нова назва для {target}",
                                              initialvalue=name, parent=self.root)
            if not new_name or not new_name.strip() or new_name == name:
                return
            new_name = new_name.strip()
        elif action == "update":
            if not messagebox.askyesno("Оновити агента", f"Оновити {target} релізом із теки\n{self.release}?",
                                       parent=self.root, default="no"):
                return
        elif action == "uninstall":
            typed = simpledialog.askstring("Видалити агента", f"Щоб видалити агента з {target}, введіть ID цього ПК.",
                                           parent=self.root)
            if typed is None:
                return
            if typed.strip().upper() != client:
                self.notice.set("ID не збігається. Команду скасовано.")
                return
        else:
            return
        if self.hub is None or client not in self.rows or self.rows[client]["name"] != name:
            self.notice.set("Дані ПК змінилися. Перевірте вибраний комп’ютер.")
            return
        try:
            job = self.hub.queue(client, action, new_name)
        except Exception:
            self.notice.set(f"Команду для {target} відхилено. Перевірте стан завдання, назву та вибраний реліз.")
        else:
            self.notice.set(f"Завдання {job} для {target} у черзі. Результат з’явиться в стовпці «Стан».")
        self.refresh_agents()

    def close(self):
        if self.hub is not None:
            if not messagebox.askyesno("Закрити панель", "Зупинити локальний хаб і закрити панель?",
                                       parent=self.root, default="no"):
                return
            self.toggle()
            if self.hub is not None:
                return
        self.root.after_cancel(self.after_id)
        self.root.destroy()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--start", action="store_true", help="Start on a single interface or an exact saved binding")
    args = parser.parse_args()
    root = tk.Tk()
    root.withdraw()
    try:
        panel = Panel(root, private_state_dir())
    except Exception:
        messagebox.showerror("Керування агентами", "Не вдалося відкрити панель. Перевірте доступ до приватної теки даних.",
                             parent=root)
        root.destroy()
        return
    root.deiconify()
    if args.start:
        root.after_idle(panel.auto_start)
    root.mainloop()


if __name__ == "__main__":
    main()
