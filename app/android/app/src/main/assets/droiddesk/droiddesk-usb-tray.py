"""droiddesk-usb-tray — the "USB disky" icon in the XFCE panel.

  droiddesk-usb-tray            icon in the panel (systray plugin), started by autostart
  droiddesk-usb-tray --window   open the window (starts the icon too when it is not running)

The window lists USB flash drives and card readers (capacity, filesystem, label) with buttons
Připojit do Linuxu / Vysunout / Soubory… / Zapsat obraz… / Uložit obraz… / Naformátovat…; the
work is done by droiddesk-usb (same folder), progress comes from its --yad lines. A newly plugged drive shows a notification with "Připojit do Linuxu".
"""
import importlib.util
import os
import socket
import subprocess
import sys
import threading
import time
import warnings

try:
    import gi

    gi.require_version("Gtk", "3.0")
    # Gdk too: without it gi may pick Gdk 4.0 first, and Gtk 3 then fails to load.
    gi.require_version("Gdk", "3.0")
    from gi.repository import Gdk, GLib, Gtk
except (ImportError, ValueError, AttributeError) as error:
    # Installs from before fold.16 lack pygobject. at-spi2 leaves gi/overrides/ behind,
    # so "import gi" can succeed as an empty namespace package (AttributeError).
    # From the menu: install it in a terminal the user can see, then open the window
    # once more; at autostart: stay quiet.
    gi_module = sys.modules.get("gi")
    have_pygobject = gi_module is not None and hasattr(gi_module, "require_version")
    if "--window" in sys.argv and (have_pygobject or os.environ.get("DROIDDESK_USB_TRAY_INSTALLED")):
        # pygobject is there but GTK does not load (typelibs, display): say so, no loop.
        subprocess.run(["notify-send", "-i", "dialog-error", "USB disky", f"GTK se nenačetlo: {error}"],
                       check=False)
    elif "--window" in sys.argv:
        wrapper = os.path.join(os.path.dirname(os.path.abspath(__file__)), "droiddesk-usb-tray")
        subprocess.Popen(["xfce4-terminal", "--title=USB disky", "-x", "bash", "-c",
                          "echo 'USB disky potřebují balíček pygobject, instaluji…'; "
                          "pkg install -y pygobject && "
                          f"(DROIDDESK_USB_TRAY_INSTALLED=1 setsid '{wrapper}' --window &); "
                          "echo; read -rp 'Enter zavře okno. '"])
    sys.exit(0)

warnings.filterwarnings("ignore", category=DeprecationWarning)  # Gtk.StatusIcon

HERE = os.path.dirname(os.path.abspath(__file__))
CLIENT = os.path.join(HERE, "droiddesk-usb.py")
INSTANCE = "\0droiddesk.usb-tray"
ICON = "drive-removable-media"
# DROIDDESK_USB_ORIGIN: operations started here notify through their progress window already.
ENV = dict(os.environ, PYTHONUTF8="1", DROIDDESK_USB_ORIGIN="tray")

spec = importlib.util.spec_from_file_location("droiddesk_usb", CLIENT)
usb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(usb)


def storage_devices():
    try:
        return [d for d in usb.devices() if d["storage"] or d["held"]]
    except SystemExit:  # the app does not answer
        return None


def label_of(dev):
    return " ".join(x for x in (dev["maker"], dev["product"]) if x) or "neznámé zařízení"


def run_client(*args):
    out = subprocess.run([sys.executable, CLIENT, *args], capture_output=True, text=True, env=ENV)
    text = (out.stdout + out.stderr).strip().splitlines()
    return out.returncode, (text[-1] if text else "")


def client_lines(*args):
    """(exit code, stdout lines) of droiddesk-usb."""
    out = subprocess.run([sys.executable, CLIENT, *args], capture_output=True, text=True, env=ENV)
    return out.returncode, out.stdout.splitlines(), (out.stderr.strip().splitlines() or [""])[-1]


def disk_summary(name):
    """(capacity, filesystems, labels) for the device list, from "info --raw"."""
    code, lines, error = client_lines("info", name, "--raw")
    if code != 0:
        return ("?", "?", error.removeprefix("droiddesk-usb: "))
    capacity, systems, labels = "", [], []
    for line in lines:
        fields = line.split("\t")
        if fields[0] == "disk":
            capacity = usb.human(int(fields[1]))
        elif fields[0] == "part" and len(fields) >= 6:
            systems.append(usb.FS_NAMES.get(fields[4], fields[4] or "?"))
            if fields[5]:
                labels.append(fields[5])
    return (capacity, ", ".join(systems) or "žádný", ", ".join(labels))


def progress(parent, title, args, on_done=None):
    """Runs droiddesk-usb with --yad output in a modal window with a progress bar."""
    dialog = Gtk.Dialog(title=title, transient_for=parent, modal=True)
    dialog.set_default_size(460, -1)
    area = dialog.get_content_area()
    area.set_spacing(8)
    area.set_border_width(12)
    bar = Gtk.ProgressBar(show_text=True)
    text = Gtk.Label(xalign=0, wrap=True, selectable=True)
    area.pack_start(bar, False, False, 0)
    area.pack_start(text, False, False, 0)
    close = dialog.add_button("Zavřít", Gtk.ResponseType.CLOSE)
    close.set_sensitive(False)
    dialog.connect("delete-event", lambda *_: not close.get_sensitive())
    dialog.show_all()

    def line(value):
        if value.isdigit():
            bar.set_fraction(min(int(value), 100) / 100)
        elif value.startswith("#"):
            text.set_text(value[1:].strip())
        elif value:  # errors from droiddesk-usb itself (sys.exit)
            text.set_text(value.removeprefix("droiddesk-usb: "))
        return False

    def done(code):
        close.set_sensitive(True)
        if code == 0:
            bar.set_fraction(1)
        else:
            bar.set_text("Chyba")
        usb.notify(f"{title}: {'hotovo' if code == 0 else 'chyba — ' + text.get_text()}")
        if on_done:
            on_done(code)
        return False

    def work():
        proc = subprocess.Popen([sys.executable, CLIENT, *args], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, env=ENV)
        for value in proc.stdout:
            GLib.idle_add(line, value.strip())
        GLib.idle_add(done, proc.wait())
    threading.Thread(target=work, daemon=True).start()
    dialog.run()
    dialog.destroy()


def confirm(parent, title, text, action):
    dialog = Gtk.MessageDialog(parent=parent, modal=True, message_type=Gtk.MessageType.WARNING,
                               buttons=Gtk.ButtonsType.NONE, text=title)
    dialog.format_secondary_text(text)
    dialog.add_buttons("Zrušit", Gtk.ResponseType.CANCEL, action, Gtk.ResponseType.OK)
    answer = dialog.run()
    dialog.destroy()
    return answer == Gtk.ResponseType.OK


def ask_text(parent, title, prompt, value=""):
    dialog = Gtk.Dialog(title=title, transient_for=parent, modal=True)
    dialog.add_buttons("Zrušit", Gtk.ResponseType.CANCEL, "OK", Gtk.ResponseType.OK)
    dialog.set_default_response(Gtk.ResponseType.OK)
    area = dialog.get_content_area()
    area.set_spacing(8)
    area.set_border_width(12)
    area.pack_start(Gtk.Label(label=prompt, xalign=0), False, False, 0)
    entry = Gtk.Entry(text=value, activates_default=True)
    area.pack_start(entry, False, False, 0)
    dialog.show_all()
    answer = dialog.run()
    result = entry.get_text().strip()
    dialog.destroy()
    return result if answer == Gtk.ResponseType.OK and result else None


class FilesWindow:
    """Files on the disk (FAT, exFAT, NTFS, ext2/3/4): browse, upload, download, new folder, delete."""

    def __init__(self, parent, name, title):
        self.name = name
        self.path = "/"
        win = Gtk.Window(title=f"Soubory — {title}", transient_for=parent)
        win.set_icon_name(ICON)
        win.set_default_size(640, 440)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8, margin=12)
        top = Gtk.Box(spacing=6)
        up = Gtk.Button(label="Nahoru")
        up.connect("clicked", lambda *_: self.go(self.path.rsplit("/", 1)[0] or "/"))
        top.pack_start(up, False, False, 0)
        self.where = Gtk.Label(xalign=0, selectable=True)
        top.pack_start(self.where, True, True, 0)
        box.pack_start(top, False, False, 0)
        # icon, name, size text, is folder
        self.store = Gtk.ListStore(str, str, str, bool)
        view = Gtk.TreeView(model=self.store)
        view.get_selection().set_mode(Gtk.SelectionMode.MULTIPLE)
        column = Gtk.TreeViewColumn("Název")
        icon = Gtk.CellRendererPixbuf()
        column.pack_start(icon, False)
        column.add_attribute(icon, "icon-name", 0)
        text = Gtk.CellRendererText()
        column.pack_start(text, True)
        column.add_attribute(text, "text", 1)
        column.set_expand(True)
        view.append_column(column)
        size = Gtk.CellRendererText(xalign=1)
        view.append_column(Gtk.TreeViewColumn("Velikost", size, text=2))
        view.connect("row-activated", self.activated)
        self.view = view
        scroll = Gtk.ScrolledWindow(vexpand=True)
        scroll.add(view)
        box.pack_start(scroll, True, True, 0)
        self.status = Gtk.Label(xalign=0, wrap=True, selectable=True)
        box.pack_start(self.status, False, False, 0)
        buttons = Gtk.Box(spacing=6)
        for text_, action in (("Nahrát soubory…", self.upload_files), ("Nahrát složku…", self.upload_folder),
                              ("Stáhnout…", self.download), ("Nová složka…", self.new_folder),
                              ("Smazat", self.delete), ("Obnovit", lambda: self.go(self.path))):
            button = Gtk.Button(label=text_)
            button.connect("clicked", lambda *_, a=action: a())
            buttons.pack_start(button, False, False, 0)
        box.pack_start(buttons, False, False, 0)
        win.add(box)
        self.window = win
        win.show_all()
        self.go("/")

    def go(self, path):
        self.path = path
        self.where.set_text(f"usb:{path}")
        self.status.set_text("Načítám…")

        def work():
            code, lines, error = client_lines("ls", self.name, "--raw", f"usb:{path}")
            GLib.idle_add(self.listed, path, code, lines, error)
        threading.Thread(target=work, daemon=True).start()

    def listed(self, path, code, lines, error):
        if path != self.path:
            return False
        self.store.clear()
        if code != 0:
            self.status.set_text(error.removeprefix("droiddesk-usb: "))
            return False
        for line in lines:
            kind, size, name = (line.split("\t", 2) + ["", "", ""])[:3]
            is_dir = kind == "d"
            self.store.append(["folder" if is_dir else "text-x-generic", name,
                               "" if is_dir else usb.human(int(size or 0)), is_dir])
        count = len(lines)
        noun = "položka" if count == 1 else "položky" if 2 <= count <= 4 else "položek"
        self.status.set_text(f"{count} {noun}" if lines else "Prázdná složka")
        return False

    def child(self, name):
        return f"{self.path.rstrip('/')}/{name}"

    def activated(self, view, path, column):
        row = self.store[path]
        if row[3]:
            self.go(self.child(row[1]))

    def selected(self):
        model, paths = self.view.get_selection().get_selected_rows()
        return [model[p][1] for p in paths]

    def choose(self, title, action, multiple=False):
        dialog = Gtk.FileChooserDialog(title=title, parent=self.window, action=action)
        dialog.add_buttons("Zrušit", Gtk.ResponseType.CANCEL, "Vybrat", Gtk.ResponseType.OK)
        dialog.set_default_response(Gtk.ResponseType.OK)  # Enter (also in the location bar) accepts
        dialog.set_select_multiple(multiple)
        dialog.set_current_folder(os.path.expanduser("~"))
        chosen = dialog.get_filenames() if dialog.run() == Gtk.ResponseType.OK else []
        dialog.destroy()
        return chosen

    def upload_files(self):
        files = self.choose("Nahrát soubory na USB", Gtk.FileChooserAction.OPEN, multiple=True)
        if files:
            progress(self.window, "Kopírování na USB", ("cp", *files, f"usb:{self.path}", self.name, "--yad"),
                     lambda code: self.go(self.path))

    def upload_folder(self):
        folders = self.choose("Nahrát složku na USB", Gtk.FileChooserAction.SELECT_FOLDER)
        if folders:
            progress(self.window, "Kopírování na USB", ("cp", *folders, f"usb:{self.path}", self.name, "--yad"),
                     lambda code: self.go(self.path))

    def download(self):
        names = self.selected()
        if not names:
            self.status.set_text("Vyber soubory nebo složky ke stažení.")
            return
        target = self.choose("Kam stáhnout z USB", Gtk.FileChooserAction.SELECT_FOLDER)
        if target:
            progress(self.window, "Kopírování z USB",
                     ("cp", *[f"usb:{self.child(n)}" for n in names], target[0], self.name, "--yad"))

    def new_folder(self):
        name = ask_text(self.window, "Nová složka", f"Název nové složky v usb:{self.path}")
        if name:
            code, text = run_client("mkdir", self.name, f"usb:{self.child(name)}")
            self.status.set_text(text.removeprefix("droiddesk-usb: ") if code else "")
            self.go(self.path)

    def delete(self):
        names = self.selected()
        if not names:
            self.status.set_text("Vyber, co smazat.")
            return
        shown = ", ".join(names[:5]) + (" …" if len(names) > 5 else "")
        if confirm(self.window, "Smazat z USB?", f"{shown}\nSložky se smažou i s obsahem.", "Smazat"):
            code, text = run_client("rm", "-r", self.name, *[f"usb:{self.child(n)}" for n in names])
            self.status.set_text(text.removeprefix("droiddesk-usb: ") if code else "")
            self.go(self.path)


class Tray:
    # Columns of the device list.
    STATE, LABEL, CAPACITY, FS, FS_LABEL, ID, NAME, HELD = range(8)

    def __init__(self):
        self.window = None
        self.busy = False
        self.summaries = {}  # device name -> (capacity, filesystems, labels), held devices only
        self.activities = {}  # device name -> running operation from droiddesk-usb (status files)
        GLib.timeout_add_seconds(2, self.poll_activity)
        self.icon = Gtk.StatusIcon.new_from_icon_name(ICON)
        self.icon.set_title("USB disky")
        self.icon.set_tooltip_text("USB disky")
        self.icon.connect("activate", lambda *_: self.toggle())
        self.icon.connect("popup-menu", self.popup)
        threading.Thread(target=self.watch, daemon=True).start()

    # ── panel icon ──

    def popup(self, icon, button, when):
        menu = Gtk.Menu()
        for text, action in (("Otevřít USB disky", self.show), ("Vysunout vše", self.eject_all)):
            item = Gtk.MenuItem(label=text)
            item.connect("activate", lambda *_, a=action: a())
            menu.append(item)
        menu.show_all()
        menu.popup(None, None, Gtk.StatusIcon.position_menu, icon, button, when)

    def toggle(self):
        if self.window and self.window.get_visible():
            self.window.hide()
        else:
            self.show()

    def eject_all(self):
        def work():
            for dev in storage_devices() or []:
                if dev["held"]:
                    run_client("eject", dev["name"])
            GLib.idle_add(self.refresh)
        threading.Thread(target=work, daemon=True).start()

    # ── events from the app (droiddesk-usb watch) ──

    def watch(self):
        while True:
            try:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.connect(usb.SOCKET)
                sock.sendall(b"watch\n")
                GLib.idle_add(self.refresh)
                with sock.makefile("r", encoding="utf-8") as lines:
                    for line in lines:
                        GLib.idle_add(self.on_event, line.rstrip("\n").split("\t"))
            except OSError:
                pass
            time.sleep(5)  # the app restarted or is not running yet

    def on_event(self, fields):
        kind = fields[0]
        if kind == "attached" and len(fields) >= 7 and fields[4] == "1":
            dev = {"name": fields[1], "maker": fields[5], "product": fields[6] if len(fields) > 6 else ""}
            threading.Thread(target=self.offer_attach, args=(dev,), daemon=True).start()
        if kind in ("detached", "released") and len(fields) > 1:
            self.summaries.pop(fields[1], None)
        self.refresh()

    def offer_attach(self, dev):
        try:
            out = subprocess.run(
                ["notify-send", "-i", ICON, "-A", "attach=Připojit do Linuxu", "-A", "open=Otevřít USB disky",
                 "USB disky", f"Připojeno: {label_of(dev)}"],
                capture_output=True, text=True, timeout=300)
        except (OSError, subprocess.TimeoutExpired):
            return
        action = out.stdout.strip()
        if action == "attach":
            code, text = run_client("attach", dev["name"])
            usb.notify(text)
            GLib.idle_add(self.refresh)
        elif action == "open":
            GLib.idle_add(self.show)

    # ── window ──

    def show(self):
        if self.window is None:
            self.build_window()
        self.refresh()
        self.window.show_all()
        self.window.present()

    def build_window(self):
        win = Gtk.Window(title="USB disky")
        win.set_icon_name(ICON)
        win.set_default_size(780, 340)
        win.connect("delete-event", lambda *_: win.hide() or True)
        # A format or copy from the terminal changes what the columns show: read the disks
        # again when the window comes back to the front (at most every 5 s).
        self.last_focus = 0.0
        win.connect("focus-in-event", lambda *_: self.focused())
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8, margin=12)
        self.store = Gtk.ListStore(str, str, str, str, str, str, str, bool)
        view = Gtk.TreeView(model=self.store)
        for title, col in (("Stav", self.STATE), ("Zařízení", self.LABEL), ("Kapacita", self.CAPACITY),
                           ("Systém souborů", self.FS), ("Jmenovka", self.FS_LABEL), ("ID", self.ID),
                           ("Cesta", self.NAME)):
            column = Gtk.TreeViewColumn(title, Gtk.CellRendererText(), text=col)
            column.set_expand(col == self.LABEL)
            view.append_column(column)
        view.get_selection().connect("changed", lambda *_: self.update_buttons())
        view.connect("row-activated", lambda *_: self.files())
        self.view = view
        scroll = Gtk.ScrolledWindow(vexpand=True)
        scroll.add(view)
        box.pack_start(scroll, True, True, 0)
        self.status = Gtk.Label(xalign=0, wrap=True, selectable=True)
        box.pack_start(self.status, False, False, 0)
        buttons = Gtk.Box(spacing=6)
        self.buttons = {}
        for key, text, action in (("attach", "Připojit do Linuxu", self.attach),
                                  ("eject", "Vysunout", self.eject),
                                  ("files", "Soubory…", self.files),
                                  ("flash", "Zapsat obraz…", self.flash),
                                  ("read", "Uložit obraz…", self.read),
                                  ("format", "Naformátovat…", self.format),
                                  ("check", "Zkontrolovat", self.check),
                                  ("refresh", "Obnovit", self.reload)):
            button = Gtk.Button(label=text)
            button.connect("clicked", lambda *_, a=action: a())
            buttons.pack_start(button, False, False, 0)
            self.buttons[key] = button
        box.pack_start(buttons, False, False, 0)
        win.add(box)
        self.window = win

    def reload(self):
        self.summaries.clear()
        self.refresh()

    def focused(self):
        if not self.busy and time.monotonic() - self.last_focus > 5:
            self.last_focus = time.monotonic()
            self.reload()
        return False

    def refresh(self):
        held = 0
        found = storage_devices()
        if found is not None:
            held = sum(d["held"] for d in found)
        if not self.activities:
            self.icon.set_tooltip_text(f"USB disky — {held} připojeno do Linuxu" if held else "USB disky")
        if self.window is None:
            return False
        selected = self.selected()
        self.store.clear()
        if found is None:
            self.status.set_text("Aplikace DroidDesk neodpovídá.")
        elif not found:
            self.status.set_text("Žádná USB paměť ani čtečka karet není připojená.")
        elif not self.busy:
            self.status.set_text("")
        for dev in found or []:
            state = "v Linuxu" if dev["held"] else "Android"
            running = self.activities.get(dev["name"])
            if running:
                state = f"{state} — {running}"
            if dev["held"]:
                summary = self.summaries.get(dev["name"])
                if summary is None:
                    summary = ("…", "…", "")
                    self.summaries[dev["name"]] = summary
                    threading.Thread(target=self.summarize, args=(dev["name"],), daemon=True).start()
            else:
                summary = ("—", "připoj do Linuxu", "")
            row = self.store.append([state, label_of(dev), *summary, f"{dev['vid']}:{dev['pid']}",
                                     dev["name"], dev["held"]])
            if selected and selected[self.NAME] == dev["name"] or len(found) == 1:
                self.view.get_selection().select_iter(row)
        self.update_buttons()
        return False

    def poll_activity(self):
        """Operations of droiddesk-usb (also from a terminal): the State column, the icon's
        tooltip, and a notification when one ends."""
        import json
        folder = usb.status_dir()
        seen = {}
        try:
            names = [n for n in os.listdir(folder) if n.startswith("droiddesk-usb_") and n.endswith(".status")]
        except OSError:
            names = []
        for name in names:
            path = os.path.join(folder, name)
            try:
                with open(path, encoding="utf-8") as source:
                    state = json.load(source)
            except (OSError, ValueError):
                continue
            device = name[len("droiddesk-usb"):-len(".status")].replace("_", "/")
            operation = usb.OPERATION_NAMES.get(state.get("op"), state.get("op", ""))
            alive = True
            try:
                os.kill(int(state.get("pid", 0)), 0)
            except (OSError, ValueError):
                alive = False
            if "done" in state or not alive:
                if state.get("origin") != "tray":
                    if "done" not in state:
                        usb.notify(f"{device}: {operation} přerušeno")
                    elif state["done"] == 0:
                        usb.notify(f"{device}: {operation} — hotovo. {state.get('result', '')}".strip())
                    else:
                        usb.notify(f"{device}: {operation} — chyba: {state.get('result', '')}")
                try:
                    os.unlink(path)
                except OSError:
                    pass
                self.summaries.pop(device, None)
                continue
            percent, elapsed = state.get("percent"), float(state.get("elapsed") or 0)
            text = operation
            if percent is not None:
                text += f" {percent} %"
                if 0 < percent < 100 and elapsed > 10:
                    left = elapsed * (100 - percent) / percent
                    text += f", zbývá ~{int(left // 60)} min" if left >= 90 else f", zbývá ~{int(left)} s"
            seen[device] = text
        changed = seen != self.activities
        self.activities = seen
        if changed:
            self.refresh()
        if seen:
            self.icon.set_tooltip_text("USB disky — " + "; ".join(f"{d}: {t}" for d, t in seen.items()))
        return True

    def summarize(self, name):
        summary = disk_summary(name)

        def apply():
            self.summaries[name] = summary
            for row in self.store:
                if row[self.NAME] == name:
                    row[self.CAPACITY], row[self.FS], row[self.FS_LABEL] = summary
            return False
        GLib.idle_add(apply)

    def selected(self):
        model, row = self.view.get_selection().get_selected() if self.window else (None, None)
        return list(model[row]) if row else None

    def update_buttons(self):
        row = self.selected()
        for key, button in self.buttons.items():
            if key == "refresh":
                button.set_sensitive(True)
            elif key == "attach":
                button.set_sensitive(bool(row) and not row[self.HELD] and not self.busy)
            elif key in ("eject", "files"):
                button.set_sensitive(bool(row) and row[self.HELD] and not self.busy)
            else:
                button.set_sensitive(bool(row) and not self.busy)

    def in_background(self, args, message):
        self.busy = True
        self.status.set_text(message)
        self.update_buttons()

        def work():
            code, text = run_client(*args)
            GLib.idle_add(self.finished, text)
        threading.Thread(target=work, daemon=True).start()

    def finished(self, text):
        self.busy = False
        self.status.set_text(text.removeprefix("droiddesk-usb: "))
        self.refresh()
        return False

    def operation(self, title, args, name):
        """A long operation with a progress window; the disk summary is read again after it."""
        self.busy = True
        self.update_buttons()

        def done(code):
            self.busy = False
            self.summaries.pop(name, None)
            self.refresh()
        progress(self.window, title, args, done)

    def attach(self):
        row = self.selected()
        self.in_background(("attach", row[self.NAME]), "Připojuji… (povolení USB se může ukázat na displeji telefonu)")

    def eject(self):
        row = self.selected()
        self.in_background(("eject", row[self.NAME]), "Vysouvám…")

    def files(self):
        row = self.selected()
        if not row or not row[self.HELD]:
            self.status.set_text("Soubory: nejdřív Připojit do Linuxu.")
            return
        FilesWindow(self.window, row[self.NAME], row[self.LABEL])

    def flash(self):
        row = self.selected()
        dialog = Gtk.FileChooserDialog(title="Zapsat obraz na USB", parent=self.window,
                                       action=Gtk.FileChooserAction.OPEN)
        dialog.add_buttons("Zrušit", Gtk.ResponseType.CANCEL, "Vybrat", Gtk.ResponseType.OK)
        dialog.set_default_response(Gtk.ResponseType.OK)  # Enter (also in the location bar) accepts
        images = Gtk.FileFilter()
        images.set_name("Obrazy disků")
        for pattern in ("*.img", "*.iso", "*.raw", "*.bin", "*.xz", "*.gz"):
            images.add_pattern(pattern)
        dialog.add_filter(images)
        anything = Gtk.FileFilter()
        anything.set_name("Všechny soubory")
        anything.add_pattern("*")
        dialog.add_filter(anything)
        dialog.set_current_folder(os.path.expanduser("~"))
        path = dialog.get_filename() if dialog.run() == Gtk.ResponseType.OK else None
        dialog.destroy()
        if path and confirm(self.window, f"Přepsat {row[self.LABEL]}?",
                            f"VŠECHNA data na zařízení {row[self.LABEL]} ({row[self.NAME]}) budou přepsána "
                            f"obrazem\n{os.path.basename(path)}.", "Přepsat"):
            self.operation("Zápis obrazu", ("flash", path, row[self.NAME], "--yes", "--yad"), row[self.NAME])

    def read(self):
        row = self.selected()
        dialog = Gtk.FileChooserDialog(title="Uložit obraz USB", parent=self.window,
                                       action=Gtk.FileChooserAction.SAVE)
        dialog.add_buttons("Zrušit", Gtk.ResponseType.CANCEL, "Uložit", Gtk.ResponseType.OK)
        dialog.set_default_response(Gtk.ResponseType.OK)
        dialog.set_do_overwrite_confirmation(True)
        dialog.set_current_folder(os.path.expanduser("~"))
        name = "".join(c if c.isalnum() or c in "-_" else "-" for c in row[self.LABEL]).strip("-") or "usb"
        dialog.set_current_name(f"{name}-{time.strftime('%Y%m%d')}.img")
        path = dialog.get_filename() if dialog.run() == Gtk.ResponseType.OK else None
        dialog.destroy()
        if path:
            self.operation("Ukládání obrazu", ("read", path, row[self.NAME], "--yad"), row[self.NAME])

    def check(self):
        """Read the whole disk twice and compare: finds memory that returns unstable data."""
        row = self.selected()
        self.operation("Kontrola disku (2× čtení)", ("check", row[self.NAME], "--yad"), row[self.NAME])

    def format(self):
        row = self.selected()
        dialog = Gtk.Dialog(title="Naformátovat USB", transient_for=self.window, modal=True)
        dialog.add_buttons("Zrušit", Gtk.ResponseType.CANCEL, "Pokračovat", Gtk.ResponseType.OK)
        area = dialog.get_content_area()
        area.set_spacing(8)
        area.set_border_width(12)
        area.pack_start(Gtk.Label(label=f"{row[self.LABEL]} ({row[self.CAPACITY]})", xalign=0), False, False, 0)
        kinds = Gtk.ComboBoxText()
        for key, text in (("fat32", "FAT32 — Windows, macOS, Linux, TV, auto (soubory do 4 GB)"),
                          ("exfat", "exFAT — velké soubory nad 4 GB, Windows, macOS, Linux"),
                          ("ntfs", "NTFS — Windows (macOS jen čte)"),
                          ("ext4", "ext4 — jen Linux")):
            kinds.append(key, text)
        kinds.set_active_id("fat32")
        area.pack_start(kinds, False, False, 0)
        area.pack_start(Gtk.Label(label="Jmenovka (nepovinné)", xalign=0), False, False, 0)
        label = Gtk.Entry(max_length=15)
        area.pack_start(label, False, False, 0)
        dialog.show_all()
        answer = dialog.run()
        kind, name = kinds.get_active_id(), label.get_text().strip()
        dialog.destroy()
        if answer != Gtk.ResponseType.OK:
            return
        # Show what would be lost: filesystem, label and the top folder. Reading it takes a
        # few seconds on a slow stick: say so, and keep the window responsive meanwhile.
        self.busy = True
        self.update_buttons()
        self.status.set_text("Zjišťuji, co je na disku…")
        self.window.get_window().set_cursor(Gdk.Cursor.new_from_name(self.window.get_display(), "wait"))

        def work():
            result = client_lines("ls", row[self.NAME], "--raw", "usb:/")
            GLib.idle_add(self.format_confirm, row, kind, name, result)
        threading.Thread(target=work, daemon=True).start()

    def format_confirm(self, row, kind, name, result):
        self.window.get_window().set_cursor(None)
        self.busy = False
        self.status.set_text("")
        self.update_buttons()
        code, lines, _ = result
        now = f"Teď je na disku: {row[self.FS]}" + (f" „{row[self.FS_LABEL]}“" if row[self.FS_LABEL] else "")
        if code == 0:
            names = [line.split("\t", 2)[2] for line in lines if line.count("\t") >= 2]
            names = [n for n in names if n != "lost+found"]
            now += (f"\nV kořeni {len(names)} položek: " + ", ".join(names[:8]) + (" …" if len(names) > 8 else "")
                    if names else "\nDisk je prázdný.")
        if confirm(self.window, f"Naformátovat {row[self.LABEL]}?",
                   f"{now}\n\nVŠECHNA data na zařízení {row[self.LABEL]} ({row[self.NAME]}) budou smazána.\n"
                   "Vznikne jeden oddíl (MBR) se systémem souborů "
                   f"{usb.FS_NAMES.get(kind, kind)}.", "Naformátovat"):
            args = ["format", row[self.NAME], kind, "--yes", "--yad"] + (["--label", name] if name else [])
            self.operation("Formátování", tuple(args), row[self.NAME])
        return False


def serve(tray, server):
    while True:
        conn, _ = server.accept()
        with conn:
            if conn.recv(64).strip() == b"show":
                GLib.idle_add(tray.show)


def main(argv):
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        server.bind(INSTANCE)
    except OSError:
        # Already running: ask it to show the window.
        if "--window" in argv:
            other = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            other.connect(INSTANCE)
            other.sendall(b"show\n")
        return 0
    server.listen(4)
    tray = Tray()
    threading.Thread(target=serve, args=(tray, server), daemon=True).start()
    if "--window" in argv:
        tray.show()
    Gtk.main()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
