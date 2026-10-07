#!/usr/bin/env python3
"""droiddesk-net: Linux access to networks Android does not route to.

An Ethernet dock in a VLAN without internet stays a side network while the mobile data
remain the default; without root Linux cannot pick the interface itself. The app binds
its sockets to the chosen network and offers it as a SOCKS5 proxy on 127.0.0.1:

  droiddesk-net status                         networks, addresses, proxies and rules
  droiddesk-net enable IFACE                   switch a network on for Linux (e.g. eth0)
  droiddesk-net disable IFACE
  droiddesk-net rule add CIDR IFACE            e.g. 10.227.13.0/24 eth0
  droiddesk-net rule del CIDR
  droiddesk-net run CMD [ARGS...]              CMD through the router (port 1080, proxychains4):
                                               rule subnets via their network, the rest as usual
  droiddesk-net ping [--via IFACE] HOST [N]    ICMP through the network (by the rules by default)
  droiddesk-net forward tcp|udp LPORT HOST PORT [--via IFACE]
                                               127.0.0.1:LPORT -> HOST:PORT through the network
                                               (UDP, or programs without SOCKS); Ctrl+C ends it
  droiddesk-net pac [PATH]                     PAC file for Firefox from the rules

Proxies: 1080 = router by the rules, 1081 Ethernet, 1082 Wi-Fi, others from 1083
(socks5://127.0.0.1:PORT, for curl: --socks5-hostname). They run while a network is on.
"""
import os
import signal
import socket
import sys

SOCKET = "\0droiddesk.net"
ROUTER_PORT = 1080


def connect():
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        client.connect(SOCKET)
    except OSError:
        sys.exit("droiddesk-net: the DroidDesk app is not running (no network bridge)")
    return client


def request(line):
    """Sends one request and returns the reader over the answer lines."""
    client = connect()
    client.sendall((line + "\n").encode())
    return client, client.makefile("r", encoding="utf-8", errors="replace")


def simple(line):
    client, reader = request(line)
    answer = reader.readline().strip()
    client.close()
    if answer != "ok":
        sys.exit("droiddesk-net: " + (answer[4:] if answer.startswith("err ") else answer or "no answer"))


def read_status():
    client, reader = request("status")
    nets, router, rules = [], None, []
    for line in reader:
        line = line.rstrip("\n")
        if not line:
            break
        parts = line.split("\t")
        if parts[0] == "net" and len(parts) >= 9:
            nets.append(dict(iface=parts[1], transport=parts[2], addresses=parts[3], validated=parts[4] == "1",
                             default=parts[5] == "1", enabled=parts[6] == "1", port=int(parts[7]), state=parts[8]))
        elif parts[0] == "router":
            router = dict(port=int(parts[1]), state=parts[2])
        elif parts[0] == "rule":
            rules.append((parts[1], parts[2]))
    client.close()
    return nets, router, rules


TRANSPORTS = {"ethernet": "Ethernet", "wifi": "Wi-Fi", "mobile": "mobilní data", "vpn": "VPN", "usb": "USB"}
STATES = {"ok": "běží", "busy": "port obsazený", "off": "vypnutá", "disconnected": "síť nepřipojená"}


def status():
    nets, router, rules = read_status()
    print("Sítě:")
    for net in nets:
        notes = [TRANSPORTS.get(net["transport"], net["transport"])]
        if net["default"]:
            notes.append("výchozí")
        if not net["validated"] and net["transport"] != "-":
            notes.append("bez ověřeného internetu")
        proxy = f"proxy 127.0.0.1:{net['port']} ({STATES.get(net['state'], net['state'])})" if net["enabled"] else "pro Linux vypnutá"
        print(f"  {net['iface']:<12} {net['addresses'] or '-':<34} {', '.join(notes)}; {proxy}")
    if router:
        print(f"Router: 127.0.0.1:{router['port']} ({STATES.get(router['state'], router['state'])})")
    print("Pravidla:" if rules else "Pravidla: žádná (vše jde výchozí sítí)")
    for cidr, iface in rules:
        print(f"  {cidr:<20} -> {iface}")


def run(argv):
    if not argv:
        sys.exit("usage: droiddesk-net run CMD [ARGS...]")
    path = os.path.join(os.environ.get("TMPDIR", "/tmp"), "droiddesk-net-proxychains.conf")
    write_proxychains(path)
    try:
        os.execvp("proxychains4", ["proxychains4", "-q", "-f", path] + argv)
    except FileNotFoundError:
        sys.exit("droiddesk-net: proxychains4 is missing (Debian: apt install proxychains4, Termux: pkg install proxychains-ng)")


def write_proxychains(path):
    with open(path, "w") as conf:
        conf.write("strict_chain\nquiet_mode\nproxy_dns\nremote_dns_subnet 224\n"
                   "tcp_read_time_out 15000\ntcp_connect_time_out 10000\n"
                   "localnet 127.0.0.0/255.0.0.0\n[ProxyList]\n"
                   f"socks5 127.0.0.1 {ROUTER_PORT}\n")


def ping(argv):
    via = "auto"
    if len(argv) >= 2 and argv[0] == "--via":
        via, argv = argv[1], argv[2:]
    if not argv:
        sys.exit("usage: droiddesk-net ping [--via IFACE] HOST [COUNT]")
    count = argv[1] if len(argv) > 1 else "4"
    client, reader = request(f"ping {via} {argv[0]} {count}")
    failed = False
    for line in reader:
        line = line.rstrip("\n")
        if line == "done":
            break
        if line.startswith("err "):
            failed = True
            print("droiddesk-net: " + line[4:], file=sys.stderr)
        else:
            print(line, flush=True)
            if line.startswith("0/"):
                failed = True
    client.close()
    sys.exit(1 if failed else 0)


def forward(argv):
    via = "auto"
    if "--via" in argv:
        index = argv.index("--via")
        via = argv[index + 1] if index + 1 < len(argv) else ""
        argv = argv[:index] + argv[index + 2:]
    if len(argv) != 4 or argv[0] not in ("tcp", "udp") or not via:
        sys.exit("usage: droiddesk-net forward tcp|udp LPORT HOST PORT [--via IFACE]")
    protocol, local_port, host, port = argv
    client, reader = request(f"forward {protocol} {local_port} {via} {host} {port}")
    answer = reader.readline().strip()
    if not answer.startswith("ok"):
        sys.exit("droiddesk-net: " + (answer[4:] if answer.startswith("err ") else answer or "no answer"))
    print(f"127.0.0.1:{answer.split()[1]}/{protocol} -> {host}:{port} (via {via}); Ctrl+C ends it", flush=True)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    try:
        # The app forwards until this connection closes.
        client.recv(1)
    except KeyboardInterrupt:
        pass
    client.close()


def pac(argv):
    _, _, rules = read_status()
    path = argv[0] if argv else os.path.expanduser("~/.config/droiddesk/net.pac")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    lines = ["// From droiddesk-net rules: these subnets through the DroidDesk router.",
             "function FindProxyForURL(url, host) {"]
    for cidr, iface in rules:
        network, prefix = cidr.split("/")
        mask = (0xFFFFFFFF << (32 - int(prefix))) & 0xFFFFFFFF if int(prefix) else 0
        mask_text = ".".join(str((mask >> shift) & 0xFF) for shift in (24, 16, 8, 0))
        lines.append(f'  if (isInNet(host, "{network}", "{mask_text}")) return "SOCKS5 127.0.0.1:{ROUTER_PORT}";  // {iface}')
    lines += ['  return "DIRECT";', "}", ""]
    with open(path, "w") as out:
        out.write("\n".join(lines))
    print(path)
    print("Firefox: Settings > Network Settings > Automatic proxy configuration URL: file://" + os.path.abspath(path))


def main(argv):
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(__doc__.strip())
        return
    command, rest = argv[0], argv[1:]
    if command == "status":
        status()
    elif command in ("enable", "disable") and len(rest) == 1:
        simple(f"{command} {rest[0]}")
    elif command == "rule" and len(rest) == 3 and rest[0] == "add":
        simple(f"rule-add {rest[1]} {rest[2]}")
    elif command == "rule" and len(rest) == 2 and rest[0] in ("del", "rm"):
        simple(f"rule-del {rest[1]}")
    elif command == "run":
        run(rest)
    elif command == "ping":
        ping(rest)
    elif command == "forward":
        forward(rest)
    elif command == "pac":
        pac(rest)
    elif command == "proxychains-conf" and len(rest) == 1:
        write_proxychains(rest[0])
    else:
        sys.exit(__doc__.strip())


if __name__ == "__main__":
    main(sys.argv[1:])
