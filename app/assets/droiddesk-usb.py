#!/usr/bin/env python3
"""droiddesk-usb: USB flash drives and card readers from DroidDesk's Linux.

Without root there is no /dev/sdX; Android hands out USB devices one at a time
after asking the user. This client talks to UsbBridge in the app.

  droiddesk-usb list
  droiddesk-usb flash IMAGE[.xz|.gz] [DEVICE] [--yes]   write + verify (stable)
  droiddesk-usb read FILE [DEVICE]                      copy the whole device
  droiddesk-usb exec DEVICE -- COMMAND...               raw usbfs fd for libusb
                                                        tools (experimental)

DEVICE is the name from "list" (/dev/bus/usb/...); it may be left out when
exactly one mass storage device is attached.
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
            name, vid, pid, storage, maker, product = (line.split("\t") + [""] * 6)[:6]
            found.append({"name": name, "vid": vid, "pid": pid, "storage": storage == "1",
                          "maker": maker, "product": product})
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


def transfer(command, path, dev, assume_yes):
    sock = connect()
    sock.sendall(f"{command} {dev['name']} {path}\n".encode())
    started = time.monotonic()
    phase = "Zápis" if command == "flash" else "Čtení"
    with sock.makefile("r", encoding="utf-8") as lines:
        for line in lines:
            kind, _, rest = line.rstrip("\n").partition(" ")
            if kind == "device":
                size = int(rest.split()[0])
                print(f"Zařízení: {describe(dev)}, {human(size)}")
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
            elif kind == "verify":
                print(f"\nOvěřuji zpětným čtením…")
                phase = "Ověření"
                started = time.monotonic()
            elif kind == "done":
                print(f"\nHotovo. SHA-256: {rest}")
                return 0
            elif kind == "err":
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
    args = [a for a in argv if a != "--yes"]
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
            print(("[paměť] " if dev["storage"] else "        ") + describe(dev))
        return 0
    if command in ("flash", "read") and len(args) >= 2:
        path = os.path.abspath(args[1])
        if command == "flash" and not os.path.isfile(path):
            sys.exit(f"droiddesk-usb: obraz {args[1]} neexistuje")
        return transfer(command, path, pick(args[2] if len(args) > 2 else None), assume_yes)
    if command == "exec" and "--" in args and args.index("--") >= 2:
        split = args.index("--")
        return raw_exec(pick(args[1]), args[split + 1:])
    print(__doc__.strip(), file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
