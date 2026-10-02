"""Fake UsbBridge for tests: "list" and "blk" over an image file (one session at a time)."""
import socket
import struct
import sys
import threading

IMAGE = sys.argv[1]
BS = int(sys.argv[2]) if len(sys.argv) > 2 else 512
NAME = "/dev/bus/usb/001/002"
lock = threading.Lock()
stats = {"R": 0, "W": 0, "F": 0, "sessions": 0}


def read_exact(conn, n):
    data = b""
    while len(data) < n:
        part = conn.recv(n - len(data))
        if not part:
            raise EOFError
        data += part
    return data


def handle(conn):
    with conn:
        line = b""
        while not line.endswith(b"\n"):
            c = conn.recv(1)
            if not c:
                return
            line += c
        request = line.decode().strip()
        if request == "list":
            conn.sendall(f"{NAME}\tabcd\t1234\t1\tTest\tFake disk\t1\n\n".encode())
            return
        if not request.startswith("blk "):
            conn.sendall(b"err unknown request\n")
            return
        if not lock.acquire(timeout=20):
            print("DEADLOCK: second blk session while one is open", flush=True)
            conn.sendall(b"err busy\n")
            return
        stats["sessions"] += 1
        counts = {"R": [0, 0], "W": [0, 0], "F": [0, 0]}
        try:
            with open(IMAGE, "r+b") as img:
                img.seek(0, 2)
                cap = img.tell()
                conn.sendall(f"device {cap} {BS} 0 1\n".encode())
                while True:
                    try:
                        op, lba, count = struct.unpack(">cQI", read_exact(conn, 13))
                    except EOFError:
                        return
                    op = op.decode()
                    if op == "Q":
                        return
                    n = count * BS
                    if op != "F" and (count <= 0 or n > 1 << 20):
                        msg = b"bad request"
                        conn.sendall(b"\x01" + struct.pack(">H", len(msg)) + msg)
                        return
                    stats[op] = stats.get(op, 0) + 1
                    counts[op][0] += 1
                    counts[op][1] += n if op != "F" else 0
                    if op == "W":
                        data = read_exact(conn, n)
                    if op in "RW" and (lba + count) * BS > cap:
                        msg = b"access beyond the end of the device"
                        conn.sendall(b"\x01" + struct.pack(">H", len(msg)) + msg)
                        continue
                    if op == "R":
                        img.seek(lba * BS)
                        conn.sendall(b"\x00" + img.read(n))
                    elif op == "W":
                        img.seek(lba * BS)
                        img.write(data)
                        conn.sendall(b"\x00")
                    elif op == "F":
                        img.flush()
                        conn.sendall(b"\x00")
        finally:
            print("session " + " ".join(f"{k}={v[0]}/{v[1] >> 10}KiB" for k, v in counts.items()), flush=True)
            lock.release()


server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind("\0droiddesk.usb")
server.listen(8)
print("fake bridge ready", flush=True)
while True:
    c, _ = server.accept()
    threading.Thread(target=handle, args=(c,), daemon=True).start()
