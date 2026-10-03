"""Tray without its window: the Thunar bookmark of a held disk gets the filesystem label."""
import importlib.util
import os
import sys

sys.argv = ["droiddesk-usb-tray"]
spec = importlib.util.spec_from_file_location("tray", "/src/assets/droiddesk/droiddesk-usb-tray.py")
tray_mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tray_mod)
from gi.repository import GLib, Gtk  # noqa: E402

tray = tray_mod.Tray()


def check():
    tray.refresh()
    print("window:", tray.window, flush=True)
    print("bookmarks:", open(os.path.expanduser("~/.config/gtk-3.0/bookmarks")).read(), flush=True)
    Gtk.main_quit()
    return False


GLib.timeout_add(9000, check)
Gtk.main()
