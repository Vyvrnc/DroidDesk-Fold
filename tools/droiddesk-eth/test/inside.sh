#!/bin/bash
# Runs in the test container (see run.sh). Topology: bridge br0 with
#   dd0, dd1      TAPs: dd0 = daemon's frame source, dd1 = the fake app bridge's
#   web           192.168.140.10, Raspberry Pi MAC: HTTP :80, UDP echo :7, sends LLDP/CDP/GARP
#   arp           10.227.13.10, Flowbox FBX3100 MAC, nothing listening
#   dhcpd         172.30.5.1, Moxa MAC: dnsmasq DHCP 172.30.5.100-150
#   client        no address, Teltonika MAC: udhcpc DISCOVER/REQUEST for `watch`
set -u
cd /tmp
ETH="/src/bin/droiddesk-eth-amd64"
export TMPDIR=/tmp
fail=0
check() { if [ "$1" -ne 0 ]; then echo "FAIL: $2"; fail=1; else echo "ok: $2"; fi; }
# expect PATTERN FILE DESCRIPTION
expect() { grep -q -- "$1" "$2"; check $? "$3"; }

ip link add br0 type bridge
# Let LLDP (01:80:c2:00:00:0e) through the bridge, a real wire carries it.
ip link set br0 type bridge group_fwd_mask 0x4000
ip link set br0 up
for t in dd0 dd1; do ip tuntap add $t mode tap; ip link set $t master br0 up; done

host() { # host NAME MAC [CIDR]
    ip netns add "$1"
    ip link add "v-$1" type veth peer name eth0 netns "$1"
    ip link set "v-$1" master br0 up
    ip -n "$1" link set lo up
    ip -n "$1" link set eth0 address "$2" up
    [ -n "${3:-}" ] && ip -n "$1" addr add "$3" dev eth0
    return 0
}
host web b8:27:eb:00:00:10 192.168.140.10/24
host arp 70:82:0e:00:00:01 10.227.13.10/24
host dhcpd 00:90:e8:00:00:01 172.30.5.1/24
host client 00:1e:42:00:00:01
ip netns exec arp sysctl -qw net.ipv4.icmp_echo_ignore_all=1 2>/dev/null || true

echo '<h1>droiddesk-eth test page</h1>' > /tmp/index.html
ip netns exec web sh -c 'cd /tmp && exec python3 -m http.server 80 --bind 192.168.140.10' >/tmp/http.log 2>&1 &
ip netns exec web socat UDP4-RECVFROM:7,bind=192.168.140.10,fork EXEC:cat >/dev/null 2>&1 &
ip netns exec dhcpd dnsmasq --no-daemon --port=0 --interface=eth0 --bind-interfaces --no-ping \
    --dhcp-range=172.30.5.100,172.30.5.150,255.255.255.0,1h --dhcp-option=3,172.30.5.1 \
    --dhcp-leasefile=/tmp/dnsmasq.leases >/tmp/dnsmasq.log 2>&1 &
sleep 2

echo "=== start (TAP)"
$ETH start --tap dd0 --mac 02:dd:00:00:00:01; check $? "start --tap dd0"
$ETH status; check $? "status"
$ETH capture -w /tmp/all.pcap 2>/tmp/capture.err &
CAPTURE=$!
$ETH capture > /tmp/second.pcap &
CAPTURE2=$!
sleep 0.5

echo "=== arpscan without an own address (ARP probes)"
$ETH arpscan 192.168.140.0/24 --timeout 1 | tee /tmp/scan1.txt
expect "192.168.140.10 *b8:27:eb:00:00:10 *Raspberry Pi" /tmp/scan1.txt "arpscan finds the web host with MAC and vendor"
$ETH arpscan 10.227.13.0/24 --timeout 1 | tee /tmp/scan2.txt
expect "10.227.13.10 *70:82:0e:00:00:01 *⭐ Flowbox FBX3100" /tmp/scan2.txt "arpscan finds the ARP-only host as FBX3100"

echo "=== addresses in two subnets"
$ETH ip add 192.168.140.250/24; check $? "ip add 192.168.140.250/24"
$ETH ip add 10.227.13.250/24; check $? "ip add 10.227.13.250/24"
$ETH ip add 10.227.13.250/24 2>/dev/null; [ $? -ne 0 ]; check $? "duplicate ip add refused"
$ETH arpscan 10.227.13.0/24 --timeout 1 | tee /tmp/scan3.txt
expect "z adresy 10.227.13.250" /tmp/scan3.txt "arpscan uses our address in the subnet"
expect "10.227.13.10" /tmp/scan3.txt "arpscan with an own address finds the host"

echo "=== ping"
$ETH ping 192.168.140.10 -c 3 | tee /tmp/ping.txt; check ${PIPESTATUS[0]} "ping 192.168.140.10"
expect "3/3" /tmp/ping.txt "3/3 echo replies"
$ETH ping 10.99.0.1 -c 1 2>&1 | tee /tmp/ping2.txt
expect "není v žádné nastavené podsíti" /tmp/ping2.txt "ping outside our subnets explains why"

echo "=== SOCKS5"
curl -s --max-time 10 --socks5 127.0.0.1:1090 http://192.168.140.10/ | tee /tmp/curl.txt
expect "droiddesk-eth test page" /tmp/curl.txt "curl --socks5 through the stack"
curl -s --max-time 10 --socks5-hostname 127.0.0.1:1090 http://192.168.140.10/ > /tmp/curl2.txt
expect "test page" /tmp/curl2.txt "curl --socks5-hostname (address as a name)"
head -c 3000000 /dev/urandom > /tmp/big.bin
curl -s --max-time 30 --socks5 127.0.0.1:1090 -o /tmp/big.down http://192.168.140.10/big.bin
cmp -s /tmp/big.bin /tmp/big.down; check $? "3 MB download through SOCKS5 intact"
curl -s --max-time 10 --socks5 127.0.0.1:1090 http://192.168.140.10:81/ >/dev/null; [ $? -ne 0 ]; check $? "closed port fails cleanly"

echo "=== run (proxychains4)"
$ETH run curl -s --max-time 10 http://192.168.140.10/ > /tmp/run.txt
expect "test page" /tmp/run.txt "droiddesk-eth run curl"

echo "=== forward"
$ETH forward tcp 8080 192.168.140.10 80 > /tmp/fwd-tcp.txt &
FWD_TCP=$!
$ETH forward udp 7007 192.168.140.10 7 > /tmp/fwd-udp.txt &
FWD_UDP=$!
sleep 0.5
cat /tmp/fwd-tcp.txt /tmp/fwd-udp.txt
curl -s --max-time 10 http://127.0.0.1:8080/ > /tmp/fwd.txt
expect "test page" /tmp/fwd.txt "forward tcp 8080 -> 192.168.140.10:80"
echo "ahoj přes UDP" | socat -t 2 - UDP4:127.0.0.1:7007 > /tmp/udp.txt
expect "ahoj přes UDP" /tmp/udp.txt "forward udp 7007 -> UDP echo"
kill -INT $FWD_TCP $FWD_UDP; wait $FWD_TCP $FWD_UDP 2>/dev/null
sleep 0.3
curl -s --max-time 3 http://127.0.0.1:8080/ >/dev/null; [ $? -ne 0 ]; check $? "forward ends with the client"

echo "=== watch"
$ETH watch > /tmp/watch.txt 2>&1 &
WATCH=$!
sleep 0.5
ip netns exec client busybox udhcpc -i eth0 -n -q -t 3 -T 1 -s /bin/true -x hostname:plc-test >/tmp/udhcpc.log 2>&1
ip netns exec web python3 /src/test/neighbours.py eth0 192.168.140.10
sleep 1.5
kill -INT $WATCH; wait $WATCH 2>/dev/null
cat /tmp/watch.txt
expect "DHCP DISCOVER *00:1e:42:00:00:01 (Teltonika) *jméno plc-test" /tmp/watch.txt "watch: DHCP DISCOVER with MAC, vendor, hostname"
expect "DHCP REQUEST *00:1e:42:00:00:01.*chce 172.30.5.1" /tmp/watch.txt "watch: DHCP REQUEST with the requested IP"
expect "LLDP soused.*systém sw-test, port ge-0/0/7 (rozvadec R1), mgmt 192.168.140.10" /tmp/watch.txt "watch: LLDP neighbour"
expect "CDP soused.*systém cdp-switch, port GigabitEthernet0/3, mgmt 192.168.140.10" /tmp/watch.txt "watch: CDP neighbour"
expect "gratuitous ARP *b8:27:eb:00:00:10.*192.168.140.10" /tmp/watch.txt "watch: gratuitous ARP"

echo "=== dhcp"
$ETH dhcp | tee /tmp/dhcp.txt; check ${PIPESTATUS[0]} "dhcp"
expect "Adresa:  172.30.5.1[0-5][0-9]/24 (server 172.30.5.1)" /tmp/dhcp.txt "dhcp lease in the dnsmasq range"
expect "Brána:   172.30.5.1" /tmp/dhcp.txt "dhcp router"
$ETH ping 172.30.5.1 -c 1 >/dev/null; check $? "ping the DHCP server from the leased address"
$ETH route del default; check $? "route del default"
$ETH route add default via 192.168.140.10; check $? "route add default via 192.168.140.10"
$ETH route add default via 10.99.0.1 2>/dev/null; [ $? -ne 0 ]; check $? "gateway outside our subnets refused"
$ETH ip del 10.227.13.250/24; check $? "ip del"
$ETH status | tee /tmp/status.txt
expect "192.168.140.250/24, 172.30.5" /tmp/status.txt "status lists the addresses"
expect "výchozí přes 192.168.140.10" /tmp/status.txt "status shows the default route"

echo "=== capture"
kill -INT $CAPTURE; wait $CAPTURE 2>/dev/null
kill -INT $CAPTURE2; wait $CAPTURE2 2>/dev/null
cat /tmp/capture.err
tcpdump -n -r /tmp/all.pcap 2>/dev/null > /tmp/all.txt; check $? "tcpdump reads the capture"
echo "  $(wc -l < /tmp/all.txt) frames, e.g.:"; grep -m3 -E "ARP|HTTP|ICMP" /tmp/all.txt | sed 's/^/    /'
expect "ARP, Request who-has 192.168.140.10" /tmp/all.txt "capture has our ARP requests (outbound)"
expect "ICMP echo reply" /tmp/all.txt "capture has ICMP replies (inbound)"
expect "LLDP\|0x88cc" /tmp/all.txt "capture has the LLDP frame"
tcpdump -n -r /tmp/second.pcap 2>/dev/null | grep -q "ICMP echo request"; check $? "second concurrent capture works too"

$ETH stop; check $? "stop"
$ETH status >/dev/null; [ $? -eq 3 ]; check $? "status after stop says not running"

echo "=== fake app bridge (@droiddesk.usb), daemon as nobody"
mkdir -p /tmp/nobody && chown nobody /tmp/nobody
AS_NOBODY="setpriv --reuid=nobody --regid=nogroup --clear-groups env TMPDIR=/tmp/nobody"
fake() { FAKE_LIST=$1 python3 /src/test/fake_bridge.py dd1 > /tmp/fake-$1.log 2>&1 & FAKE=$!; sleep 0.5; }
fake none
$AS_NOBODY $ETH start > /tmp/none.txt 2>&1; [ $? -ne 0 ]; check $? "no ECM adapter: start fails"
cat /tmp/none.txt
expect "žádný podporovaný Ethernet adaptér" /tmp/none.txt "no ECM adapter: Czech explanation"
expect "AX88179" /tmp/none.txt "no ECM adapter: list printed"
kill $FAKE; wait $FAKE 2>/dev/null
fake two
$AS_NOBODY $ETH start > /tmp/two.txt 2>&1; [ $? -ne 0 ]; check $? "two ECM adapters: start asks for --device"
expect "start --device" /tmp/two.txt "two ECM adapters: hint"
kill $FAKE; wait $FAKE 2>/dev/null
fake one
$AS_NOBODY $ETH start | tee /tmp/start.txt; check ${PIPESTATUS[0]} "start via the fake bridge (list -> eth)"
expect "Potvrď na telefonu" /tmp/start.txt "permission line shown"
expect "MAC 02:00:00:00:00:01, režim ecm" /tmp/start.txt "ok <mac> ecm parsed"
$AS_NOBODY $ETH ip add 192.168.140.251/24; check $? "ip add over the bridge"
$AS_NOBODY $ETH arpscan 192.168.140.0/24 --timeout 1 > /tmp/scan4.txt
expect "192.168.140.10 *b8:27:eb:00:00:10" /tmp/scan4.txt "arpscan over the bridge"
curl -s --max-time 30 --socks5 127.0.0.1:1090 -o /tmp/big2.down http://192.168.140.10/big.bin
cmp -s /tmp/big.bin /tmp/big2.down; check $? "3 MB through SOCKS5 over the length-prefixed bridge"
$AS_NOBODY $ETH status | grep -E "Zdroj|Přijato|Odesláno"
$AS_NOBODY $ETH stop; check $? "stop over the bridge"
sleep 0.5
cat /tmp/fake-one.log
expect "closed: adapter returned" /tmp/fake-one.log "closing the socket gives the adapter back"
kill $FAKE 2>/dev/null

echo
if [ $fail -eq 0 ]; then echo "ALL OK"; else echo "SOME TESTS FAILED"; echo "--- daemon log"; tail -30 /tmp/droiddesk-eth.log; fi
exit $fail
