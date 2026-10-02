#!/usr/bin/env python3
"""droiddesk-usb: USB flash drives and card readers from DroidDesk's Linux.

Without root there is no /dev/sdX; Android hands out USB devices one at a time
after asking the user. This client talks to UsbBridge in the app.

  droiddesk-usb list
  droiddesk-usb flash IMAGE[.xz|.gz] [DEVICE] [--lun N] [--yes]   write + verify (stable)
  droiddesk-usb read FILE [DEVICE] [--lun N]                      copy the whole device
  droiddesk-usb attach [DEVICE]                         hold the device for Linux now
  droiddesk-usb eject [DEVICE]                          give the device back to Android
  droiddesk-usb watch                                   print attach/detach/hold events
  --yad                                                 progress lines for yad --progress
  droiddesk-usb exec DEVICE -- COMMAND...               raw usbfs fd for libusb
                                                        tools (experimental)

DEVICE is the name from "list" (/dev/bus/usb/...); it may be left out when
exactly one mass storage device is attached. After a read or flash the device
stays with Linux (no unmount/remount between operations) until "eject". --lun picks the slot of a
multi-slot card reader when more than one card is inserted.
"""
import os
import socket
import subprocess
import sys
import time

SOCKET = "\0droiddesk.usb"


def connect():
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.connect(SOCKET)
    except OSError:
        sys.exit("droiddesk-usb: aplikace DroidDesk neodpovídá (běží Linuxová relace?)")
    return sock


def human(size):
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if size < 1000 or unit == "TB":
            return f"{size:.1f} {unit}" if unit != "B" else f"{size} B"
        size /= 1000


def devices():
    sock = connect()
    sock.sendall(b"list\n")
    found = []
    with sock.makefile("r", encoding="utf-8") as lines:
        for line in lines:
            line = line.rstrip("\n")
            if not line:
                break
            name, vid, pid, storage, maker, product, held = (line.split("\t") + [""] * 7)[:7]
            found.append({"name": name, "vid": vid, "pid": pid, "storage": storage == "1",
                          "maker": maker, "product": product, "held": held == "1"})
    return found


def describe(dev):
    label = " ".join(x for x in (dev["maker"], dev["product"]) if x) or "neznámé zařízení"
    return f"{dev['name']}  {dev['vid']}:{dev['pid']}  {label}"


def pick(wanted):
    found = devices()
    if wanted:
        for dev in found:
            if dev["name"] == wanted:
                return dev
        sys.exit(f"droiddesk-usb: zařízení {wanted} není připojené (droiddesk-usb list)")
    storage = [d for d in found if d["storage"]]
    if len(storage) == 1:
        return storage[0]
    if not storage:
        sys.exit("droiddesk-usb: žádná USB paměť / čtečka karet není připojená")
    sys.exit("droiddesk-usb: připojeno víc USB pamětí, vyber jednu:\n  "
             + "\n  ".join(describe(d) for d in storage))


def ask(question):
    try:
        with open("/dev/tty", "r+", encoding="utf-8") as tty:
            tty.write(question)
            tty.flush()
            return tty.readline().strip()
    except OSError:
        print(question, end="", flush=True)
        return sys.stdin.readline().strip()


PERMISSION_TEXT = "Potvrď povolení USB na displeji telefonu (dialog Androidu; v DeX se může ukázat jen na telefonu)."

# --yad: progress for `yad --progress` on stdout (percent lines and "# text" lines).
YAD = False


def notify(text):
    """Desktop notification, in case the terminal is not in view."""
    try:
        subprocess.run(["notify-send", "-i", "drive-removable-media", "DroidDesk USB", text],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False, timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        pass


def transfer(command, path, dev, assume_yes, lun):
    sock = connect()
    sock.sendall(f"{command} {dev['name']} {lun if lun is not None else '-'} {path}\n".encode())
    started = time.monotonic()
    phase = "Zápis" if command == "flash" else "Čtení"
    with sock.makefile("r", encoding="utf-8") as lines:
        for line in lines:
            kind, _, rest = line.rstrip("\n").partition(" ")
            if YAD and kind in ("progress", "done", "err", "permission", "verify", "validate"):
                if kind == "progress":
                    done, total = (int(x) for x in rest.split())
                    speed = done / max(time.monotonic() - started, 0.001)
                    if total > 0:
                        print(min(99, done * 100 // total))
                    print(f"# {phase}: {human(done)}  ({human(speed)}/s)")
                elif kind == "permission":
                    print(f"# {PERMISSION_TEXT}")
                    notify(PERMISSION_TEXT)
                elif kind == "validate":
                    phase, started = "Kontrola obrazu", time.monotonic()
                elif kind == "verify":
                    phase, started = "Ověření zpětným čtením", time.monotonic()
                elif kind == "done":
                    print("100")
                    print(f"# Hotovo, SHA-256 {rest[:16]}…")
                    return 0
                elif kind == "err":
                    print(f"# Chyba: {rest}")
                    notify(f"Chyba: {rest}")
                    return 1
                continue
            if kind == "permission":
                print(PERMISSION_TEXT)
                notify(PERMISSION_TEXT)
            elif kind == "device":
                fields = rest.split()
                size = int(fields[0])
                slot = f", slot {fields[2]} z {fields[3]}" if len(fields) > 3 and fields[3] != "1" else ""
                info = f"Zařízení: {describe(dev)}, {human(size)}{slot}"
                print(f"# {info}" if YAD else f"\n{info}")
                if command == "flash" and not assume_yes:
                    answer = ask(f"VŠECHNA data na tomto zařízení ({human(size)}) budou přepsána.\n"
                                 "Pokračovat? Napiš 'ano': ")
                    if answer != "ano":
                        sock.sendall(b"stop\n")
                        sys.exit("Zrušeno, nic nebylo zapsáno.")
                sock.sendall(b"go\n")
                started = time.monotonic()
            elif kind == "progress":
                done, total = (int(x) for x in rest.split())
                speed = done / max(time.monotonic() - started, 0.001)
                share = f"{done * 100 // total:3d} % " if total > 0 else ""
                print(f"\r{phase}: {share}{human(done)}  {human(speed)}/s   ", end="", flush=True)
            elif kind == "validate":
                phase = "Kontrola obrazu"
                started = time.monotonic()
            elif kind == "verify":
                print(f"\nOvěřuji zpětným čtením…")
                phase = "Ověření"
                started = time.monotonic()
            elif kind == "done":
                print(f"\nHotovo. SHA-256: {rest}")
                return 0
            elif kind == "err":
                sys.stdout.flush()  # the progress line first, then the error
                print(f"\ndroiddesk-usb: {rest}", file=sys.stderr)
                return 1
    print("\ndroiddesk-usb: spojení s aplikací se přerušilo", file=sys.stderr)
    return 1


def raw_exec(dev, command):
    sock = connect()
    sock.sendall(f"open {dev['name']}\n".encode())
    msg, fds, _, _ = socket.recv_fds(sock, 4096, 1)
    reply = msg.decode().strip()
    if not reply.startswith("ok ") or not fds:
        sys.exit(f"droiddesk-usb: {reply.removeprefix('err ')}")
    fd = fds[0]
    interface, ep_in, ep_out = reply.split()[1:4]
    env = dict(os.environ, DROIDDESK_USB_FD=str(fd), DROIDDESK_USB_INTERFACE=interface,
               DROIDDESK_USB_EP_IN=ep_in, DROIDDESK_USB_EP_OUT=ep_out)
    # The app keeps the device open until this socket closes.
    return subprocess.run(command, env=env, pass_fds=(fd,)).returncode


def main(argv):
    # Keep stdout and stderr in order when both go to a terminal.
    sys.stdout.reconfigure(line_buffering=True)
    if argv and argv[0] == "exec":
        # Everything after "--" belongs to the child command, untouched.
        if "--" not in argv or argv.index("--") < 2:
            print(__doc__.strip(), file=sys.stderr)
            return 2
        split = argv.index("--")
        return raw_exec(pick(argv[1]), argv[split + 1:])
    lun = None
    if "--lun" in argv:
        at = argv.index("--lun")
        if at + 1 >= len(argv) or not argv[at + 1].isdigit():
            sys.exit("droiddesk-usb: --lun potřebuje číslo slotu")
        lun = int(argv[at + 1])
        argv = argv[:at] + argv[at + 2:]
    global YAD
    YAD = "--yad" in argv
    args = [a for a in argv if a not in ("--yes", "--yad")]
    assume_yes = "--yes" in argv
    if not args or args[0] in ("-h", "--help", "help"):
        print(__doc__.strip())
        return 0
    command = args[0]
    if command == "list":
        found = devices()
        if not found:
            print("Žádné USB zařízení.")
        for dev in found:
            mark = "[drženo] " if dev["held"] else ("[paměť]  " if dev["storage"] else "         ")
            print(mark + describe(dev))
        return 0
    if command == "eject":
        wanted = args[1] if len(args) > 1 else None
        if wanted is None:
            heldDevs = [d for d in devices() if d["held"]]
            if not heldDevs:
                print("Žádné zařízení není drženo.")
                return 0
            if len(heldDevs) > 1:
                sys.exit("droiddesk-usb: drženo víc zařízení, vyber jedno:\n  "
                         + "\n  ".join(describe(d) for d in heldDevs))
            wanted = heldDevs[0]["name"]
        sock = connect()
        sock.sendall(f"eject {wanted}\n".encode())
        reply = sock.makefile("r", encoding="utf-8").readline().strip()
        if reply != "ok":
            sys.exit(f"droiddesk-usb: {reply.removeprefix('err ')}")
        print(f"{wanted} vráceno Androidu, lze bezpečně odpojit.")
        return 0
    if command == "attach":
        dev = pick(args[1] if len(args) > 1 else None)
        if dev["held"]:
            print(f"{dev['name']} už je připojené do Linuxu.")
            return 0
        sock = connect()
        sock.sendall(f"attach {dev['name']}\n".encode())
        with sock.makefile("r", encoding="utf-8") as lines:
            for line in lines:
                reply = line.strip()
                if reply == "permission":
                    print(PERMISSION_TEXT)
                    notify(PERMISSION_TEXT)
                    continue
                if reply != "ok":
                    sys.exit(f"droiddesk-usb: {reply.removeprefix('err ')}")
                print(f"{describe(dev)} připojeno do Linuxu (vysunout: droiddesk-usb eject).")
                return 0
        sys.exit("droiddesk-usb: spojení s aplikací se přerušilo")
    if command == "watch":
        sock = connect()
        sock.sendall(b"watch\n")
        try:
            with sock.makefile("r", encoding="utf-8") as lines:
                for line in lines:
                    print(line.rstrip("\n"))
        except KeyboardInterrupt:
            return 0
        return 1
    if command in ("flash", "read") and len(args) >= 2:
        path = os.path.abspath(args[1])
        if command == "flash" and not os.path.isfile(path):
            sys.exit(f"droiddesk-usb: obraz {args[1]} neexistuje")
        return transfer(command, path, pick(args[2] if len(args) > 2 else None), assume_yes, lun)
    print(__doc__.strip(), file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
