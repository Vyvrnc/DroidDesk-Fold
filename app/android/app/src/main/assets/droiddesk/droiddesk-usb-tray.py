"""droiddesk-usb-tray — the "USB disky" icon in the XFCE panel.

  droiddesk-usb-tray            icon in the panel (systray plugin), started by autostart
  droiddesk-usb-tray --window   open the window (starts the icon too when it is not running)

The window lists USB flash drives and card readers with buttons Připojit do Linuxu / Vysunout /
Zapsat obraz… / Uložit obraz…; the work is done by droiddesk-usb (same folder), progress comes
from its --yad lines. A newly plugged drive shows a notification with "Připojit do Linuxu".
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
    from gi.repository import GLib, Gtk
except (ImportError, ValueError, AttributeError):
    # Installs from before fold.16 lack pygobject. at-spi2 leaves gi/overrides/ behind,
    # so "import gi" can succeed as an empty namespace package (AttributeError). From the menu: install it in a
    # terminal the user can see, then open the window; at autostart: stay quiet.
    if "--window" in sys.argv:
        me = os.path.realpath(sys.argv[0])
        subprocess.Popen(["xfce4-terminal", "--title=USB disky", "-x", "bash", "-c",
                          "echo 'USB disky potřebují balíček pygobject, instaluji…'; "
                          f"pkg install -y pygobject && (setsid {sys.executable} '{me}' --window &); "
                          "echo; read -rp 'Enter zavře okno. '"])
    sys.exit(0)

warnings.filterwarnings("ignore", category=DeprecationWarning)  # Gtk.StatusIcon

HERE = os.path.dirname(os.path.abspath(__file__))
CLIENT = os.path.join(HERE, "droiddesk-usb.py")
INSTANCE = "\0droiddesk.usb-tray"
ICON = "drive-removable-media"
ENV = dict(os.environ, PYTHONUTF8="1")

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


class Tray:
    def __init__(self):
        self.window = None
        self.busy = False
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
        win.set_default_size(620, 320)
        win.connect("delete-event", lambda *_: win.hide() or True)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8, margin=12)
        # state, label, vid:pid, name, held
        self.store = Gtk.ListStore(str, str, str, str, bool)
        view = Gtk.TreeView(model=self.store)
        for title, col in (("Stav", 0), ("Zařízení", 1), ("ID", 2), ("Cesta", 3)):
            column = Gtk.TreeViewColumn(title, Gtk.CellRendererText(), text=col)
            column.set_expand(col == 1)
            view.append_column(column)
        view.get_selection().connect("changed", lambda *_: self.update_buttons())
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
                                  ("flash", "Zapsat obraz…", self.flash),
                                  ("read", "Uložit obraz…", self.read),
                                  ("refresh", "Obnovit", self.refresh)):
            button = Gtk.Button(label=text)
            button.connect("clicked", lambda *_, a=action: a())
            buttons.pack_start(button, False, False, 0)
            self.buttons[key] = button
        box.pack_start(buttons, False, False, 0)
        win.add(box)
        self.window = win

    def refresh(self):
        held = 0
        found = storage_devices()
        if found is not None:
            held = sum(d["held"] for d in found)
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
            row = self.store.append([state, label_of(dev), f"{dev['vid']}:{dev['pid']}", dev["name"], dev["held"]])
            if selected and selected[3] == dev["name"] or len(found) == 1:
                self.view.get_selection().select_iter(row)
        self.update_buttons()
        return False

    def selected(self):
        model, row = self.view.get_selection().get_selected() if self.window else (None, None)
        return list(model[row]) if row else None

    def update_buttons(self):
        row = self.selected()
        for key, button in self.buttons.items():
            if key == "refresh":
                button.set_sensitive(True)
            elif key == "attach":
                button.set_sensitive(bool(row) and not row[4] and not self.busy)
            elif key == "eject":
                button.set_sensitive(bool(row) and row[4] and not self.busy)
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

    def attach(self):
        row = self.selected()
        self.in_background(("attach", row[3]), "Připojuji… (povolení USB se může ukázat na displeji telefonu)")

    def eject(self):
        row = self.selected()
        self.in_background(("eject", row[3]), "Vysouvám…")

    def flash(self):
        row = self.selected()
        dialog = Gtk.FileChooserDialog(title="Zapsat obraz na USB", parent=self.window,
                                       action=Gtk.FileChooserAction.OPEN)
        dialog.add_buttons("Zrušit", Gtk.ResponseType.CANCEL, "Vybrat", Gtk.ResponseType.OK)
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
        if not path:
            return
        confirm = Gtk.MessageDialog(parent=self.window, modal=True, message_type=Gtk.MessageType.WARNING,
                                    buttons=Gtk.ButtonsType.NONE,
                                    text=f"Přepsat {row[1]}?")
        confirm.format_secondary_text(f"VŠECHNA data na zařízení {row[1]} ({row[3]}) budou přepsána obrazem\n"
                                      f"{os.path.basename(path)}.")
        confirm.add_buttons("Zrušit", Gtk.ResponseType.CANCEL, "Přepsat", Gtk.ResponseType.OK)
        answer = confirm.run()
        confirm.destroy()
        if answer == Gtk.ResponseType.OK:
            self.progress("Zápis obrazu", ("flash", path, row[3], "--yes", "--yad"))

    def read(self):
        row = self.selected()
        dialog = Gtk.FileChooserDialog(title="Uložit obraz USB", parent=self.window,
                                       action=Gtk.FileChooserAction.SAVE)
        dialog.add_buttons("Zrušit", Gtk.ResponseType.CANCEL, "Uložit", Gtk.ResponseType.OK)
        dialog.set_do_overwrite_confirmation(True)
        dialog.set_current_folder(os.path.expanduser("~"))
        name = "".join(c if c.isalnum() or c in "-_" else "-" for c in row[1]).strip("-") or "usb"
        dialog.set_current_name(f"{name}-{time.strftime('%Y%m%d')}.img")
        path = dialog.get_filename() if dialog.run() == Gtk.ResponseType.OK else None
        dialog.destroy()
        if path:
            self.progress("Ukládání obrazu", ("read", path, row[3], "--yad"))

    def progress(self, title, args):
        dialog = Gtk.Dialog(title=title, transient_for=self.window, modal=True)
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
        self.busy = True
        self.update_buttons()

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
            self.busy = False
            if code == 0:
                bar.set_fraction(1)
            else:
                bar.set_text("Chyba")
            usb.notify(f"{title}: {'hotovo' if code == 0 else 'chyba — ' + text.get_text()}")
            self.refresh()
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
