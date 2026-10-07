package main

import (
	"context"
	"errors"
	"fmt"
	"net"
	"net/netip"
	"sort"
	"sync"

	"gvisor.dev/gvisor/pkg/buffer"
	"gvisor.dev/gvisor/pkg/tcpip"
	"gvisor.dev/gvisor/pkg/tcpip/adapters/gonet"
	"gvisor.dev/gvisor/pkg/tcpip/header"
	"gvisor.dev/gvisor/pkg/tcpip/link/channel"
	"gvisor.dev/gvisor/pkg/tcpip/link/ethernet"
	"gvisor.dev/gvisor/pkg/tcpip/network/arp"
	"gvisor.dev/gvisor/pkg/tcpip/network/ipv4"
	"gvisor.dev/gvisor/pkg/tcpip/stack"
	"gvisor.dev/gvisor/pkg/tcpip/transport/icmp"
	"gvisor.dev/gvisor/pkg/tcpip/transport/tcp"
	"gvisor.dev/gvisor/pkg/tcpip/transport/udp"
)

const nicID = 1

// netStack is gVisor's TCP/IP on one NIC. The channel endpoint is the boundary to our frame
// loops; the ethernet wrapper adds/strips the link header and makes the stack resolve
// neighbours with ARP, as a real Ethernet NIC would.
type netStack struct {
	s    *stack.Stack
	link *channel.Endpoint
	mac  net.HardwareAddr

	mu      sync.Mutex
	addrs   []netip.Prefix // in the order added; the first match wins as a source
	gateway netip.Addr
}

func newNetStack(mac net.HardwareAddr) (*netStack, error) {
	s := stack.New(stack.Options{
		NetworkProtocols:   []stack.NetworkProtocolFactory{ipv4.NewProtocol, arp.NewProtocol},
		TransportProtocols: []stack.TransportProtocolFactory{tcp.NewProtocol, udp.NewProtocol, icmp.NewProtocol4},
	})
	link := channel.New(1024, 1500, tcpip.LinkAddress(mac))
	if err := s.CreateNIC(nicID, ethernet.New(link)); err != nil {
		return nil, errors.New(err.String())
	}
	sack := tcpip.TCPSACKEnabled(true)
	s.SetTransportProtocolOption(tcp.ProtocolNumber, &sack)
	return &netStack{s: s, link: link, mac: mac}, nil
}

// inject hands an inbound frame (with its Ethernet header) to the stack.
func (n *netStack) inject(frame []byte) {
	pkt := stack.NewPacketBuffer(stack.PacketBufferOptions{Payload: buffer.MakeWithData(frame)})
	n.link.InjectInbound(0, pkt)
	pkt.DecRef()
}

// next blocks for the stack's next outbound frame; nil when ctx ends.
func (n *netStack) next(ctx context.Context) []byte {
	pkt := n.link.ReadContext(ctx)
	if pkt == nil {
		return nil
	}
	view := pkt.ToView()
	frame := append([]byte(nil), view.AsSlice()...)
	view.Release()
	pkt.DecRef()
	return frame
}

func toTCPIP(a netip.Addr) tcpip.Address { return tcpip.AddrFrom4(a.As4()) }

func (n *netStack) addAddress(p netip.Prefix) error {
	if !p.Addr().Is4() {
		return errors.New("jen IPv4")
	}
	n.mu.Lock()
	defer n.mu.Unlock()
	for _, have := range n.addrs {
		if have.Addr() == p.Addr() {
			return fmt.Errorf("adresa %s už je nastavená (%s)", p.Addr(), have)
		}
	}
	protoAddr := tcpip.ProtocolAddress{
		Protocol:          ipv4.ProtocolNumber,
		AddressWithPrefix: tcpip.AddressWithPrefix{Address: toTCPIP(p.Addr()), PrefixLen: p.Bits()},
	}
	if err := n.s.AddProtocolAddress(nicID, protoAddr, stack.AddressProperties{}); err != nil {
		return errors.New(err.String())
	}
	n.addrs = append(n.addrs, p)
	n.updateRoutesLocked()
	return nil
}

func (n *netStack) delAddress(p netip.Prefix) error {
	n.mu.Lock()
	defer n.mu.Unlock()
	for i, have := range n.addrs {
		if have.Addr() == p.Addr() {
			if err := n.s.RemoveAddress(nicID, toTCPIP(p.Addr())); err != nil {
				return errors.New(err.String())
			}
			n.addrs = append(n.addrs[:i], n.addrs[i+1:]...)
			n.updateRoutesLocked()
			return nil
		}
	}
	return fmt.Errorf("adresa %s není nastavená", p.Addr())
}

func (n *netStack) setGateway(gw netip.Addr) error {
	n.mu.Lock()
	defer n.mu.Unlock()
	if gw.IsValid() && !n.sourceForLocked(gw).IsValid() {
		return fmt.Errorf("brána %s není v žádné nastavené podsíti (nejdřív ip add)", gw)
	}
	n.gateway = gw
	n.updateRoutesLocked()
	return nil
}

// updateRoutesLocked: every address's subnet is on-link, longest prefix first, then the
// default route if any.
func (n *netStack) updateRoutesLocked() {
	prefixes := append([]netip.Prefix(nil), n.addrs...)
	sort.SliceStable(prefixes, func(i, j int) bool { return prefixes[i].Bits() > prefixes[j].Bits() })
	var table []tcpip.Route
	for _, p := range prefixes {
		sub := tcpip.AddressWithPrefix{Address: toTCPIP(p.Addr()), PrefixLen: p.Bits()}.Subnet()
		table = append(table, tcpip.Route{Destination: sub, NIC: nicID})
	}
	if n.gateway.IsValid() {
		table = append(table, tcpip.Route{Destination: header.IPv4EmptySubnet, Gateway: toTCPIP(n.gateway), NIC: nicID})
	}
	n.s.SetRouteTable(table)
}

func (n *netStack) snapshot() ([]netip.Prefix, netip.Addr) {
	n.mu.Lock()
	defer n.mu.Unlock()
	return append([]netip.Prefix(nil), n.addrs...), n.gateway
}

// sourceFor picks our address for talking to dst. With several subnets on one wire the
// stack's own choice could take an address the peer would not answer, so we bind explicitly:
// the address whose subnet holds dst, else the one that reaches the gateway.
func (n *netStack) sourceFor(dst netip.Addr) (netip.Addr, error) {
	n.mu.Lock()
	defer n.mu.Unlock()
	if src := n.sourceForLocked(dst); src.IsValid() {
		return src, nil
	}
	if n.gateway.IsValid() {
		if src := n.sourceForLocked(n.gateway); src.IsValid() {
			return src, nil
		}
	}
	if len(n.addrs) == 0 {
		return netip.Addr{}, errors.New("není nastavená žádná adresa (droiddesk-eth ip add CIDR nebo dhcp)")
	}
	return netip.Addr{}, fmt.Errorf("%s není v žádné nastavené podsíti a není výchozí brána (droiddesk-eth ip add / route add default via)", dst)
}

func (n *netStack) sourceForLocked(dst netip.Addr) netip.Addr {
	best := -1
	var src netip.Addr
	for _, p := range n.addrs {
		if p.Contains(dst) && p.Bits() > best {
			best, src = p.Bits(), p.Addr()
		}
	}
	return src
}

func (n *netStack) dialTCP(ctx context.Context, dst netip.AddrPort) (net.Conn, error) {
	src, err := n.sourceFor(dst.Addr())
	if err != nil {
		return nil, err
	}
	return gonet.DialTCPWithBind(ctx, n.s,
		tcpip.FullAddress{NIC: nicID, Addr: toTCPIP(src)},
		tcpip.FullAddress{NIC: nicID, Addr: toTCPIP(dst.Addr()), Port: dst.Port()},
		ipv4.ProtocolNumber)
}

func (n *netStack) dialUDP(dst netip.AddrPort) (*gonet.UDPConn, error) {
	src, err := n.sourceFor(dst.Addr())
	if err != nil {
		return nil, err
	}
	return gonet.DialUDP(n.s,
		&tcpip.FullAddress{NIC: nicID, Addr: toTCPIP(src)},
		&tcpip.FullAddress{NIC: nicID, Addr: toTCPIP(dst.Addr()), Port: dst.Port()},
		ipv4.ProtocolNumber)
}
