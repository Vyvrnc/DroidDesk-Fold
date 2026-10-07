package main

import (
	"fmt"
	"net"
	"net/netip"
	"os"
	"strings"
	"time"
)

// watch listens without sending anything: the way to find a device that has no address yet
// (it asks DHCP), just took one (ARP probe/announcement) or sits in a foreign subnet (it ARPs
// for its gateway), and which switch port we are on (LLDP/CDP).
func cmdWatch(args []string) {
	if len(args) > 0 {
		fail("použití: droiddesk-eth watch")
	}
	st, running := readStatus()
	if !running {
		fail("neběží (spusť droiddesk-eth start)")
	}
	ours := strings.ToLower(st.get("mac", 0))
	conn, reader := request("capture")
	answer, err := reader.ReadString('\n')
	if err != nil || strings.TrimSpace(answer) != "ok" {
		fail("%s", strings.TrimPrefix(orNoAnswer(strings.TrimSpace(answer)), "err "))
	}
	interrupted(conn)
	pcap, err := newPcapReader(reader)
	if err != nil {
		fail("%v", err)
	}
	fmt.Println("Poslouchám (DHCP, ARP, LLDP, CDP, nová zařízení); Ctrl+C ukončí")
	w := &watcher{ours: ours, lastShown: map[string]time.Time{}, devices: map[string]bool{}}
	for {
		at, frame, err := pcap.next()
		if err != nil {
			return
		}
		w.frame(at, frame)
	}
}

type watcher struct {
	ours      string
	lastShown map[string]time.Time
	devices   map[string]bool // MAC+IP pairs already reported
}

// show prints an event unless the same one was printed recently: devices repeat DISCOVERs
// every few seconds and switches LLDP every 30 s.
func (w *watcher) show(at time.Time, quiet time.Duration, kind string, mac net.HardwareAddr, text string) {
	key := kind + mac.String() + text
	if last, ok := w.lastShown[key]; ok && at.Sub(last) < quiet {
		return
	}
	w.lastShown[key] = at
	vendor := vendorOf(mac)
	if vendor != "" {
		vendor = " (" + vendor + ")"
	}
	fmt.Printf("%s  %-15s %s%s  %s\n", at.Format("15:04:05"), kind, mac, vendor, text)
	os.Stdout.Sync()
}

func (w *watcher) device(at time.Time, mac net.HardwareAddr, ip netip.Addr) {
	key := mac.String() + " " + ip.String()
	if w.devices[key] {
		return
	}
	w.devices[key] = true
	w.show(at, 0, "zařízení", mac, ip.String())
}

func (w *watcher) frame(at time.Time, frame []byte) {
	if len(frame) < 14 {
		return
	}
	dst, src := net.HardwareAddr(frame[0:6]), net.HardwareAddr(frame[6:12])
	if src.String() == w.ours {
		return
	}
	typ, payload, ok := etherPayload(frame)
	if !ok {
		return
	}
	switch {
	case typ == etherTypeARP:
		w.arp(at, payload)
	case typ == etherTypeIPv4:
		w.ipv4(at, src, payload)
	case typ == etherTypeLLDP:
		if n, ok := parseLLDP(payload); ok {
			w.neighbour(at, src, n)
		}
	case typ <= 1500 && dst.String() == cdpDest.String():
		if n, ok := parseCDP(payload); ok {
			w.neighbour(at, src, n)
		}
	}
}

func (w *watcher) arp(at time.Time, payload []byte) {
	a, ok := parseARP(payload)
	if !ok {
		return
	}
	switch {
	case a.Op == 1 && !a.SenderIP.IsUnspecified() && a.SenderIP == a.TargetIP:
		w.show(at, 30*time.Second, "ARP oznámení", a.SenderMAC, "bere si "+a.SenderIP.String())
	case a.Op == 1 && a.SenderIP.IsUnspecified():
		w.show(at, 30*time.Second, "ARP probe", a.SenderMAC, "zkouší, zda je volná "+a.TargetIP.String())
		return
	case a.Op == 2 && a.SenderIP == a.TargetIP:
		w.show(at, 30*time.Second, "gratuitous ARP", a.SenderMAC, a.SenderIP.String())
	}
	if !a.SenderIP.IsUnspecified() {
		w.device(at, a.SenderMAC, a.SenderIP)
	}
}

func (w *watcher) ipv4(at time.Time, src net.HardwareAddr, payload []byte) {
	if _, _, sport, dport, data, ok := udpPayload(payload); ok && sport == 68 && dport == 67 {
		w.dhcp(at, data)
		return
	}
	if len(payload) < 20 {
		return
	}
	ip := netip.AddrFrom4([4]byte(payload[12:16]))
	// Only local sources: behind a router every internet address carries the router's MAC.
	if ip.IsPrivate() || ip.IsLinkLocalUnicast() {
		w.device(at, src, ip)
	}
}

func (w *watcher) dhcp(at time.Time, data []byte) {
	m, ok := parseDHCP(data)
	if !ok || m.Op != 1 {
		return
	}
	name := dhcpTypeNames[m.Type()]
	if name == "" {
		name = fmt.Sprintf("typ %d", m.Type())
	}
	var parts []string
	if host := m.Options[12]; len(host) > 0 {
		parts = append(parts, "jméno "+string(host))
	}
	if req := m.optAddr(50); req.IsValid() {
		parts = append(parts, "chce "+req.String())
	} else if m.CIAddr.IsValid() && !m.CIAddr.IsUnspecified() {
		parts = append(parts, "má "+m.CIAddr.String())
	}
	if vendor := m.Options[60]; len(vendor) > 0 {
		parts = append(parts, "třída "+string(vendor))
	}
	if len(parts) == 0 {
		parts = append(parts, "bez adresy")
	}
	w.show(at, 10*time.Second, "DHCP "+name, m.CHAddr, strings.Join(parts, ", "))
}

func (w *watcher) neighbour(at time.Time, src net.HardwareAddr, n neighbour) {
	var parts []string
	if n.System != "" {
		parts = append(parts, "systém "+n.System)
	}
	port := n.Port
	if n.PortDesc != "" && n.PortDesc != n.Port {
		port = strings.TrimSpace(port + " (" + n.PortDesc + ")")
	}
	if port != "" {
		parts = append(parts, "port "+port)
	}
	if n.MgmtIP.IsValid() {
		parts = append(parts, "mgmt "+n.MgmtIP.String())
	}
	if n.Platform != "" {
		platform := n.Platform
		if len(platform) > 60 {
			platform = platform[:60] + "…"
		}
		parts = append(parts, platform)
	}
	w.show(at, 5*time.Minute, n.Protocol+" soused", src, strings.Join(parts, ", "))
}
