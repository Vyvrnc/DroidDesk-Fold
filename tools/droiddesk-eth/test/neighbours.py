"""Sends what `droiddesk-eth watch` should notice: LLDP, CDP and a gratuitous ARP (run in a namespace)."""
import socket
import struct
import sys

IFACE, IP = sys.argv[1], socket.inet_aton(sys.argv[2])
sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
sock.bind((IFACE, 0))
mac = sock.getsockname()[4]


def lldp_tlv(kind, value):
    return struct.pack(">H", kind << 9 | len(value)) + value


lldp = (bytes.fromhex("0180c200000e") + mac + b"\x88\xcc"
        + lldp_tlv(1, b"\x04" + mac)
        + lldp_tlv(2, b"\x05ge-0/0/7")
        + lldp_tlv(3, struct.pack(">H", 120))
        + lldp_tlv(4, b"rozvadec R1")
        + lldp_tlv(5, b"sw-test")
        + lldp_tlv(6, b"Test switch 1.0")
        + lldp_tlv(8, b"\x05\x01" + IP + b"\x02" + struct.pack(">I", 7) + b"\x00")
        + lldp_tlv(0, b""))


def cdp_tlv(kind, value):
    return struct.pack(">HH", kind, 4 + len(value)) + value


cdp_body = (b"\xaa\xaa\x03\x00\x00\x0c\x20\x00" + b"\x02\xb4\x00\x00"
            + cdp_tlv(1, b"cdp-switch")
            + cdp_tlv(2, struct.pack(">I", 1) + b"\x01\x01\xcc" + struct.pack(">H", 4) + IP)
            + cdp_tlv(3, b"GigabitEthernet0/3")
            + cdp_tlv(6, b"cisco WS-C2960"))
cdp = bytes.fromhex("01000ccccccc") + mac + struct.pack(">H", len(cdp_body)) + cdp_body

garp = (b"\xff" * 6 + mac + b"\x08\x06" + struct.pack(">HHBBH", 1, 0x0800, 6, 4, 2)
        + mac + IP + b"\xff" * 6 + IP)

for frame in (lldp, cdp, garp):
    sock.send(frame.ljust(60, b"\0"))
print("sent LLDP, CDP, gratuitous ARP from", mac.hex(":"))
