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
  droiddesk-usb info [DEVICE]                           capacity, partitions, filesystems, labels
  droiddesk-usb format [DEVICE] fat32|exfat|ext4 [--label NAME] [--yes]
                                                        new MBR with one partition + filesystem
  droiddesk-usb ls [DEVICE] [usb:/PATH]                 files on the disk (FAT, ext2/3/4)
  droiddesk-usb cp FILE... usb:/FOLDER                  copy to the disk (folders recursively)
  droiddesk-usb cp usb:/PATH... TARGET                  copy from the disk
  droiddesk-usb rm [-r] usb:/PATH...  |  mkdir usb:/PATH...
  --part N (partition from "info"), --raw (tab separated output)
  --yad                                                 progress lines for yad --progress
  droiddesk-usb exec DEVICE -- COMMAND...               raw usbfs fd for libusb
                                                        tools (experimental)

DEVICE is the name from "list" (/dev/bus/usb/...); it may be left out when
exactly one mass storage device is attached. After a read or flash the device
stays with Linux (no unmount/remount between operations) until "eject". --lun picks the slot of a
multi-slot card reader when more than one card is inserted.
"""
import os
import re
import socket
import struct
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


# ── Disk access for format and files (bridge "blk", libdroiddesk_blk.so) ──

# DROIDDESK_PREFIX: tests outside Termux.
PREFIX = os.environ.get("DROIDDESK_PREFIX") or os.path.dirname(os.path.dirname(os.path.realpath(sys.executable)))
SHIM = os.path.join(PREFIX, "lib", "libdroiddesk_blk.so")
VPATH = "/dev/droiddesk-blk"
MAX_REQUEST = 1 << 20
# Owner of new files and of the root folder on ext4: the usual first user of a PC.
EXT_OWNER = 1000
# Termux package of each tool, installed on first use.
PACKAGES = {"mkfs.fat": "dosfstools", "mkfs.exfat": "exfatprogs", "mkfs.ext4": "e2fsprogs",
            "debugfs": "e2fsprogs", "mdir": "mtools", "mcopy": "mtools", "mdel": "mtools",
            "mdeltree": "mtools", "mmd": "mtools", "mrd": "mtools"}
FS_NAMES = {"fat12": "FAT12", "fat16": "FAT16", "fat32": "FAT32", "exfat": "exFAT", "ntfs": "NTFS",
            "ext2": "ext2", "ext3": "ext3", "ext4": "ext4", "iso9660": "ISO 9660"}


class BlkError(Exception):
    pass


class Blk:
    """One "blk" session: block reads and writes on the held device (holds its lock)."""

    def __init__(self, dev, lun=None):
        self.sock = connect()
        self.sock.sendall(f"blk {dev['name']} {'-' if lun is None else lun}\n".encode())
        self.io = self.sock.makefile("rwb")
        while True:
            line = self.io.readline().decode("utf-8", "replace").strip()
            if line != "permission":
                break
            say(PERMISSION_TEXT)
            notify(PERMISSION_TEXT)
        if not line.startswith("device "):
            self.sock.close()
            raise BlkError(line.removeprefix("err ") or "spojení s aplikací se přerušilo")
        fields = line.split()
        self.capacity, self.bs = int(fields[1]), int(fields[2])
        self.blocks = self.capacity // self.bs

    def _request(self, op, lba, count, data=b""):
        self.io.write(op + struct.pack(">QI", lba, count) + data)
        self.io.flush()
        status = self.io.read(1)
        if not status:
            raise BlkError("spojení s aplikací se přerušilo")
        if status != b"\0":
            (length,) = struct.unpack(">H", self.io.read(2))
            raise BlkError(self.io.read(length).decode("utf-8", "replace"))
        if op == b"R":
            data = self.io.read(count * self.bs)
            if len(data) != count * self.bs:
                raise BlkError("spojení s aplikací se přerušilo")
            return data
        return b""

    def read(self, lba, count):
        step = MAX_REQUEST // self.bs
        return b"".join(self._request(b"R", lba + at, min(step, count - at)) for at in range(0, count, step))

    def write(self, lba, data):
        assert len(data) % self.bs == 0
        step = MAX_REQUEST // self.bs * self.bs
        for at in range(0, len(data), step):
            part = data[at:at + step]
            self._request(b"W", lba + at // self.bs, len(part) // self.bs, part)

    def read_bytes(self, offset, length):
        first = offset // self.bs
        last = (offset + length + self.bs - 1) // self.bs
        data = self.read(first, last - first)
        return data[offset - first * self.bs:][:length]

    def flush(self):
        self._request(b"F", 0, 0)

    def close(self):
        try:
            self.io.write(b"Q" + bytes(12))
            self.io.flush()
        except OSError:
            pass
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def say(text):
    """A status line: "# text" for yad, plain otherwise."""
    print(f"# {text}" if YAD else text)


def step(percent, text):
    if YAD:
        print(percent)
    say(text)


def _boot_fs(sector):
    """Filesystem from a boot sector / superblock area (at least 2 KiB from its start)."""
    if sector[3:11] == b"EXFAT   ":
        return "exfat"
    if sector[3:11] == b"NTFS    ":
        return "ntfs"
    if sector[0x52:0x5A] == b"FAT32   ":
        return "fat32"
    if sector[0x36:0x3B] == b"FAT16":
        return "fat16"
    if sector[0x36:0x3B] == b"FAT12":
        return "fat12"
    if len(sector) >= 2048 and sector[1024 + 0x38:1024 + 0x3A] == b"\x53\xef":
        compat, incompat = struct.unpack_from("<II", sector, 1024 + 0x5C)
        if incompat & 0x2C0:  # extents, 64bit, flex_bg
            return "ext4"
        return "ext3" if compat & 0x4 else "ext2"
    return ""


def _label(blk, start, head, fs):
    if fs == "fat32":
        raw = head[0x47:0x52]
    elif fs in ("fat16", "fat12"):
        raw = head[0x2B:0x36]
    elif fs.startswith("ext"):
        return head[1024 + 0x78:1024 + 0x88].split(b"\0")[0].decode("utf-8", "replace")
    elif fs == "exfat":
        # The label is an entry (type 0x83) of the root folder.
        sector_shift, cluster_shift = head[0x6C], head[0x6D]
        heap, root = struct.unpack_from("<I", head, 0x58)[0], struct.unpack_from("<I", head, 0x60)[0]
        at = start + ((heap + ((root - 2) << cluster_shift)) << sector_shift)
        entries = blk.read_bytes(at, 4096)
        for i in range(0, len(entries), 32):
            kind = entries[i]
            if kind == 0:
                break
            if kind == 0x83:
                count = min(entries[i + 1], 11)
                return entries[i + 2:i + 2 + 2 * count].decode("utf-16-le", "replace")
        return ""
    else:
        return ""
    text = raw.decode("latin-1").strip()
    return "" if text == "NO NAME" else text


def probe(blk, start):
    head = blk.read_bytes(start, 4096)
    fs = _boot_fs(head)
    return fs, (_label(blk, start, head, fs) if fs else "")


def disk_info(blk):
    """{"capacity", "bs", "table": mbr/gpt/none, "parts": [{number, start, size, fs, label}]}."""
    bs = blk.bs
    info = {"capacity": blk.capacity, "bs": bs, "table": "none", "parts": []}
    first = blk.read_bytes(0, 4096)
    iso = blk.read_bytes(0x8000, 2048) if blk.capacity > 0x8800 else b""
    if iso[1:6] == b"CD001":
        info["table"] = "iso"
        label = iso[40:72].decode("latin-1").strip()
        info["parts"].append({"number": 0, "start": 0, "size": blk.capacity, "fs": "iso9660", "label": label})
        return info
    fs = _boot_fs(first)
    if fs:  # a filesystem on the whole device, no partition table ("superfloppy")
        info["parts"].append({"number": 0, "start": 0, "size": blk.capacity, "fs": fs,
                              "label": _label(blk, 0, first, fs)})
        return info
    if first[510:512] != b"\x55\xaa":
        return info
    entries = [first[446 + 16 * i:462 + 16 * i] for i in range(4)]
    found = []
    if any(e[4] == 0xEE for e in entries):
        header = blk.read_bytes(bs, 512)
        if header[:8] == b"EFI PART":
            info["table"] = "gpt"
            lba, count, size = struct.unpack_from("<QII", header, 72)
            if 128 <= size <= 4096 and 0 < count <= 256:
                raw = blk.read_bytes(lba * bs, count * size)
                for i in range(count):
                    entry = raw[i * size:(i + 1) * size]
                    if entry[:16] == bytes(16):
                        continue
                    low, high = struct.unpack_from("<QQ", entry, 32)
                    found.append((i + 1, low * bs, (high - low + 1) * bs))
    else:
        info["table"] = "mbr"
        for i, entry in enumerate(entries):
            kind = entry[4]
            low, count = struct.unpack_from("<II", entry, 8)
            if kind and count and kind not in (0x05, 0x0F, 0x85):  # not extended
                found.append((i + 1, low * bs, count * bs))
    for number, start, size in found:
        if start + size > blk.capacity or start % bs:
            continue
        fs, label = probe(blk, start)
        info["parts"].append({"number": number, "start": start, "size": size, "fs": fs, "label": label})
    return info


def describe_part(part):
    name = FS_NAMES.get(part["fs"], part["fs"] or "neznámý systém")
    label = f" „{part['label']}“" if part["label"] else ""
    where = f"oddíl {part['number']}" if part["number"] else "celé zařízení"
    return f"{where}: {name}{label}, {human(part['size'])}"


def ensure_tools(*names):
    missing = sorted({PACKAGES[n] for n in names if not os.path.isfile(os.path.join(PREFIX, "bin", n))})
    if missing:
        say(f"Instaluji {' '.join(missing)}…")
        pkg = os.path.join(PREFIX, "bin", "pkg")
        result = subprocess.run([pkg, "install", "-y", *missing], stdout=subprocess.DEVNULL,
                                stderr=subprocess.PIPE, text=True)
        if result.returncode != 0 or any(not os.path.isfile(os.path.join(PREFIX, "bin", n)) for n in names):
            sys.exit(f"droiddesk-usb: chybí {' '.join(missing)}; nainstaluj v XFCE terminálu: "
                     f"pkg install {' '.join(missing)}")
    if not os.path.isfile(SHIM):
        sys.exit(f"droiddesk-usb: chybí {SHIM} (spusť znovu Linuxovou relaci DroidDesk)")


def tool_env(dev, lun, part):
    env = dict(os.environ)
    env["LD_PRELOAD"] = SHIM + (":" + env["LD_PRELOAD"] if env.get("LD_PRELOAD") else "")
    env["DROIDDESK_BLK_DEVICE"] = dev["name"]
    env["DROIDDESK_BLK_LUN"] = "-" if lun is None else str(lun)
    env["DROIDDESK_BLK_OFFSET"] = str(part["start"])
    env["DROIDDESK_BLK_SIZE"] = str(part["size"])
    return env


def run_tool(args, env, show=True, check=True, stdin=None):
    """Runs a Termux tool on the disk; output lines go to the user (or yad)."""
    args = [os.path.join(PREFIX, "bin", args[0])] + list(args[1:])
    result = subprocess.run(args, env=env, input=stdin, capture_output=True, text=True, errors="replace")
    if show:
        for line in (result.stdout + result.stderr).splitlines():
            if line.strip():
                say(line.strip())
    # A deferred write can fail after the tool already reported success.
    shim_errors = [line for line in result.stderr.splitlines() if line.startswith("droiddesk-blk: error:")]
    if check and shim_errors:
        sys.exit(f"droiddesk-usb: {shim_errors[-1].removeprefix('droiddesk-blk: error: ')}")
    if check and result.returncode != 0:
        tail = (result.stderr.strip() or result.stdout.strip()).splitlines()
        sys.exit(f"droiddesk-usb: {args[0].rsplit('/', 1)[-1]} selhal: {tail[-1] if tail else result.returncode}")
    return result


def open_blk(dev, lun):
    try:
        return Blk(dev, lun)
    except BlkError as error:
        sys.exit(f"droiddesk-usb: {error}")


def contents(dev, lun):
    """What is on the disk now, for the warning before formatting."""
    with open_blk(dev, lun) as blk:
        info = disk_info(blk)
    lines = []
    for part in info["parts"]:
        line = describe_part(part)
        if part["fs"].startswith(("fat", "ext")):
            try:
                files = (FatFiles if part["fs"].startswith("fat") else ExtFiles)(dev, lun, part)
                try:
                    names = [n + ("/" if d else "") for d, _, n in files.ls("/", missing_ok=True)
                             if n != "lost+found"]
                finally:
                    files.close()
                if names:
                    line += f"; v kořeni {len(names)} položek: " + ", ".join(names[:6]) + (" …" if len(names) > 6 else "")
                else:
                    line += "; prázdný"
            except SystemExit:
                pass
        lines.append(line)
    return lines


def format_disk(dev, lun, kind, label, assume_yes):
    kind = {"fat": "fat32", "vfat": "fat32", "fat32": "fat32", "exfat": "exfat", "ext4": "ext4"}.get(kind.lower())
    if not kind:
        sys.exit("droiddesk-usb: formát umí fat32, exfat nebo ext4")
    tool = {"fat32": "mkfs.fat", "exfat": "mkfs.exfat", "ext4": "mkfs.ext4"}[kind]
    ensure_tools(tool)
    if not assume_yes:
        for line in contents(dev, lun):
            say(f"Na disku: {line}")
    with open_blk(dev, lun) as blk:
        bs, blocks = blk.bs, blk.blocks
        mib = (1 << 20) // bs
        if blocks < 8 * mib:
            sys.exit("droiddesk-usb: zařízení je menší než 8 MB")
        if blocks - mib > 0xFFFFFFFF:
            sys.exit("droiddesk-usb: zařízení nad 2 TiB (MBR) zatím neumím")
        start, count = mib, blocks - mib
        size = count * bs
        if kind == "fat32" and size < 512 << 20:
            kind = "fat16"  # FAT32 needs at least 65525 clusters
        say(f"Zařízení: {describe(dev)}, {human(blk.capacity)} → {FS_NAMES[kind]}"
            + (f" „{label}“" if label else ""))
        if not assume_yes:
            answer = ask(f"VŠECHNA data na tomto zařízení ({human(blk.capacity)}) budou smazána.\n"
                         "Pokračovat? Napiš 'ano': ")
            if answer != "ano":
                sys.exit("Zrušeno, nic nebylo změněno.")
        try:
            step(5, "Mažu staré tabulky oddílů a podpisy systémů souborů")
            zero = bytes(1 << 20)
            blk.write(0, zero)
            blk.write(blocks - mib, zero)  # GPT backup, NTFS backup boot sector
            blk.write(start, zero)
            step(15, "Zapisuji tabulku oddílů (MBR, jeden oddíl)")
            mbr = bytearray(bs)
            mbr[440:444] = os.urandom(4)
            ptype = {"fat32": 0x0C, "fat16": 0x0E, "exfat": 0x07, "ext4": 0x83}[kind]
            mbr[446:462] = bytes([0x00, 0xFE, 0xFF, 0xFF, ptype, 0xFE, 0xFF, 0xFF]) + struct.pack("<II", start, count)
            mbr[510:512] = b"\x55\xaa"
            blk.write(0, bytes(mbr))
            blk.flush()
        except BlkError as error:
            sys.exit(f"droiddesk-usb: {error}")
    part = {"number": 1, "start": start * bs, "size": size}
    step(25, f"Vytvářím {FS_NAMES[kind]}…")
    env = tool_env(dev, lun, part)
    if kind in ("fat32", "fat16"):
        args = ["mkfs.fat", "-F", "32" if kind == "fat32" else "16", "-S", str(bs), "-h", str(start),
                "--mbr=n"]
        if label:
            args += ["-n", label.upper()[:11]]
    elif kind == "exfat":
        args = ["mkfs.exfat"] + (["-L", label[:15]] if label else [])
    else:
        args = ["mkfs.ext4", "-F", "-m", "0", "-E",
                f"nodiscard,lazy_itable_init=1,lazy_journal_init=1,root_owner={EXT_OWNER}:{EXT_OWNER}"]
        if label:
            args += ["-L", label[:16]]
    run_tool(args + [VPATH], env)
    step(95, "Kontroluji výsledek")
    with open_blk(dev, lun) as blk:
        info = disk_info(blk)
    made = next((p for p in info["parts"] if p["number"] == 1), None)
    if not made or made["fs"] != kind:
        sys.exit("droiddesk-usb: po formátování se nový systém souborů nenašel")
    step(100, f"Hotovo: {describe_part(made)}")
    return 0


# ── Files (FAT through mtools, ext2/3/4 through debugfs) ──

def pick_part(dev, lun, number):
    with open_blk(dev, lun) as blk:
        info = disk_info(blk)
    parts = info["parts"]
    if number is not None:
        part = next((p for p in parts if p["number"] == number), None)
        if not part:
            sys.exit(f"droiddesk-usb: oddíl {number} neexistuje")
    else:
        usable = [p for p in parts if p["fs"].startswith(("fat", "ext"))]
        if not usable:
            if any(p["fs"] == "exfat" for p in parts):
                sys.exit("droiddesk-usb: soubory na exFAT zatím neumím (jen formát); FAT32 a ext4 ano")
            sys.exit("droiddesk-usb: na zařízení není FAT ani ext2/3/4 (droiddesk-usb info)")
        part = usable[0]
    if part["fs"] == "exfat":
        sys.exit("droiddesk-usb: soubory na exFAT zatím neumím (jen formát); FAT32 a ext4 ano")
    if not part["fs"].startswith(("fat", "ext")):
        sys.exit(f"droiddesk-usb: {FS_NAMES.get(part['fs'], part['fs'] or 'neznámý systém')} neumím")
    return part


def usb_path(path):
    path = path.removeprefix("usb:") or "/"
    path = "/" + path.strip("/")
    if '"' in path:
        sys.exit('droiddesk-usb: názvy s " neumím')
    return path


class FatFiles:
    """FAT through mtools; the drive letter u: is /dev/droiddesk-blk."""

    def __init__(self, dev, lun, part):
        ensure_tools("mdir", "mcopy", "mdel", "mdeltree", "mmd", "mrd")
        self.env = tool_env(dev, lun, part)
        rc = os.path.join(os.environ.get("TMPDIR", "/tmp"), f"droiddesk-mtools-{os.getpid()}.rc")
        with open(rc, "w") as out:
            out.write(f'drive u: file="{VPATH}"\nmtools_skip_check=1\n')
        self.env["MTOOLSRC"] = rc
        self.rc = rc

    def close(self):
        try:
            os.unlink(self.rc)
        except OSError:
            pass

    def ls(self, path, missing_ok=False):
        """[(is_dir, size, name)]."""
        result = run_tool(["mdir", "-a", f"u:{path}"], self.env, show=False, check=not missing_ok)
        out = result.stdout if result.returncode == 0 else ""
        entries = []
        for line in out.splitlines():
            # "SHORT    EXT       123 2026-10-02  12:00  long name" or "<DIR>" instead of the size
            match = re.match(r"^(.{8}) (.{3}) +(<DIR>|\d+) +\S+ +\S+(?: {2,}(.*\S))?\s*$", line)
            if not match:
                continue
            short, ext, size, long = match.groups()
            name = long or (short.strip() + ("." + ext.strip() if ext.strip() else ""))
            if name in (".", ".."):
                continue
            entries.append((size == "<DIR>", 0 if size == "<DIR>" else int(size), name))
        return entries

    def is_dir(self, path):
        if path == "/":
            return True
        parent, name = path.rsplit("/", 1)
        return any(d and n.lower() == name.lower() for d, _, n in self.ls(parent or "/", missing_ok=True))

    def upload(self, sources, dest):
        run_tool(["mcopy", "-s", "-m", "-n", "-D", "o", *sources, f"u:{dest}/"], self.env)

    def download(self, source, dest):
        run_tool(["mcopy", "-s", "-m", "-n", f"u:{source}", dest], self.env)

    def remove(self, path, recursive):
        if self.is_dir(path):
            run_tool(["mdeltree" if recursive else "mrd", f"u:{path}"], self.env)
        else:
            run_tool(["mdel", f"u:{path}"], self.env)

    def mkdir(self, path):
        at, missing = "", False
        for part in path.strip("/").split("/"):
            at += "/" + part
            missing = missing or not self.is_dir(at)
            if missing:
                run_tool(["mmd", f"u:{at}"], self.env)


class ExtFiles:
    """ext2/3/4 through debugfs (e2fsprogs)."""

    def __init__(self, dev, lun, part):
        ensure_tools("debugfs")
        self.env = tool_env(dev, lun, part)

    def close(self):
        pass

    def debugfs(self, commands, write=False):
        """Runs commands in one session; returns (stdout, error lines)."""
        args = ["debugfs"] + (["-w"] if write else []) + ["-f", "-", VPATH]
        result = run_tool(args, self.env, show=False, check=False, stdin="\n".join(commands) + "\n")
        errors = [line for line in result.stderr.splitlines()
                  if line.strip() and not line.startswith("debugfs ")]
        if result.returncode != 0 and not errors:
            errors = [f"debugfs skončil s chybou {result.returncode}"]
        return result.stdout, errors

    def ls(self, path, missing_ok=False):
        out, errors = self.debugfs([f'ls -p "{path}"'])
        entries = []
        for line in out.splitlines():
            # /inode/mode/uid/gid/name/size/
            fields = line.split("/")
            if len(fields) < 7 or not fields[1].isdigit():
                continue
            mode, name, size = fields[2], "/".join(fields[5:-2]), fields[-2]
            if name in (".", ".."):
                continue
            is_dir = mode.startswith("04")
            entries.append((is_dir, int(size) if size.isdigit() else 0, name))
        if errors and not entries and not missing_ok:
            sys.exit(f"droiddesk-usb: {errors[-1]}")
        return entries

    def is_dir(self, path):
        if path == "/":
            return True
        parent, name = path.rsplit("/", 1)
        return any(d and n == name for d, _, n in self.ls(parent or "/", missing_ok=True))

    def _owned(self, name):
        return [f'sif "{name}" uid {EXT_OWNER}', f'sif "{name}" gid {EXT_OWNER}']

    def upload(self, sources, dest):
        commands = [f'cd "{dest}"']
        existing = {n for _, _, n in self.ls(dest)}

        def add(local, name, here_existing):
            if os.path.isdir(local):
                if name not in here_existing:
                    commands.extend([f'mkdir "{name}"', *self._owned(name)])
                commands.append(f'cd "{name}"')
                for child in sorted(os.listdir(local)):
                    add(os.path.join(local, child), child, set())
                commands.append("cd ..")
            elif os.path.isfile(local):
                if '"' in name or '"' in local:
                    sys.exit('droiddesk-usb: názvy s " neumím')
                if name in here_existing:
                    commands.append(f'rm "{name}"')
                commands.extend([f'write "{local}" "{name}"', *self._owned(name)])

        for source in sources:
            add(os.path.abspath(source), os.path.basename(os.path.abspath(source)), existing)
        _, errors = self.debugfs(commands, write=True)
        if errors:
            sys.exit("droiddesk-usb: " + "; ".join(errors[:3]))

    def download(self, source, dest):
        if self.is_dir(source):
            os.makedirs(dest, exist_ok=True)
            _, errors = self.debugfs([f'rdump "{source}" "{dest}"'])
        else:
            if os.path.isdir(dest):
                dest = os.path.join(dest, source.rsplit("/", 1)[-1])
            _, errors = self.debugfs([f'dump -p "{source}" "{dest}"'])
        if errors:
            sys.exit("droiddesk-usb: " + "; ".join(errors[:3]))

    def remove(self, path, recursive):
        commands = []

        def walk(at):
            for is_dir, _, name in self.ls(at):
                child = f"{at.rstrip('/')}/{name}"
                if is_dir:
                    walk(child)
                else:
                    commands.append(f'rm "{child}"')
            commands.append(f'rmdir "{at}"')

        if self.is_dir(path):
            if not recursive and self.ls(path):
                sys.exit("droiddesk-usb: složka není prázdná (smazat i s obsahem: rm -r)")
            walk(path)
        else:
            commands.append(f'rm "{path}"')
        _, errors = self.debugfs(commands, write=True)
        if errors:
            sys.exit("droiddesk-usb: " + "; ".join(errors[:3]))

    def mkdir(self, path):
        commands, at, missing = [], "", False
        for part in path.strip("/").split("/"):
            at += "/" + part
            # Below a missing folder everything is missing; no need to look.
            missing = missing or not self.is_dir(at)
            if missing:
                commands.extend([f'mkdir "{at}"', *self._owned(at)])
        if commands:
            _, errors = self.debugfs(commands, write=True)
            if errors:
                sys.exit("droiddesk-usb: " + "; ".join(errors[:3]))


def files_for(dev, lun, number):
    part = pick_part(dev, lun, number)
    return (FatFiles if part["fs"].startswith("fat") else ExtFiles)(dev, lun, part)


def files_command(command, args, lun, number, raw):
    device = next((a for a in args if a.startswith("/dev/bus/usb/")), None)
    args = [a for a in args if a != device]
    recursive = "-r" in args
    args = [a for a in args if a != "-r"]
    dev = pick(device)
    files = files_for(dev, lun, number)
    try:
        if command == "ls":
            path = usb_path(args[0] if args else "/")
            entries = sorted(files.ls(path), key=lambda e: (not e[0], e[2].lower()))
            for is_dir, size, name in entries:
                if raw:
                    print(f"{'d' if is_dir else 'f'}\t{size}\t{name}")
                else:
                    print(f"{'<složka>':>10}  {name}/" if is_dir else f"{human(size):>10}  {name}")
        elif command == "cp":
            if len(args) < 2:
                sys.exit("droiddesk-usb: cp ZDROJ… CÍL, jedna strana s usb:/cesta")
            *sources, dest = args
            if dest.startswith("usb:") and not any(s.startswith("usb:") for s in sources):
                for source in sources:
                    if not os.path.exists(source):
                        sys.exit(f"droiddesk-usb: {source} neexistuje")
                target = usb_path(dest)
                if target != "/":
                    files.mkdir(target)
                step(10, f"Kopíruji na USB do {target}…")
                files.upload(sources, target)
            elif all(s.startswith("usb:") for s in sources) and not dest.startswith("usb:"):
                for source in sources:
                    step(10, f"Kopíruji z USB {usb_path(source)}…")
                    files.download(usb_path(source), os.path.abspath(dest))
            else:
                sys.exit("droiddesk-usb: cp kopíruje buď na USB (cíl usb:/…), nebo z něj (zdroje usb:/…)")
            step(100, "Hotovo")
        elif command == "rm":
            for path in args:
                files.remove(usb_path(path), recursive)
        elif command == "mkdir":
            for path in args:
                files.mkdir(usb_path(path))
    finally:
        files.close()
    return 0


def info_command(args, lun, raw):
    dev = pick(args[0] if args else None)
    with open_blk(dev, lun) as blk:
        info = disk_info(blk)
    if raw:
        print(f"disk\t{info['capacity']}\t{info['bs']}\t{info['table']}")
        for p in info["parts"]:
            print(f"part\t{p['number']}\t{p['start']}\t{p['size']}\t{p['fs']}\t{p['label']}")
        return 0
    table = {"mbr": "MBR", "gpt": "GPT", "iso": "obraz ISO", "none": "bez tabulky oddílů"}[info["table"]]
    print(f"{describe(dev)}: {human(info['capacity'])}, {table}")
    for p in info["parts"]:
        print("  " + describe_part(p))
    if not info["parts"]:
        print("  žádný známý systém souborů")
    return 0


def option(argv, name, kind=str):
    """Removes "--name VALUE" from argv; returns (argv, value or None)."""
    if name not in argv:
        return argv, None
    at = argv.index(name)
    if at + 1 >= len(argv):
        sys.exit(f"droiddesk-usb: {name} potřebuje hodnotu")
    try:
        value = kind(argv[at + 1])
    except ValueError:
        sys.exit(f"droiddesk-usb: {name} potřebuje číslo")
    return argv[:at] + argv[at + 2:], value


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
    argv, part_number = option(argv, "--part", int)
    argv, label = option(argv, "--label")
    global YAD
    YAD = "--yad" in argv
    raw = "--raw" in argv
    args = [a for a in argv if a not in ("--yes", "--yad", "--raw")]
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
    if command == "info":
        return info_command(args[1:], lun, raw)
    if command == "format" and len(args) >= 2:
        # format [DEVICE] fat32|exfat|ext4
        device = next((a for a in args[1:] if a.startswith("/dev/bus/usb/")), None)
        kinds = [a for a in args[1:] if a != device]
        if len(kinds) != 1:
            sys.exit("droiddesk-usb: format [DEVICE] fat32|exfat|ext4 [--label NÁZEV]")
        return format_disk(pick(device), lun, kinds[0], label or "", assume_yes)
    if command in ("ls", "cp", "rm", "mkdir"):
        return files_command(command, args[1:], lun, part_number, raw)
    if command in ("flash", "read") and len(args) >= 2:
        path = os.path.abspath(args[1])
        if command == "flash" and not os.path.isfile(path):
            sys.exit(f"droiddesk-usb: obraz {args[1]} neexistuje")
        return transfer(command, path, pick(args[2] if len(args) > 2 else None), assume_yes, lun)
    print(__doc__.strip(), file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
