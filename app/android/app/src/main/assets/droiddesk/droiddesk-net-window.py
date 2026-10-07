"""droiddesk-net-window: the "Sítě" window (networks Android sees, switched on for Linux).

Each network has a switch "Pro Linux" (its SOCKS proxy on 127.0.0.1) and the rules send
subnets through it from droiddesk-net run / the PAC file. Works through droiddesk-net.py.
"""
import importlib.util
import os
import subprocess
import sys

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
from gi.repository import GLib, Gtk  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ETH = os.path.join(HERE, "droiddesk-eth")
ETH_SOCKET = os.path.join(os.environ.get("TMPDIR", "/tmp"), "droiddesk-eth.sock")
spec = importlib.util.spec_from_file_location("droiddesk_net", os.path.join(HERE, "droiddesk-net.py"))
net = importlib.util.module_from_spec(spec)
spec.loader.exec_module(net)


def call(function, *args):
    """Runs a droiddesk-net function; its sys.exit messages become an error text."""
    try:
        return function(*args), None
    except SystemExit as error:
        return None, str(error.code) if error.code not in (None, 0) else None


class NetWindow:
    def __init__(self):
        self.win = Gtk.Window(title="Sítě")
        self.win.set_default_size(720, 460)
        self.win.set_icon_name("network-wired")
        self.win.connect("destroy", Gtk.main_quit)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8, margin=12)
        self.win.add(box)

        intro = Gtk.Label(xalign=0, wrap=True)
        intro.set_markup("Sítě, které Android vidí. Zapnutá síť je pro Linux dostupná přes proxy, "
                         "i když výchozí zůstávají mobilní data.")
        box.pack_start(intro, False, False, 0)

        box.pack_start(self.heading("Sítě"), False, False, 0)
        self.networks = Gtk.ListBox(selection_mode=Gtk.SelectionMode.NONE)
        box.pack_start(self.framed(self.networks), False, False, 0)

        box.pack_start(self.heading("Pravidla (podsíť → síť)"), False, False, 0)
        self.rules = Gtk.ListBox(selection_mode=Gtk.SelectionMode.NONE)
        box.pack_start(self.framed(self.rules), False, False, 0)

        add = Gtk.Box(spacing=6)
        self.cidr = Gtk.Entry(placeholder_text="10.227.13.0/24")
        self.cidr.set_width_chars(20)
        self.iface = Gtk.ComboBoxText()
        button = Gtk.Button(label="Přidat pravidlo")
        button.connect("clicked", self.add_rule)
        self.cidr.connect("activate", self.add_rule)
        add.pack_start(self.cidr, False, False, 0)
        add.pack_start(Gtk.Label(label="→"), False, False, 0)
        add.pack_start(self.iface, False, False, 0)
        add.pack_start(button, False, False, 0)
        box.pack_start(add, False, False, 0)

        self.message = Gtk.Label(xalign=0, wrap=True, selectable=True)
        box.pack_start(self.message, False, False, 0)

        # The second mode: the adapter leaves Android and belongs to Linux (droiddesk-eth).
        box.pack_start(self.heading("Diagnostika Ethernetu (převzetí do Linuxu)"), False, False, 0)
        eth = Gtk.Box(spacing=8)
        self.eth_state = Gtk.Label(xalign=0, wrap=True, hexpand=True, selectable=True)
        self.eth_take = Gtk.Button(label="Převzít do Linuxu")
        self.eth_take.connect("clicked", self.eth_start)
        self.eth_back = Gtk.Button(label="Vrátit Androidu")
        self.eth_back.connect("clicked", self.eth_stop)
        eth.pack_start(self.eth_state, True, True, 0)
        eth.pack_start(self.eth_take, False, False, 0)
        eth.pack_start(self.eth_back, False, False, 0)
        box.pack_start(eth, False, False, 0)

        hint = Gtk.Label(xalign=0, wrap=True, selectable=True)
        hint.set_markup(
            "<small>Terminál: <tt>droiddesk-net run curl -k https://10.227.13.10</tt> · "
            "<tt>droiddesk-net ping 10.227.13.10</tt> · "
            "UDP: <tt>droiddesk-net forward udp 1502 10.227.13.12 502</tt>\n"
            "Firefox: <tt>droiddesk-net pac</tt> vytvoří PAC soubor podle pravidel. "
            "Proxy: 1080 router podle pravidel, 1081 Ethernet, 1082 Wi-Fi.</small>")
        box.pack_end(hint, False, False, 0)

        self.signature = None
        self.refresh()
        GLib.timeout_add_seconds(3, self.refresh)
        self.win.show_all()

    @staticmethod
    def heading(text):
        label = Gtk.Label(xalign=0)
        label.set_markup(f"<b>{GLib.markup_escape_text(text)}</b>")
        return label

    @staticmethod
    def framed(child):
        frame = Gtk.Frame()
        frame.add(child)
        return frame

    def show_error(self, text):
        self.message.set_markup(f'<span foreground="#c62828">{GLib.markup_escape_text(text)}</span>' if text else "")

    def refresh_eth(self):
        # Asks the daemon only when its socket exists: the wrapper would download the
        # program on first use.
        running = os.path.exists(ETH_SOCKET) and subprocess.run(
            [ETH, "status"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5).returncode == 0
        self.eth_state.set_text(
            "Adaptér patří Linuxu, Android teď Ethernet nemá. Příkazy: droiddesk-eth arpscan, watch, capture, ip add…"
            if running else
            "Celý adaptér pro Linux: odposlech (tcpdump), ARP scan, DHCP, adresa v cizí podsíti. Android mezitím Ethernet nemá.")
        self.eth_take.set_sensitive(not running)
        self.eth_back.set_sensitive(running)

    def eth_start(self, _button):
        # In a terminal: the first start downloads the program and the phone asks for USB access.
        subprocess.Popen(["xfce4-terminal", "--title=Ethernet diagnostika", "-x", "bash", "-c",
                          f"'{ETH}' start && '{ETH}' status; echo; "
                          "echo 'Příkazy: droiddesk-eth --help (arpscan, watch, capture, ip add, dhcp, run…)'; exec bash"])

    def eth_stop(self, _button):
        result = subprocess.run([ETH, "stop"], capture_output=True, text=True, timeout=15)
        self.show_error(result.stderr.strip() if result.returncode else "")
        self.refresh_eth()

    def refresh(self):
        try:
            self.refresh_eth()
        except (OSError, subprocess.SubprocessError):
            pass
        result, error = call(net.read_status)
        if error:
            self.show_error(error)
            return True
        nets, router, rules = result
        # Rebuild only when something changed, so a click is not lost to a rebuild.
        signature = repr((nets, router, rules))
        if signature == self.signature:
            return True
        self.signature = signature
        for row in self.networks.get_children():
            self.networks.remove(row)
        for item in nets:
            self.networks.add(self.network_row(item))
        if not nets:
            self.networks.add(Gtk.Label(label="Android nehlásí žádnou síť.", margin=8))
        for row in self.rules.get_children():
            self.rules.remove(row)
        for cidr, iface in rules:
            self.rules.add(self.rule_row(cidr, iface))
        if not rules:
            self.rules.add(Gtk.Label(label="Žádná pravidla, vše jde výchozí sítí.", margin=8))
        chosen = self.iface.get_active_text()
        self.iface.remove_all()
        names = [item["iface"] for item in nets]
        for name in names:
            self.iface.append_text(name)
        preferred = chosen if chosen in names else next(
            (item["iface"] for item in nets if item["transport"] == "ethernet"), names[0] if names else None)
        if preferred:
            self.iface.set_active(names.index(preferred))
        self.networks.show_all()
        self.rules.show_all()
        return True

    def network_row(self, item):
        row = Gtk.Box(spacing=10, margin=6)
        notes = [net.TRANSPORTS.get(item["transport"], item["transport"])]
        if item["default"]:
            notes.append("výchozí")
        if not item["validated"] and item["transport"] != "-":
            notes.append("bez internetu")
        label = Gtk.Label(xalign=0, hexpand=True, selectable=True)
        label.set_markup(f"<b>{GLib.markup_escape_text(item['iface'])}</b>  "
                         f"{GLib.markup_escape_text(item['addresses'] or '–')}\n"
                         f"<small>{GLib.markup_escape_text(', '.join(notes))}</small>")
        row.pack_start(label, True, True, 0)
        if item["enabled"]:
            state = net.STATES.get(item["state"], item["state"])
            row.pack_start(Gtk.Label(label=f"127.0.0.1:{item['port']} ({state})"), False, False, 0)
        row.pack_start(Gtk.Label(label="Pro Linux"), False, False, 0)
        switch = Gtk.Switch(active=item["enabled"], valign=Gtk.Align.CENTER)
        switch.connect("notify::active", self.toggle, item["iface"])
        row.pack_start(switch, False, False, 0)
        return row

    def rule_row(self, cidr, iface):
        row = Gtk.Box(spacing=10, margin=6)
        row.pack_start(Gtk.Label(label=f"{cidr}  →  {iface}", xalign=0, hexpand=True), True, True, 0)
        remove = Gtk.Button(label="Odebrat")
        remove.connect("clicked", lambda _: self.after(call(net.simple, f"rule-del {cidr}")))
        row.pack_start(remove, False, False, 0)
        return row

    def toggle(self, switch, _param, iface):
        command = "enable" if switch.get_active() else "disable"
        self.after(call(net.simple, f"{command} {iface}"))

    def add_rule(self, _widget):
        cidr = self.cidr.get_text().strip()
        iface = self.iface.get_active_text()
        if not cidr or not iface:
            self.show_error("Zadej podsíť (např. 10.227.13.0/24) a síť.")
            return
        _, error = call(net.simple, f"rule-add {cidr} {iface}")
        if not error:
            self.cidr.set_text("")
        self.after((None, error))

    def after(self, result):
        self.show_error(result[1])
        self.signature = None
        self.refresh()


if __name__ == "__main__":
    NetWindow()
    Gtk.main()
