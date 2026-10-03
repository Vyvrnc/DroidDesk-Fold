#!/usr/bin/env python3
"""droiddesk-serial: USB serial adapters (RS232/RS485/TTL) as a serial port in DroidDesk's Linux.

Without root Linux has no driver for them; the app drives the adapter (FTDI, CP210x,
CH340/CH341/CH343/CH9102, PL2303, CDC-ACM) and this program puts a pseudo terminal in front:

  droiddesk-serial list                               adapters and open ports
  droiddesk-serial open [DEVICE] [--port N] [--name ttyUSB0] [--format 8E1] [--foreground]
                                                      open the adapter as /tmp/ttyUSB0 (prints the path)
  droiddesk-serial set PATH 8E1|auto                  fixed data bits, parity, stop bits (see below)
  droiddesk-serial close [PATH|DEVICE|all]            close it
  droiddesk-serial lines PATH [--dtr 0|1] [--rts 0|1] modem lines (a pseudo terminal has none)
  droiddesk-serial break PATH [MS]                    send a break (default 250 ms)

The path works the same in Debian and in the XFCE session (/tmp is shared). The baud rate
comes from the program using the port (picocom -b 19200, mbpoll -b, pyserial, minicom), any
rate. Linux's pseudo terminal keeps only part of the format: stop bits and odd, mark and space
parity come through, even parity only from programs that also enable parity checking (INPCK:
libmodbus, mbpoll, picocom), and the data bits never (always 8). For anything else (pymodbus
or pyserial with even parity, 7 data bits) give the format: --format 8E1 or "set PATH 7E1"
(N none, O odd, E even, M mark, S space; stop bits 1, 1.5 or 2); "auto" goes back to reading
it from the program. DTR and RTS are on after open, as on Linux. DEVICE is the name from
"list"; it may be left out when exactly one adapter is plugged in.
"""
import errno
import fcntl
import importlib.util
import json
import os
import select
import signal
import socket
import struct
import sys
import termios
import time
import tty

HERE = os.path.dirname(os.path.realpath(__file__))
_spec = importlib.util.spec_from_file_location("droiddesk_usb", os.path.join(HERE, "droiddesk-usb.py"))
usb = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(usb)

PROG = "droiddesk-serial"
TCGETS2 = 0x802C542A  # _IOR('T', 0x2A, struct termios2), same on arm64 and x86_64
CBAUD, BOTHER = 0o10017, 0o10000
CSIZE, CSTOPB, PARENB, PARODD, CMSPAR = 0o60, 0o100, 0o400, 0o1000, 0o10000000000
DATA_BITS = {0: 5, 0o20: 6, 0o40: 7, 0o60: 8}
SPEEDS = {getattr(termios, name): int(name[1:]) for name in dir(termios)
          if name.startswith("B") and name[1:].isdigit()}
DROP_NOTICE_S = 5.0
MARK_AFTER_S = 1.0


def die(message):
    sys.exit(f"{PROG}: {message}")


def tty_dir():
    """(host folder, the same folder as this process sees it) for the ttyUSB links: the
    session's TMPDIR, which is /tmp in Debian (its wrapper sets DROIDDESK_DEBIAN_TMP)."""
    debian_tmp = os.environ.get("DROIDDESK_DEBIAN_TMP")
    if debian_tmp:
        return debian_tmp.rstrip("/"), "/tmp"
    tmp = (os.environ.get("TMPDIR") or usb.status_dir()).rstrip("/")
    return tmp, tmp


def local_path(host_path):
    host, local = tty_dir()
    return local + host_path[len(host):] if host_path.startswith(host + "/") else host_path


def state_path(name):
    return os.path.join(usb.status_dir(), f"droiddesk-serial_{name}.json")


def control_address(name):
    return "\0droiddesk-serial." + name


def sessions():
    """Open ports from their state files; stale ones (process gone) are removed."""
    found = []
    folder = usb.status_dir()
    try:
        entries = sorted(os.listdir(folder))
    except OSError:
        return found
    for entry in entries:
        if not (entry.startswith("droiddesk-serial_") and entry.endswith(".json")):
            continue
        path = os.path.join(folder, entry)
        try:
            with open(path, encoding="utf-8") as source:
                state = json.load(source)
            os.kill(state["pid"], 0)
        except (OSError, ValueError, KeyError):
            try:
                os.unlink(path)
            except OSError:
                pass
            continue
        found.append(state)
    return found


def adapters():
    return [dev for dev in usb.devices() if dev.get("serial", 0) > 0]


def pick_adapter(name):
    found = adapters()
    if name:
        for dev in found:
            if dev["name"] == name:
                return dev
        die(f"{name} není podporovaný USB sériový převodník (droiddesk-serial list)")
    if not found:
        die("není připojený žádný USB sériový převodník")
    if len(found) > 1:
        die("připojeno víc převodníků, zadejte DEVICE:\n" + "\n".join("  " + usb.describe(d) for d in found))
    return found[0]


def session_for(target):
    """The open session for a path (/tmp/ttyUSB0, ttyUSB0) or a device name."""
    base = os.path.basename(target.rstrip("/")) if target else ""
    open_now = sessions()
    for state in open_now:
        if target and (state["name"] == base or state["device"] == target):
            return state
    if not target and len(open_now) == 1:
        return open_now[0]
    if not target and open_now:
        die("otevřeno víc portů, zadejte cestu: " + ", ".join(usb.shown(s["path"]) for s in open_now))
    die(f"port {target} není otevřený" if target else "žádný port není otevřený")


# ── Termios of the pseudo terminal → adapter parameters ──

FORMAT_PARITY = "NOEMS"
FORMAT_STOP = {"1": 1, "2": 2, "1.5": 3}


def parse_format(text):
    """"8E1" -> (data bits, parity, stop bits) in the bridge's codes; None for "auto"."""
    text = text.strip().upper()
    if text == "AUTO":
        return None
    if len(text) < 3 or text[0] not in "5678" or text[1] not in FORMAT_PARITY or text[2:] not in FORMAT_STOP:
        die(f"formát {text}: datové bity 5–8, parita N/O/E/M/S, stop bity 1/1.5/2, např. 8N1 nebo 8E1")
    return int(text[0]), FORMAT_PARITY.index(text[1]), FORMAT_STOP[text[2:]]


def parameters(fd, fixed=None):
    """(baud, data bits, stop bits, parity) the program set on the terminal; [fixed] (data
    bits, parity, stop bits) replaces the format. The pty driver forces CS8 and clears PARENB
    on every change, so parity is read from PARODD/CMSPAR, even parity from INPCK."""
    try:
        raw = fcntl.ioctl(fd, TCGETS2, bytes(44))
        _iflag, _oflag, cflag, _lflag = struct.unpack_from("4I", raw)
        baud = struct.unpack_from("I", raw, 40)[0]
        if not baud and (cflag & CBAUD) != BOTHER:
            baud = SPEEDS.get(cflag & CBAUD, 0)
    except OSError:
        attrs = termios.tcgetattr(fd)
        _iflag, cflag, baud = attrs[0], attrs[2], SPEEDS.get(attrs[5], 0)
    if fixed:
        data, parity, stop = fixed
        return baud, data, stop, parity
    data = DATA_BITS[cflag & CSIZE]
    stop = 2 if cflag & CSTOPB else 1
    if cflag & CMSPAR:
        parity = 3 if cflag & PARODD else 4
    elif cflag & PARODD:
        parity = 1
    elif cflag & PARENB or _iflag & termios.INPCK:
        parity = 2
    else:
        parity = 0
    return baud, data, stop, parity


def describe_parameters(params):
    baud, data, stop, parity = params
    return f"{baud} {data}{FORMAT_PARITY[parity]}{({3: '1.5'}).get(stop, stop)}"


# ── The session (daemon) ──

class Session:
    def __init__(self, dev, port, name, path, fixed=None):
        self.dev, self.port, self.name, self.path = dev, port, name, path
        self.fixed = fixed
        self.sock = None
        self.master = self.slave = None
        self.control = None
        self.params = None
        self.modem = 0
        self.dtr = self.rts = True
        self.buffer = b""
        self.dropped = 0
        self.last_drop_notice = 0.0
        self.driver = ""
        self.attrs = None
        self.attrs_since = 0.0
        self.traffic_at = 0.0

    def frame(self, kind, payload=b""):
        for at in range(0, max(len(payload), 1), 0xFFFF):
            chunk = payload[at:at + 0xFFFF]
            self.sock.sendall(kind + struct.pack(">H", len(chunk)) + chunk)

    def connect(self):
        self.sock = usb.connect()
        self.sock.sendall(f"serial {self.dev['name']} {self.port}\n".encode())
        while True:
            line = b""
            while not line.endswith(b"\n"):
                byte = self.sock.recv(1)
                if not byte:
                    die("aplikace spojení ukončila")
                line += byte
            text = line.decode("utf-8", "replace").strip()
            if text == "permission":
                sys.stderr.write(f"{PROG}: potvrďte přístup k převodníku v dialogu na telefonu\n")
                continue
            if text.startswith("ok"):
                parts = text.split()
                self.driver = parts[1] if len(parts) > 1 else ""
                return
            die(text.removeprefix("err ") or "neznámá chyba")

    def reserve(self, auto):
        """Takes the name through the control socket's abstract address (atomic), so a
        second open can never replace the link of a live port. [auto]: try the next name."""
        while True:
            control = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                control.bind(control_address(self.name))
                control.listen(4)
                self.control = control
                return
            except OSError:
                control.close()
                if not auto:
                    die(f"{self.name} už používá jiný otevřený port (droiddesk-serial list)")
                number = int(self.name[len("ttyUSB"):]) + 1
                if number >= 64:
                    die("žádné volné jméno ttyUSB0–63")
                self.name = f"ttyUSB{number}"
                self.path = os.path.join(os.path.dirname(self.path), self.name)

    def open_terminal(self):
        self.master, self.slave = os.openpty()
        # Raw from the start: a cooked terminal would echo the adapter's bytes back to it
        # before a program opens the port. The slave stays open here so the terminal keeps
        # its settings between programs and the master never sees a hangup.
        tty.setraw(self.slave)
        attrs = termios.tcgetattr(self.slave)
        attrs[1] = 0  # setraw leaves ONLCR and delays in oflag
        attrs[4] = attrs[5] = termios.B9600
        termios.tcsetattr(self.slave, termios.TCSANOW, attrs)
        os.set_blocking(self.master, False)
        link = local_path(self.path)
        try:
            if os.path.islink(link):
                os.unlink(link)
        except OSError:
            pass
        os.symlink(os.ttyname(self.slave), link)

    def write_state(self):
        state = {"pid": os.getpid(), "name": self.name, "path": self.path, "device": self.dev["name"],
                 "port": self.port, "driver": self.driver, "label": usb.describe(self.dev),
                 "params": describe_parameters(self.params) if self.params else "",
                 "fixed": bool(self.fixed),
                 "dtr": self.dtr, "rts": self.rts, "modem": self.modem, "dropped": self.dropped}
        tmp = state_path(self.name) + ".tmp"
        with open(tmp, "w", encoding="utf-8") as out:
            json.dump(state, out)
        os.replace(tmp, state_path(self.name))

    def cleanup(self):
        link = local_path(self.path)
        try:
            if os.path.islink(link) and self.slave is not None and os.readlink(link) == os.ttyname(self.slave):
                os.unlink(link)
        except OSError:
            pass
        try:
            os.unlink(state_path(self.name))
        except OSError:
            pass

    def mark_terminal(self):
        """glibc (2.41, Debian) reads the terminal back after tcsetattr and fails with EINVAL
        when none of the requested changes took effect; on a pty a change of only the parity
        or data bits never does (see parameters), so pymodbus opening with even parity at the
        same baud rate failed. OPOST is switched back on once the settings are stable: every
        program clears it (cfmakeraw, pyserial, libmodbus), so its tcsetattr always changes
        something. Only when the output flags are otherwise all off: OPOST alone leaves the
        bytes as they are (with ONLCR it would turn \n into \r\n).

        The write races with a program changing the settings at the same instant (its change
        would be lost), so it happens once per change, after a second without changes and
        without traffic, from a fresh read: the window is microseconds once a second at most."""
        attrs = termios.tcgetattr(self.slave)
        now = time.monotonic()
        if attrs != self.attrs:
            self.attrs, self.attrs_since = attrs, now
        elif attrs[1] == 0 and now - max(self.attrs_since, self.traffic_at) > MARK_AFTER_S:
            fresh = termios.tcgetattr(self.slave)
            if fresh == attrs:
                fresh[1] = termios.OPOST
                termios.tcsetattr(self.slave, termios.TCSANOW, fresh)
            self.attrs = termios.tcgetattr(self.slave)
            self.attrs_since = now

    def sync_parameters(self):
        params = parameters(self.slave, self.fixed)
        if params != self.params and params[0] > 0:
            self.params = params
            self.frame(b"P", struct.pack(">IBBB", *params))
            self.write_state()

    def set_lines(self, dtr=None, rts=None):
        if dtr is not None:
            self.dtr = dtr
        if rts is not None:
            self.rts = rts
        self.frame(b"M", bytes([int(self.dtr), int(self.rts)]))
        self.write_state()

    def from_adapter(self, data):
        try:
            written = os.write(self.master, data)
        except BlockingIOError:
            written = 0
        except OSError as error:
            if error.errno != errno.EIO:
                raise
            written = 0
        if written < len(data):
            # Nobody reads the port and the terminal's buffer is full: like a UART, drop.
            self.dropped += len(data) - written
            now = time.monotonic()
            if now - self.last_drop_notice > DROP_NOTICE_S:
                self.last_drop_notice = now
                self.write_state()

    def handle_frames(self):
        data = self.sock.recv(65536)
        if not data:
            return False
        self.buffer += data
        while len(self.buffer) >= 3:
            kind = self.buffer[:1]
            length = struct.unpack(">H", self.buffer[1:3])[0]
            if len(self.buffer) < 3 + length:
                break
            payload, self.buffer = self.buffer[3:3 + length], self.buffer[3 + length:]
            if kind == b"D":
                self.from_adapter(payload)
            elif kind == b"S" and payload:
                self.modem = payload[0]
                self.write_state()
            elif kind == b"E":
                sys.stderr.write(f"{PROG}: {self.name}: {payload.decode('utf-8', 'replace')}\n")
        return True

    def handle_control(self):
        client, _ = self.control.accept()
        with client:
            client.settimeout(2)
            try:
                request = client.recv(256).decode("utf-8", "replace").split()
            except OSError:
                return
            _pid, uid, _gid = struct.unpack("3i", client.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED,
                                                                    struct.calcsize("3i")))
            if uid != os.getuid():
                return
            reply = "ok"
            try:
                if request[:1] == ["lines"]:
                    values = dict(item.split("=", 1) for item in request[1:])
                    self.set_lines(dtr=values["dtr"] == "1" if "dtr" in values else None,
                                   rts=values["rts"] == "1" if "rts" in values else None)
                elif request[:1] == ["break"]:
                    ms = int(request[1]) if len(request) > 1 else 250
                    self.frame(b"B", struct.pack(">H", max(1, min(ms, 5000))))
                elif request[:1] == ["set"] and len(request) == 2:
                    self.fixed = parse_format(request[1])
                    self.params = None  # sent again by sync_parameters
                    self.sync_parameters()
                    self.write_state()
                elif request[:1] == ["close"]:
                    client.sendall(b"ok\n")
                    raise SystemExit(0)
                else:
                    reply = "err unknown request"
            except (ValueError, KeyError, IndexError):
                reply = "err bad request"
            client.sendall((reply + "\n").encode())

    def run(self, ready=None, on_ready=None, auto_name=False):
        self.reserve(auto_name)
        self.connect()
        try:
            self.open_terminal()
            self.sync_parameters()
            self.set_lines(True, True)
            self.write_state()
            if ready is not None:
                os.write(ready, (self.path + "\n").encode())
                os.close(ready)
            if on_ready is not None:
                on_ready()
            while True:
                readable, _, _ = select.select([self.master, self.sock, self.control], [], [], 0.1)
                if readable:
                    self.traffic_at = time.monotonic()
                if self.sock in readable and not self.handle_frames():
                    sys.stderr.write(f"{PROG}: {self.name}: převodník odpojen nebo aplikace skončila\n")
                    return
                if self.master in readable:
                    try:
                        data = os.read(self.master, 65536)
                    except (BlockingIOError, InterruptedError):
                        data = None
                    if data:
                        # Settings first: a program sets the baud rate, then writes.
                        self.sync_parameters()
                        self.frame(b"D", data)
                if self.control in readable:
                    self.handle_control()
                self.sync_parameters()
                self.mark_terminal()
        finally:
            self.cleanup()


def free_name():
    taken = {s["name"] for s in sessions()}
    folder = tty_dir()[1]
    for n in range(64):
        name = f"ttyUSB{n}"
        path = os.path.join(folder, name)
        if name not in taken and not os.path.lexists(path):
            return name
        if name not in taken and os.path.islink(path) and not os.path.exists(path):
            return name  # left over from a session that was killed
    die("žádné volné jméno ttyUSB0–63")


def cmd_open(args):
    device = port = name = fixed = None
    foreground = False
    rest = list(args)
    while rest:
        arg = rest.pop(0)
        if arg == "--port":
            port = int(rest.pop(0))
        elif arg == "--name":
            name = rest.pop(0)
        elif arg == "--format":
            fixed = parse_format(rest.pop(0) if rest else "")
        elif arg == "--foreground":
            foreground = True
        elif arg.startswith("-"):
            die(f"neznámá volba {arg}")
        else:
            device = arg
    dev = pick_adapter(device)
    port = port or 0
    if port >= dev["serial"]:
        die(f"převodník má porty 0–{dev['serial'] - 1}")
    for state in sessions():
        if state["device"] == dev["name"] and state["port"] == port:
            if fixed:
                control(state, "set " + "%d%s%s" % (fixed[0], FORMAT_PARITY[fixed[1]],
                                                    {3: "1.5"}.get(fixed[2], fixed[2])))
            print(usb.shown(state["path"]))
            return
    auto_name = not name
    name = name or free_name()
    if "/" in name or not name:
        die("--name je jméno bez cesty, např. ttyUSB0")
    if not auto_name and any(st["name"] == name for st in sessions()):
        die(f"{name} už používá jiný otevřený port (droiddesk-serial list)")
    session = Session(dev, port, name, os.path.join(tty_dir()[0], name), fixed)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    signal.signal(signal.SIGHUP, signal.SIG_IGN)
    if foreground:
        try:
            session.run(on_ready=lambda: print(usb.shown(session.path), flush=True), auto_name=auto_name)
        except KeyboardInterrupt:
            pass
        return
    read_end, write_end = os.pipe()
    if os.fork():
        os.close(write_end)
        with os.fdopen(read_end) as ready:
            path = ready.readline().strip()
        if not path:
            sys.exit(1)  # the daemon printed why
        print(usb.shown(path))
        return
    os.close(read_end)
    os.setsid()
    devnull = os.open(os.devnull, os.O_RDWR)
    os.dup2(devnull, 0)
    os.dup2(devnull, 1)

    def detach():
        # Errors before this point (permission, no such port) went to the caller's terminal.
        log = os.open(os.path.join(usb.status_dir(), f"droiddesk-serial_{name}.log"),
                      os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        os.dup2(log, 2)
        os.close(log)
    session.run(ready=write_end, on_ready=detach, auto_name=auto_name)


def control(state, request):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.connect(control_address(state["name"]))
        sock.sendall(request.encode())
        reply = sock.makefile("r").readline().strip()
    except OSError as error:
        die(f"{state['name']} neodpovídá: {error}")
    if reply != "ok":
        die(reply.removeprefix("err "))


def cmd_close(args):
    targets = sessions() if args[:1] == ["all"] else [session_for(args[0] if args else None)]
    for state in targets:
        control(state, "close")
        for _ in range(30):
            try:
                os.kill(state["pid"], 0)
            except OSError:
                break
            time.sleep(0.1)
        print(f"{usb.shown(state['path'])} zavřen")


def cmd_list(args):
    open_now = {(s["device"], s["port"]): s for s in sessions()}
    found = adapters()
    if not found:
        print("Žádný USB sériový převodník (FTDI, CP210x, CH34x, PL2303, CDC-ACM) není připojený.")
    for dev in found:
        print(usb.describe(dev) + (f"  ({dev['serial']} porty)" if dev["serial"] > 1 else ""))
        for port in range(dev["serial"]):
            state = open_now.get((dev["name"], port))
            if state:
                lines = f"DTR {'1' if state['dtr'] else '0'} RTS {'1' if state['rts'] else '0'}"
                extra = f", zahozeno {state['dropped']} B (nikdo nečetl)" if state.get("dropped") else ""
                fmt = state["params"] + (" (pevný formát)" if state.get("fixed") else "")
                print(f"  port {port}: {usb.shown(state['path'])}  {state['driver']}  {fmt}  {lines}{extra}")
            elif dev["serial"] > 1:
                print(f"  port {port}: zavřený")


def cmd_lines(args):
    target = None
    values = []
    rest = list(args)
    while rest:
        arg = rest.pop(0)
        if arg in ("--dtr", "--rts"):
            value = rest.pop(0) if rest else ""
            if value not in ("0", "1"):
                die(f"{arg} 0|1")
            values.append(f"{arg[2:]}={value}")
        else:
            target = arg
    if not values:
        die("zadejte --dtr 0|1 a/nebo --rts 0|1")
    control(session_for(target), "lines " + " ".join(values))


def cmd_set(args):
    if len(args) != 2:
        die("použití: droiddesk-serial set PATH 8E1|auto")
    parse_format(args[1])
    control(session_for(args[0]), "set " + args[1])


def cmd_break(args):
    target = args[0] if args else None
    ms = int(args[1]) if len(args) > 1 else 250
    control(session_for(target), f"break {ms}")


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help", "help"):
        print(__doc__.strip())
        return
    commands = {"list": cmd_list, "open": cmd_open, "close": cmd_close, "lines": cmd_lines, "break": cmd_break,
                "set": cmd_set}
    if args[0] not in commands:
        die(f"neznámý příkaz {args[0]} (droiddesk-serial --help)")
    commands[args[0]](args[1:])


if __name__ == "__main__":
    main()
