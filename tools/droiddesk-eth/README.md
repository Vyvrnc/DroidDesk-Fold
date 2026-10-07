# droiddesk-eth

Linux diagnostics on a USB Ethernet adapter the DroidDesk app took over from Android
(design: `docs/eth-takeover-plan.md`). The app carries raw Ethernet frames over the abstract
socket `@droiddesk.usb`; `droiddesk-eth` runs a userspace TCP/IP stack on them
([gVisor netstack](https://gvisor.dev), `go` branch) plus raw tools: capture, ARP scan, passive
watch. No root, one static binary, the same in Termux and in Debian proot.

## Build

Static, no cgo. Without a local Go, in Docker (Git Bash on Windows: `export MSYS_NO_PATHCONV=1`):

```sh
docker run --rm -v "$PWD":/src -w /src golang:1.27 sh -c \
  'CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -trimpath -ldflags="-s -w" -o bin/droiddesk-eth-arm64 .'
```

`amd64` the same with `GOARCH=amd64`. The first build downloads the modules (gVisor,
golang.org/x/sys, x/time, x/exp, google/btree; pinned in `go.mod`/`go.sum`), so it needs
network; `go mod vendor` makes an offline tree if ever needed. Binary size about 5.3 MB.

## Use

```sh
droiddesk-eth start                      # takes the adapter over (picks the only "ecm" one)
droiddesk-eth start --device /dev/bus/usb/002/003
droiddesk-eth status
droiddesk-eth arpscan 192.168.140.0/20   # IP, MAC, vendor; ⭐ Flowbox FBX3100 (70:82:0E)
droiddesk-eth watch                      # DHCP DISCOVER/REQUEST, ARP probes, LLDP/CDP, new MAC+IP
droiddesk-eth ip add 192.168.140.250/20  # several addresses in different subnets are fine
droiddesk-eth dhcp                       # lease + default route (not renewed)
droiddesk-eth route add default via 192.168.140.1
droiddesk-eth ping 192.168.140.10 -c 3
curl --socks5 127.0.0.1:1090 http://192.168.140.10/
droiddesk-eth run ssh admin@192.168.140.10        # proxychains4, no proxy_dns
droiddesk-eth forward tcp 8502 192.168.140.10 502 # Modbus tool on 127.0.0.1:8502
droiddesk-eth forward udp 47808 192.168.140.20 47808
droiddesk-eth capture | tcpdump -n -r -
droiddesk-eth capture -w /sdcard/Download/site.pcap
droiddesk-eth stop                       # the adapter goes back to Android
```

The daemon logs to `$TMPDIR/droiddesk-eth.log`; the control socket is
`$TMPDIR/droiddesk-eth.sock` (mode 0600). `start --tap NAME` uses a Linux TAP instead of the
app (tests).

Source address: for each connection the address whose subnet holds the target is used, else
the one that reaches the default gateway. Only frames for our MAC, broadcast and multicast go to
the stack; capture and watch see everything the adapter delivers (promiscuous in CDC mode).

## Test

`test/run.sh` (Docker): builds both binaries, then in a container bridges a TAP to network
namespaces with an HTTP server, a UDP echo, an ARP-only "FBX3100", dnsmasq and a udhcpc client,
and checks arpscan, ping, SOCKS5 (incl. a 3 MB download), `run`, both forwards, watch
(DHCP/LLDP/CDP/gratuitous ARP), dhcp, routes and concurrent captures read by tcpdump. A fake
`@droiddesk.usb` (`test/fake_bridge.py`) then plays the app: `list` with the Ethernet-mode
field, `permission` before `ok <mac> ecm`, batched length-prefixed frames to a second TAP; the
daemon runs as `nobody` there.
