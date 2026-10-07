package main

import (
	"encoding/binary"
	"errors"
	"io"
	"net"
	"net/netip"
	"time"
)

// Hand-built frames for what must bypass the stack: ARP probes from 0.0.0.0, DHCP before we
// own an address, and parsing of everything we only listen to.

const (
	etherTypeIPv4 = 0x0800
	etherTypeARP  = 0x0806
	etherTypeVLAN = 0x8100
	etherTypeLLDP = 0x88cc
)

var broadcastMAC = net.HardwareAddr{0xff, 0xff, 0xff, 0xff, 0xff, 0xff}

// etherPayload strips the Ethernet header and one or two VLAN tags (managed switches mirror
// tagged traffic). For 802.3 frames (length instead of a type, e.g. CDP) it returns the
// length as the type, so the caller can tell.
func etherPayload(frame []byte) (etherType uint16, payload []byte, ok bool) {
	if len(frame) < 14 {
		return 0, nil, false
	}
	etherType = binary.BigEndian.Uint16(frame[12:14])
	payload = frame[14:]
	for etherType == etherTypeVLAN || etherType == 0x88a8 {
		if len(payload) < 4 {
			return 0, nil, false
		}
		etherType = binary.BigEndian.Uint16(payload[2:4])
		payload = payload[4:]
	}
	return etherType, payload, true
}

func ethernetFrame(dst, src net.HardwareAddr, etherType uint16, payload []byte) []byte {
	frame := make([]byte, 14+len(payload))
	copy(frame[0:6], dst)
	copy(frame[6:12], src)
	binary.BigEndian.PutUint16(frame[12:14], etherType)
	copy(frame[14:], payload)
	return frame
}

// arpPacket is an Ethernet/IPv4 ARP message.
type arpPacket struct {
	Op        uint16 // 1 request, 2 reply
	SenderMAC net.HardwareAddr
	SenderIP  netip.Addr
	TargetMAC net.HardwareAddr
	TargetIP  netip.Addr
}

func parseARP(payload []byte) (arpPacket, bool) {
	if len(payload) < 28 || binary.BigEndian.Uint16(payload[0:2]) != 1 ||
		binary.BigEndian.Uint16(payload[2:4]) != etherTypeIPv4 || payload[4] != 6 || payload[5] != 4 {
		return arpPacket{}, false
	}
	return arpPacket{
		Op:        binary.BigEndian.Uint16(payload[6:8]),
		SenderMAC: net.HardwareAddr(append([]byte(nil), payload[8:14]...)),
		SenderIP:  netip.AddrFrom4([4]byte(payload[14:18])),
		TargetMAC: net.HardwareAddr(append([]byte(nil), payload[18:24]...)),
		TargetIP:  netip.AddrFrom4([4]byte(payload[24:28])),
	}, true
}

func arpRequest(srcMAC net.HardwareAddr, srcIP, target netip.Addr) []byte {
	p := make([]byte, 28)
	binary.BigEndian.PutUint16(p[0:2], 1)
	binary.BigEndian.PutUint16(p[2:4], etherTypeIPv4)
	p[4], p[5] = 6, 4
	binary.BigEndian.PutUint16(p[6:8], 1)
	copy(p[8:14], srcMAC)
	s4, t4 := srcIP.As4(), target.As4()
	copy(p[14:18], s4[:])
	copy(p[24:28], t4[:])
	return ethernetFrame(broadcastMAC, srcMAC, etherTypeARP, p)
}

func checksum(data []byte) uint16 {
	var sum uint32
	for i := 0; i+1 < len(data); i += 2 {
		sum += uint32(binary.BigEndian.Uint16(data[i:]))
	}
	if len(data)%2 == 1 {
		sum += uint32(data[len(data)-1]) << 8
	}
	for sum > 0xffff {
		sum = sum>>16 + sum&0xffff
	}
	return ^uint16(sum)
}

// udpFrame builds Ethernet/IPv4/UDP; the UDP checksum is optional in IPv4 and left zero.
func udpFrame(dstMAC, srcMAC net.HardwareAddr, src, dst netip.Addr, sport, dport uint16, data []byte) []byte {
	ip := make([]byte, 20+8+len(data))
	ip[0] = 0x45
	binary.BigEndian.PutUint16(ip[2:4], uint16(len(ip)))
	ip[8] = 64
	ip[9] = 17
	s4, d4 := src.As4(), dst.As4()
	copy(ip[12:16], s4[:])
	copy(ip[16:20], d4[:])
	binary.BigEndian.PutUint16(ip[10:12], checksum(ip[:20]))
	udp := ip[20:]
	binary.BigEndian.PutUint16(udp[0:2], sport)
	binary.BigEndian.PutUint16(udp[2:4], dport)
	binary.BigEndian.PutUint16(udp[4:6], uint16(8+len(data)))
	copy(udp[8:], data)
	return ethernetFrame(dstMAC, srcMAC, etherTypeIPv4, ip)
}

// udpPayload returns the IPv4 addresses, ports and data of a UDP packet.
func udpPayload(ipPacket []byte) (src, dst netip.Addr, sport, dport uint16, data []byte, ok bool) {
	if len(ipPacket) < 20 || ipPacket[0]>>4 != 4 || ipPacket[9] != 17 {
		return
	}
	ihl := int(ipPacket[0]&0x0f) * 4
	if ihl < 20 || len(ipPacket) < ihl+8 {
		return
	}
	// Fragments other than the first carry no UDP header.
	if binary.BigEndian.Uint16(ipPacket[6:8])&0x1fff != 0 {
		return
	}
	src = netip.AddrFrom4([4]byte(ipPacket[12:16]))
	dst = netip.AddrFrom4([4]byte(ipPacket[16:20]))
	udp := ipPacket[ihl:]
	sport = binary.BigEndian.Uint16(udp[0:2])
	dport = binary.BigEndian.Uint16(udp[2:4])
	end := int(binary.BigEndian.Uint16(udp[4:6]))
	if end < 8 || end > len(udp) {
		end = len(udp)
	}
	return src, dst, sport, dport, udp[8:end], true
}

// DHCP (RFC 2131): only what a client and a passive listener need.

const (
	dhcpDiscover = 1
	dhcpOffer    = 2
	dhcpRequest  = 3
	dhcpDecline  = 4
	dhcpAck      = 5
	dhcpNak      = 6
	dhcpRelease  = 7
	dhcpInform   = 8
)

var dhcpTypeNames = map[byte]string{
	dhcpDiscover: "DISCOVER", dhcpOffer: "OFFER", dhcpRequest: "REQUEST", dhcpDecline: "DECLINE",
	dhcpAck: "ACK", dhcpNak: "NAK", dhcpRelease: "RELEASE", dhcpInform: "INFORM",
}

type dhcpMessage struct {
	Op      byte
	XID     uint32
	CIAddr  netip.Addr
	YIAddr  netip.Addr
	CHAddr  net.HardwareAddr
	Options map[byte][]byte
}

func (m dhcpMessage) Type() byte {
	if v := m.Options[53]; len(v) == 1 {
		return v[0]
	}
	return 0
}

func (m dhcpMessage) optAddr(code byte) netip.Addr {
	if v := m.Options[code]; len(v) >= 4 {
		return netip.AddrFrom4([4]byte(v[:4]))
	}
	return netip.Addr{}
}

func (m dhcpMessage) optAddrs(code byte) []netip.Addr {
	var out []netip.Addr
	v := m.Options[code]
	for len(v) >= 4 {
		out = append(out, netip.AddrFrom4([4]byte(v[:4])))
		v = v[4:]
	}
	return out
}

func (m dhcpMessage) optUint32(code byte) uint32 {
	if v := m.Options[code]; len(v) == 4 {
		return binary.BigEndian.Uint32(v)
	}
	return 0
}

var dhcpMagic = []byte{99, 130, 83, 99}

func parseDHCP(data []byte) (dhcpMessage, bool) {
	if len(data) < 240 || string(data[236:240]) != string(dhcpMagic) || data[1] != 1 || data[2] != 6 {
		return dhcpMessage{}, false
	}
	m := dhcpMessage{
		Op:      data[0],
		XID:     binary.BigEndian.Uint32(data[4:8]),
		CIAddr:  netip.AddrFrom4([4]byte(data[12:16])),
		YIAddr:  netip.AddrFrom4([4]byte(data[16:20])),
		CHAddr:  net.HardwareAddr(append([]byte(nil), data[28:34]...)),
		Options: map[byte][]byte{},
	}
	opts := data[240:]
	for len(opts) > 0 {
		code := opts[0]
		if code == 255 {
			break
		}
		if code == 0 {
			opts = opts[1:]
			continue
		}
		if len(opts) < 2 || len(opts) < 2+int(opts[1]) {
			break
		}
		m.Options[code] = append(m.Options[code], opts[2:2+int(opts[1])]...)
		opts = opts[2+int(opts[1]):]
	}
	return m, true
}

type dhcpOption struct {
	code byte
	data []byte
}

func buildDHCP(xid uint32, mac net.HardwareAddr, secs uint16, options []dhcpOption) []byte {
	b := make([]byte, 240, 300)
	b[0], b[1], b[2] = 1, 1, 6
	binary.BigEndian.PutUint32(b[4:8], xid)
	binary.BigEndian.PutUint16(b[8:10], secs)
	// Broadcast flag: we have no address yet, so the server must not unicast the answer to
	// an IP nobody holds.
	binary.BigEndian.PutUint16(b[10:12], 0x8000)
	copy(b[28:34], mac)
	copy(b[236:240], dhcpMagic)
	for _, o := range options {
		b = append(b, o.code, byte(len(o.data)))
		b = append(b, o.data...)
	}
	b = append(b, 255)
	for len(b) < 300 {
		b = append(b, 0) // some servers ignore requests shorter than BOOTP's 300 bytes
	}
	return b
}

// LLDP (IEEE 802.1AB) and CDP: what the neighbouring switch says about itself.

type neighbour struct {
	Protocol, System, Port, PortDesc, Platform string
	MgmtIP                                     netip.Addr
}

func parseLLDP(payload []byte) (neighbour, bool) {
	n := neighbour{Protocol: "LLDP"}
	for len(payload) >= 2 {
		head := binary.BigEndian.Uint16(payload[0:2])
		typ, length := head>>9, int(head&0x1ff)
		if len(payload) < 2+length {
			break
		}
		value := payload[2 : 2+length]
		payload = payload[2+length:]
		switch typ {
		case 0:
			return n, true
		case 2: // port ID: subtype byte, then the ID (MAC for subtype 3)
			if len(value) > 1 {
				n.Port = lldpID(value)
			}
		case 4:
			n.PortDesc = string(value)
		case 5:
			n.System = string(value)
		case 6:
			n.Platform = string(value)
		case 8: // management address: length, subtype (1 = IPv4), address
			if len(value) >= 6 && value[0] == 5 && value[1] == 1 {
				n.MgmtIP = netip.AddrFrom4([4]byte(value[2:6]))
			}
		case 1:
			if n.System == "" && len(value) > 1 {
				n.System = lldpID(value)
			}
		}
	}
	return n, true
}

func lldpID(value []byte) string {
	if value[0] == 3 && len(value) == 7 { // MAC address subtype
		return net.HardwareAddr(value[1:]).String()
	}
	return string(value[1:])
}

var cdpDest = net.HardwareAddr{0x01, 0x00, 0x0c, 0xcc, 0xcc, 0xcc}

// parseCDP takes the 802.3 payload (LLC/SNAP + CDP).
func parseCDP(payload []byte) (neighbour, bool) {
	// LLC AA AA 03, SNAP OUI 00 00 0C, protocol 0x2000
	if len(payload) < 12 || payload[0] != 0xaa || payload[1] != 0xaa || payload[2] != 0x03 ||
		payload[3] != 0 || payload[4] != 0 || payload[5] != 0x0c || binary.BigEndian.Uint16(payload[6:8]) != 0x2000 {
		return neighbour{}, false
	}
	n := neighbour{Protocol: "CDP"}
	tlvs := payload[12:] // version, TTL, checksum
	for len(tlvs) >= 4 {
		typ := binary.BigEndian.Uint16(tlvs[0:2])
		length := int(binary.BigEndian.Uint16(tlvs[2:4]))
		if length < 4 || length > len(tlvs) {
			break
		}
		value := tlvs[4:length]
		tlvs = tlvs[length:]
		switch typ {
		case 0x0001:
			n.System = string(value)
		case 0x0003:
			n.Port = string(value)
		case 0x0006:
			n.Platform = string(value)
		case 0x0002, 0x0016: // addresses / management addresses
			if ip, ok := cdpFirstIPv4(value); ok && !n.MgmtIP.IsValid() {
				n.MgmtIP = ip
			}
		}
	}
	return n, true
}

func cdpFirstIPv4(value []byte) (netip.Addr, bool) {
	if len(value) < 4 {
		return netip.Addr{}, false
	}
	count := binary.BigEndian.Uint32(value[0:4])
	rest := value[4:]
	for i := uint32(0); i < count && len(rest) >= 2; i++ {
		protoLen := int(rest[1])
		if len(rest) < 2+protoLen+2 {
			break
		}
		proto := rest[2 : 2+protoLen]
		addrLen := int(binary.BigEndian.Uint16(rest[2+protoLen:]))
		addr := rest[4+protoLen:]
		if len(addr) < addrLen {
			break
		}
		if protoLen == 1 && proto[0] == 0xcc && addrLen == 4 {
			return netip.AddrFrom4([4]byte(addr[:4])), true
		}
		rest = addr[addrLen:]
	}
	return netip.Addr{}, false
}

// pcap (classic format, LINKTYPE_ETHERNET): readable by tcpdump, Wireshark and tshark.

func pcapHeader() []byte {
	h := make([]byte, 24)
	binary.LittleEndian.PutUint32(h[0:], 0xa1b2c3d4)
	binary.LittleEndian.PutUint16(h[4:], 2)
	binary.LittleEndian.PutUint16(h[6:], 4)
	binary.LittleEndian.PutUint32(h[16:], 65535)
	binary.LittleEndian.PutUint32(h[20:], 1)
	return h
}

func pcapRecord(t time.Time, frame []byte) []byte {
	r := make([]byte, 16+len(frame))
	binary.LittleEndian.PutUint32(r[0:], uint32(t.Unix()))
	binary.LittleEndian.PutUint32(r[4:], uint32(t.Nanosecond()/1000))
	binary.LittleEndian.PutUint32(r[8:], uint32(len(frame)))
	binary.LittleEndian.PutUint32(r[12:], uint32(len(frame)))
	copy(r[16:], frame)
	return r
}

// pcapReader reads back our own stream (little endian, as written above).
type pcapReader struct{ r io.Reader }

func newPcapReader(r io.Reader) (*pcapReader, error) {
	head := make([]byte, 24)
	if _, err := io.ReadFull(r, head); err != nil {
		return nil, err
	}
	if binary.LittleEndian.Uint32(head) != 0xa1b2c3d4 {
		return nil, errors.New("neplatný pcap")
	}
	return &pcapReader{r}, nil
}

func (p *pcapReader) next() (time.Time, []byte, error) {
	var head [16]byte
	if _, err := io.ReadFull(p.r, head[:]); err != nil {
		return time.Time{}, nil, err
	}
	t := time.Unix(int64(binary.LittleEndian.Uint32(head[0:])), int64(binary.LittleEndian.Uint32(head[4:]))*1000)
	frame := make([]byte, binary.LittleEndian.Uint32(head[8:]))
	if _, err := io.ReadFull(p.r, frame); err != nil {
		return time.Time{}, nil, err
	}
	return t, frame, nil
}
