"""USB disky window with a USB serial adapter (fake_serial_bridge.py): open the port from the
panel, set a format, screenshot /t/gui_serial.png."""
import importlib.util
import sys

sys.argv = ["droiddesk-usb-tray", "--window"]
spec = importlib.util.spec_from_file_location("tray", "/src/assets/droiddesk/droiddesk-usb-tray.py")
tray_mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tray_mod)
from gi.repository import Gdk, GLib, Gtk  # noqa: E402

tray = tray_mod.Tray()
tray.show()
panel = tray.serial


def open_port():
    print("serial rows:", [list(r) for r in panel.store], flush=True)
    panel.open_port()
    return False


def set_format():
    print("after open:", panel.status.get_text(), [list(r) for r in panel.store], flush=True)
    panel.in_background(("set", panel.selected()[panel.HOST], "8E1"), "Nastavuji…", lambda _: "Formát 8E1")
    return False


def shot():
    print("after set:", panel.status.get_text(), [list(r) for r in panel.store], flush=True)
    root = Gdk.get_default_root_window()
    pb = Gdk.pixbuf_get_from_window(root, 0, 0, root.get_width(), root.get_height())
    pb.savev("/t/gui_serial.png", "png", [], [])
    tray_mod.run_serial("close", "all")
    Gtk.main_quit()
    return False


GLib.timeout_add(3000, open_port)
GLib.timeout_add(6000, set_format)
GLib.timeout_add(9000, shot)
Gtk.main()
