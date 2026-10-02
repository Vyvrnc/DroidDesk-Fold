import importlib.util
import sys

sys.argv = ["droiddesk-usb-tray", "--window"]
spec = importlib.util.spec_from_file_location("tray", "/src/assets/droiddesk/droiddesk-usb-tray.py")
tray_mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tray_mod)
from gi.repository import Gdk, GLib, Gtk  # noqa: E402

tray = tray_mod.Tray()
tray.show()
state = {}


def open_files():
    print("selected:", tray.selected(), flush=True)
    state["files"] = tray_mod.FilesWindow(tray.window, tray.selected()[tray.NAME], tray.selected()[tray.LABEL])
    return False


def enter_docs():
    w = state["files"]
    print("files status:", w.status.get_text(), [list(r) for r in w.store], flush=True)
    w.go("/docs")
    return False


def shot():
    w = state["files"]
    print("docs:", w.status.get_text(), [list(r) for r in w.store], flush=True)
    print("tray rows:", [list(r) for r in tray.store], flush=True)
    root = Gdk.get_default_root_window()
    pb = Gdk.pixbuf_get_from_window(root, 0, 0, root.get_width(), root.get_height())
    pb.savev("/t/gui.png", "png", [], [])
    Gtk.main_quit()
    return False


GLib.timeout_add(5000, open_files)
GLib.timeout_add(9000, enter_docs)
GLib.timeout_add(14000, shot)
Gtk.main()
