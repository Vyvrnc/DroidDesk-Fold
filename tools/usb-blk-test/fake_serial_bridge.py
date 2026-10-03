"""Fake UsbBridge for droiddesk-serial: "list" with one USB serial adapter and "serial" with a
simulated device behind it. Logs every frame from the client to stdout (P/M/B/D).

The "adapter" behaves by its parameters, like a real bus: at 19200 8E1 a Modbus RTU slave
(address 1; functions 3 and 6, 100 holding registers) answers, at 115200 8N1 every byte is
echoed, anything else gets no answer. Creating /tmp/unplug makes it report an error and
close the session (unplugged adapter).
"""
import os
import socket
import struct
import sys
import threading

DEVICE = "/dev/bus/usb/001/005"
registers = [i * 11 for i in range(100)]


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return struct.pack("<H", crc)


def modbus(request):
    """Reply to one RTU frame, or None (not for us / bad CRC)."""
    if len(request) < 8 or crc16(request[:-2]) != request[-2:] or request[0] != 1:
        return None
    function = request[1]
    if function == 3:
        start, count = struct.unpack(">HH", request[2:6])
        if start + count > len(registers) or not 1 <= count <= 125:
            body = bytes([1, 0x83, 2])
        else:
            body = bytes([1, 3, count * 2]) + b"".join(struct.pack(">H", v) for v in registers[start:start + count])
    elif function == 6:
        address, value = struct.unpack(">HH", request[2:6])
        if address >= len(registers):
            body = bytes([1, 0x86, 2])
        else:
            registers[address] = value
            body = request[:6]
    else:
        body = bytes([1, 0x80 | function, 1])
    return body + crc16(body)


def log(text):
    print(text, flush=True)


def handle(conn):
    reader = conn.makefile("rb")
    request = reader.readline().decode().strip()
    if request == "list":
        conn.sendall(f"{DEVICE}\t0403\t6001\t0\tFTDI\tFT232R USB UART\t0\t1\n"
                     "/dev/bus/usb/001/002\t090c\t1000\t1\tSamsung\tType-C\t0\t0\n\n".encode())
        return
    if not request.startswith("serial "):
        conn.sendall(b"err unknown request\n")
        return
    _, name, port = request.split()
    if name != DEVICE or port != "0":
        conn.sendall(b"err not a supported serial adapter or no port " + port.encode() + b"\n")
        return
    conn.sendall(b"ok Ftdi 1\n")
    log("open")
    params = None
    lock = threading.Lock()
    alive = [True]
    pending = bytearray()

    def send(kind, payload):
        with lock:
            conn.sendall(kind + struct.pack(">H", len(payload)) + payload)

    def watch_unplug():
        import time
        while alive[0]:
            if os.path.exists("/tmp/unplug"):
                os.unlink("/tmp/unplug")
                send(b"E", b"device disconnected")
                log("unplug")
                conn.shutdown(socket.SHUT_RDWR)
                return
            time.sleep(0.1)
    threading.Thread(target=watch_unplug, daemon=True).start()
    send(b"S", bytes([1 | 2]))  # CTS and DSR
    try:
        while True:
            head = reader.read(3)
            if len(head) < 3:
                break
            kind, length = head[:1], struct.unpack(">H", head[1:])[0]
            payload = reader.read(length)
            if kind == b"P":
                params = struct.unpack(">IBBB", payload)
                log("P %d %d %d %d" % params)
            elif kind == b"M":
                log("M %d %d" % (payload[0], payload[1]))
            elif kind == b"B":
                log("B %d" % struct.unpack(">H", payload))
            elif kind == b"D":
                log("D %d" % len(payload))
                if params == (115200, 8, 1, 0):
                    send(b"D", payload)
                elif params == (19200, 8, 1, 2):
                    pending.extend(payload)
                    reply = modbus(bytes(pending))
                    if reply is not None or len(pending) >= 8:
                        pending.clear()
                    if reply:
                        send(b"D", reply)
    except OSError:
        pass
    finally:
        alive[0] = False
        log("close")


server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind("\0droiddesk.usb")
server.listen(8)
log("listening")
while True:
    c, _ = server.accept()
    threading.Thread(target=lambda c=c: (handle(c), c.close()), daemon=True).start()
