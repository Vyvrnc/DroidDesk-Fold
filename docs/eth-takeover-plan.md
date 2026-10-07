# Ethernet takeover for diagnostics — plan (2026-10-07)

Goal: the USB Ethernet adapter in a dock (or a USB NIC) belongs to Linux instead of Android for
a while, without root: capture everything on the wire (tcpdump/Wireshark), ARP scan, watch DHCP,
find devices without an IP or in a foreign subnet (Flowbox FBX3100 OUI `70:82:0E`), and talk to
them from any IP we choose. Android has no Ethernet while it lasts. The second mode next to
NetBridge (phase 1, `droiddesk-net`), which keeps the adapter in Android and only binds sockets.

## Feasibility (checked)
- Android hands any USB device to an app except hubs and HID boot keyboards/mice
  (`UsbHostManager.isDenyListed`, plus an OEM address deny list). A USB NIC is allowed.
- `claimInterface(intf, force = true)` detaches the kernel driver (r8152 / ax88179 / cdc_ether);
  Android's `eth0` disappears.
- Many Realtek adapters (RTL8153 `0bda:8153`, RTL8156 `0bda:8156`) carry a second configuration
  with standard CDC-ECM (or NCM) next to the vendor one the kernel driver uses. In CDC mode the
  bulk endpoints carry plain Ethernet frames, and `SET_ETHERNET_PACKET_FILTER` turns on
  promiscuous mode. ASIX AX88179 (`0b95:1790`) has only its vendor protocol: a small port of
  Linux `ax88179_178a.c` (init registers, RX header) would be needed. **First step: the adapter's
  vid:pid and configurations** (`droiddesk-usb list` shows vid:pid).
- Giving it back: switching to the vendor configuration again makes the kernel probe its driver,
  so Android's Ethernet returns. Otherwise unplug and plug in the dock.

## Pieces
- **App (Kotlin)** `UsbBridge` request `eth <name>`: permission, pick the CDC configuration,
  claim with force, data interface alt setting 1, packet filter promiscuous|all multicast|
  broadcast|directed, read the MAC (iMACAddress string). Answers `ok <mac> <mode>` and then
  carries frames both ways on the socket: `length(2, BE) frame`. Closing the socket gives the
  adapter back. Reads with a few UsbRequests queued so bursts are not dropped.
- **Linux side** `droiddesk-eth` (Go, static arm64, runs in Termux and in Debian; downloaded
  on first use like mesa-kgsl, not in the APK): a daemon holds the bridge socket and runs a
  userspace TCP/IP stack (gVisor netstack) on the frames.
  - `start [DEVICE]` / `stop` / `status`
  - `capture [-w FILE]` pcap stream: `droiddesk-eth capture | tcpdump -n -r -`,
    `wireshark -k -i <(droiddesk-eth capture)`
  - `ip add 192.168.140.250/20` (several, for foreign subnets), `ip del`, `dhcp`
  - `arpscan 192.168.140.0/20` (IP, MAC, vendor from OUI; FBX3100 marked)
  - `watch` passive: DHCP requests (devices without an IP), ARP probes, LLDP/CDP
  - `ping HOST`, SOCKS5 on 127.0.0.1:1090 through the stack, `run CMD` (proxychains4 like
    `droiddesk-net run`), `forward tcp|udp LPORT HOST PORT`
- **Sítě window**: on the Ethernet row a mode switch "Android (proxy)" / "Linux diagnostika
  (převzít)".
- **Harness**: the daemon's frame source is either the bridge socket or a Linux TAP; in Docker a
  TAP plus a network namespace with real hosts (DHCP server, HTTP, ARP) tests everything except
  USB. USB itself only on the device.
