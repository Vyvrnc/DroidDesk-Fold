"""Fake UsbBridge for the "eth" request: answers like the app and relays frames to a TAP.

Proves the app-socket protocol end to end: `list` with the Ethernet-mode field, the
`permission` line before `ok`, and the 2-byte big-endian length framing (frames towards the
daemon are batched into one write, so the reader must split them itself).
FAKE_LIST=one|none|two picks how many adapters `list` offers.
"""
import fcntl
import os
import select
import socket
import struct
import sys
import time

TAP = sys.argv[1]
MAC = "02:00:00:00:00:01"
DEVICE = "/dev/bus/usb/002/003"
LISTS = {
    "one": [f"/dev/bus/usb/001/002\t0781\t5581\t1\tSanDisk\tUltra\t0\tAA01\t",
            f"{DEVICE}\t0bda\t8153\t0\tRealtek\tUSB 10/100/1000 LAN\t0\t000001\tecm"],
    "none": ["/dev/bus/usb/002/004\t0b95\t1790\t0\tASIX\tAX88179\t0\t0001\t"],
    "two": [f"{DEVICE}\t0bda\t8153\t0\tRealtek\tUSB 10/100/1000 LAN\t0\t1\tecm",
            "/dev/bus/usb/002/005\t0bda\t8156\t0\tRealtek\tUSB 2.5G LAN\t0\t2\tecm"],
}


def open_tap(name):
    fd = os.open("/dev/net/tun", os.O_RDWR)
    fcntl.ioctl(fd, 0x400454CA, struct.pack("16sH", name.encode(), 0x0002 | 0x1000))
    os.set_blocking(fd, False)
    return fd


def read_line(conn):
    line = b""
    while not line.endswith(b"\n"):
        c = conn.recv(1)
        if not c:
            return None
        line += c
    return line.decode().strip()


def session(conn):
    request = read_line(conn)
    print("request:", request, flush=True)
    if request == "list":
        lines = LISTS[os.environ.get("FAKE_LIST", "one")]
        conn.sendall(("\n".join(lines) + "\n\n").encode())
        return
    if request != "eth " + DEVICE:
        conn.sendall(b"err no such device\n")
        return
    conn.sendall(b"permission\n")
    time.sleep(1)  # the user taps "Allow"
    conn.sendall(f"ok {MAC} ecm\n".encode())
    tap = open_tap(TAP)
    buf = b""
    to_daemon = from_daemon = 0
    while True:
        ready, _, _ = select.select([conn, tap], [], [])
        if tap in ready:
            # Drain what is queued and send it in one write: the daemon must split frames by
            # the length prefix, not by read boundaries.
            batch = b""
            while True:
                try:
                    frame = os.read(tap, 65536)
                except BlockingIOError:
                    break
                batch += struct.pack(">H", len(frame)) + frame
                to_daemon += 1
                if not select.select([tap], [], [], 0)[0]:
                    break
            conn.sendall(batch)
        if conn in ready:
            data = conn.recv(65536)
            if not data:
                break
            buf += data
            while len(buf) >= 2:
                (length,) = struct.unpack(">H", buf[:2])
                if len(buf) < 2 + length:
                    break
                os.write(tap, buf[2:2 + length])
                buf = buf[2 + length:]
                from_daemon += 1
    os.close(tap)
    print(f"closed: adapter returned, frames to daemon {to_daemon}, from daemon {from_daemon}", flush=True)


server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind("\0droiddesk.usb")
server.listen(4)
print("fake bridge listening", flush=True)
while True:
    conn, _ = server.accept()
    with conn:
        session(conn)
