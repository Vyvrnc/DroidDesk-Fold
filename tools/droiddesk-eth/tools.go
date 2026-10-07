package main

import (
	"bufio"
	"bytes"
	"context"
	"crypto/rand"
	"encoding/binary"
	"errors"
	"io"
	"log"
	"math/bits"
	"net"
	"net/netip"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"gvisor.dev/gvisor/pkg/tcpip"
	"gvisor.dev/gvisor/pkg/tcpip/adapters/gonet"
	"gvisor.dev/gvisor/pkg/tcpip/network/ipv4"
	"gvisor.dev/gvisor/pkg/tcpip/transport/icmp"
	"gvisor.dev/gvisor/pkg/waiter"
)

// Daemon-side tools that work on raw frames (arpscan, dhcp) or on the stack (ping, forward).

func signalNotify(ch chan os.Signal) {
	signal.Notify(ch, syscall.SIGTERM, syscall.SIGINT, syscall.SIGHUP)
}

// arpscan asks every address of the subnet. Without our own address in it we send RFC 5227
// probes (sender 0.0.0.0): hosts answer them, and we do not claim an address on a foreign
// network.
func (d *daemon) arpscan(w *lineWriter, p netip.Prefix, wait time.Duration) {
	if p.Bits() < 16 {
		w.line("err rozsah %s je příliš velký (nejvýš /16)", p)
		return
	}
	src := netip.IPv4Unspecified()
	addrs, _ := d.ns.snapshot()
	for _, a := range addrs {
		if p.Contains(a.Addr()) {
			src = a.Addr()
			break
		}
	}
	w.line("source %s", src)

	sub := d.subscribe(8192)
	defer d.unsubscribe(sub)
	type answer struct {
		ip  netip.Addr
		mac string
	}
	var mu sync.Mutex
	seen := map[answer]bool{}
	answered := map[netip.Addr]bool{}
	stop := make(chan struct{})
	collected := make(chan struct{})
	go func() {
		defer close(collected)
		for {
			select {
			case <-stop:
				return
			case ev := <-sub.ch:
				if ev.out {
					continue
				}
				typ, payload, ok := etherPayload(ev.frame)
				if !ok || typ != etherTypeARP {
					continue
				}
				a, ok := parseARP(payload)
				if !ok || a.Op != 2 || !p.Contains(a.SenderIP) || a.SenderIP == src {
					continue
				}
				key := answer{a.SenderIP, a.SenderMAC.String()}
				mu.Lock()
				if !seen[key] {
					seen[key] = true
					answered[a.SenderIP] = true
					w.line("host %s %s", a.SenderIP, a.SenderMAC)
				}
				mu.Unlock()
			}
		}
	}()

	hosts := hostsOf(p)
	// Two rounds catch hosts that dropped the first request (ARP has no retransmission);
	// big ranges get one round to stay quick.
	rounds := 2
	if len(hosts) > 1024 {
		rounds = 1
	}
	for round := 0; round < rounds; round++ {
		for i, ip := range hosts {
			mu.Lock()
			done := answered[ip]
			mu.Unlock()
			if done || ip == src {
				continue
			}
			d.send(arpRequest(d.mac, src, ip))
			// About 1000 requests/s: fast enough for a /20, gentle for small PLCs and switches.
			if i%8 == 7 {
				time.Sleep(8 * time.Millisecond)
			}
		}
		if round+1 < rounds {
			time.Sleep(300 * time.Millisecond)
		}
	}
	time.Sleep(wait)
	close(stop)
	<-collected
	w.line("done %d", len(seen))
}

func hostsOf(p netip.Prefix) []netip.Addr {
	base := binary.BigEndian.Uint32(p.Addr().AsSlice())
	size := uint32(1) << (32 - p.Bits())
	var out []netip.Addr
	for i := uint32(0); i < size; i++ {
		// Network and broadcast addresses only for /31 and /32, where they are hosts.
		if size > 2 && (i == 0 || i == size-1) {
			continue
		}
		var b [4]byte
		binary.BigEndian.PutUint32(b[:], base+i)
		out = append(out, netip.AddrFrom4(b))
	}
	return out
}

// runDHCP does DISCOVER/OFFER/REQUEST/ACK on raw frames: the stack has no DHCP client and we
// have no address to send from yet. The lease is not renewed; diagnostics sessions are short
// and `dhcp` can simply be run again.
func (d *daemon) runDHCP(w *lineWriter) {
	sub := d.subscribe(4096)
	defer d.unsubscribe(sub)
	var xidBytes [4]byte
	rand.Read(xidBytes[:])
	xid := binary.BigEndian.Uint32(xidBytes[:])
	clientID := append([]byte{1}, d.mac...)
	params := dhcpOption{55, []byte{1, 3, 6, 15, 28, 51, 54}}
	hostname := dhcpOption{12, []byte("droiddesk")}
	sendDHCP := func(msg []byte) {
		d.send(udpFrame(broadcastMAC, d.mac, netip.IPv4Unspecified(), netip.AddrFrom4([4]byte{255, 255, 255, 255}), 68, 67, msg))
	}

	start := time.Now()
	var offer dhcpMessage
	got := false
	for try := 0; try < 4 && !got; try++ {
		secs := uint16(time.Since(start).Seconds())
		sendDHCP(buildDHCP(xid, d.mac, secs, []dhcpOption{{53, []byte{dhcpDiscover}}, {61, clientID}, hostname, params}))
		offer, got = d.waitDHCP(sub, xid, 3*time.Second, dhcpOffer)
	}
	if !got {
		w.line("err žádný DHCP server neodpověděl (4× DISCOVER)")
		return
	}
	server := offer.optAddr(54)
	yi := offer.YIAddr.As4()
	var ack dhcpMessage
	got = false
	for try := 0; try < 3 && !got; try++ {
		secs := uint16(time.Since(start).Seconds())
		opts := []dhcpOption{{53, []byte{dhcpRequest}}, {61, clientID}, {50, yi[:]}, hostname, params}
		if server.IsValid() {
			s4 := server.As4()
			opts = append(opts, dhcpOption{54, s4[:]})
		}
		sendDHCP(buildDHCP(xid, d.mac, secs, opts))
		ack, got = d.waitDHCP(sub, xid, 3*time.Second, dhcpAck, dhcpNak)
	}
	if !got {
		w.line("err server %s nabídl %s, ale nepotvrdil (žádný ACK)", server, offer.YIAddr)
		return
	}
	if ack.Type() == dhcpNak {
		w.line("err server %s adresu odmítl (NAK)", server)
		return
	}
	bitsLen := 24
	if mask := ack.Options[1]; len(mask) == 4 {
		bitsLen = bits.OnesCount32(binary.BigEndian.Uint32(mask))
	}
	lease := &dhcpLease{
		Addr:   netip.PrefixFrom(ack.YIAddr, bitsLen),
		Server: server,
		DNS:    ack.optAddrs(6),
	}
	if routers := ack.optAddrs(3); len(routers) > 0 {
		lease.Router = routers[0]
	}
	if secs := ack.optUint32(51); secs > 0 {
		lease.Expires = time.Now().Add(time.Duration(secs) * time.Second)
	}
	if err := d.ns.addAddress(lease.Addr); err != nil && !strings.Contains(err.Error(), "už je") {
		w.line("err %s", err)
		return
	}
	if _, gw := d.ns.snapshot(); !gw.IsValid() && lease.Router.IsValid() {
		if err := d.ns.setGateway(lease.Router); err != nil {
			log.Printf("dhcp: brána %s: %v", lease.Router, err)
		}
	}
	d.leaseMu.Lock()
	d.lease = lease
	d.leaseMu.Unlock()
	dns := make([]string, len(lease.DNS))
	for i, a := range lease.DNS {
		dns[i] = a.String()
	}
	router := "-"
	if lease.Router.IsValid() {
		router = lease.Router.String()
	}
	expires := int64(0)
	if !lease.Expires.IsZero() {
		expires = lease.Expires.Unix()
	}
	w.line("ok %s %s %s %d %s", lease.Addr, router, server, expires, strings.Join(dns, ","))
}

func (d *daemon) waitDHCP(sub *subscriber, xid uint32, timeout time.Duration, types ...byte) (dhcpMessage, bool) {
	deadline := time.After(timeout)
	for {
		select {
		case <-deadline:
			return dhcpMessage{}, false
		case ev := <-sub.ch:
			if ev.out {
				continue
			}
			typ, payload, ok := etherPayload(ev.frame)
			if !ok || typ != etherTypeIPv4 {
				continue
			}
			_, _, sport, dport, data, ok := udpPayload(payload)
			if !ok || sport != 67 || dport != 68 {
				continue
			}
			m, ok := parseDHCP(data)
			if !ok || m.Op != 2 || m.XID != xid || m.CHAddr.String() != d.mac.String() {
				continue
			}
			for _, t := range types {
				if m.Type() == t {
					return m, true
				}
			}
		}
	}
}

// ping uses the stack's ICMP endpoint (like a Linux ping socket: the stack sets the ident
// and checksum, resolves the neighbour, and gives us the replies for our ident).
func (d *daemon) ping(w *lineWriter, host string, count int) {
	if count <= 0 {
		count = 4
	}
	dst, err := resolveIPv4(host)
	if err != nil {
		w.line("err %s", err)
		return
	}
	src, err := d.ns.sourceFor(dst)
	if err != nil {
		w.line("err %s", err)
		return
	}
	var wq waiter.Queue
	ep, tcpipErr := d.ns.s.NewEndpoint(icmp.ProtocolNumber4, ipv4.ProtocolNumber, &wq)
	if tcpipErr != nil {
		w.line("err %s", tcpipErr)
		return
	}
	defer ep.Close()
	if tcpipErr := ep.Bind(tcpip.FullAddress{NIC: nicID, Addr: toTCPIP(src)}); tcpipErr != nil {
		w.line("err %s", tcpipErr)
		return
	}
	entry, readable := waiter.NewChannelEntry(waiter.ReadableEvents)
	wq.EventRegister(&entry)
	defer wq.EventUnregister(&entry)

	w.line("start %s %s", dst, src)
	received := 0
	to := tcpip.FullAddress{NIC: nicID, Addr: toTCPIP(dst)}
	for seq := 1; seq <= count; seq++ {
		msg := make([]byte, 64)
		msg[0] = 8 // echo request
		binary.BigEndian.PutUint16(msg[6:8], uint16(seq))
		copy(msg[8:], "droiddesk-eth ping")
		sent := time.Now()
		var r bytes.Reader
		r.Reset(msg)
		if _, err := ep.Write(&r, tcpip.WriteOptions{To: &to}); err != nil {
			w.line("err %s", err)
			return
		}
		deadline := time.NewTimer(2 * time.Second)
		replied := false
		for !replied {
			var buf bytes.Buffer
			_, err := ep.Read(&buf, tcpip.ReadOptions{})
			if _, wouldBlock := err.(*tcpip.ErrWouldBlock); wouldBlock {
				select {
				case <-readable:
					continue
				case <-deadline.C:
				}
				break
			}
			if err != nil {
				break
			}
			b := buf.Bytes()
			if len(b) >= 8 && b[0] == 0 && int(binary.BigEndian.Uint16(b[6:8])) == seq {
				replied = true
				received++
				w.line("reply %d %d", seq, time.Since(sent).Microseconds())
			}
		}
		deadline.Stop()
		if !replied {
			w.line("timeout %d", seq)
		}
		if seq < count {
			time.Sleep(time.Until(sent.Add(time.Second)))
		}
	}
	w.line("done %d %d", count, received)
}

// forward listens on 127.0.0.1:LPORT and carries connections/datagrams through the stack for
// as long as the client's control connection stays open.
func (d *daemon) forward(conn net.Conn, reader *bufio.Reader, w *lineWriter, proto, lport, host, port string) {
	dst, err := resolveIPv4(host)
	if err != nil {
		w.line("err %s", err)
		return
	}
	dport, err := strconv.ParseUint(port, 10, 16)
	if err != nil {
		w.line("err neplatný port %s", port)
		return
	}
	if _, err := d.ns.sourceFor(dst); err != nil {
		w.line("err %s", err)
		return
	}
	target := netip.AddrPortFrom(dst, uint16(dport))
	clientGone := make(chan struct{})
	go func() {
		io.Copy(io.Discard, reader)
		close(clientGone)
	}()
	switch proto {
	case "tcp":
		ln, err := net.Listen("tcp", "127.0.0.1:"+lport)
		if err != nil {
			w.line("err %s", err)
			return
		}
		w.line("ok %d", ln.Addr().(*net.TCPAddr).Port)
		go func() {
			<-clientGone
			ln.Close()
		}()
		for {
			local, err := ln.Accept()
			if err != nil {
				return
			}
			go func() {
				ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
				remote, err := d.ns.dialTCP(ctx, target)
				cancel()
				if err != nil {
					log.Printf("forward %s: %v", target, err)
					local.Close()
					return
				}
				relay(local, remote)
			}()
		}
	case "udp":
		pc, err := net.ListenPacket("udp", "127.0.0.1:"+lport)
		if err != nil {
			w.line("err %s", err)
			return
		}
		w.line("ok %d", pc.LocalAddr().(*net.UDPAddr).Port)
		go func() {
			<-clientGone
			pc.Close()
		}()
		d.forwardUDP(pc, target)
	default:
		w.line("err protokol musí být tcp nebo udp")
	}
}

// forwardUDP keeps one stack socket per local client, so answers find their way back.
func (d *daemon) forwardUDP(pc net.PacketConn, target netip.AddrPort) {
	var mu sync.Mutex
	sessions := map[string]*gonet.UDPConn{}
	defer func() {
		mu.Lock()
		for _, s := range sessions {
			s.Close()
		}
		mu.Unlock()
	}()
	buf := make([]byte, 65536)
	for {
		n, from, err := pc.ReadFrom(buf)
		if err != nil {
			return
		}
		mu.Lock()
		session := sessions[from.String()]
		if session == nil {
			session, err = d.ns.dialUDP(target)
			if err != nil {
				mu.Unlock()
				log.Printf("forward udp %s: %v", target, err)
				continue
			}
			sessions[from.String()] = session
			go func(key string, from net.Addr, s *gonet.UDPConn) {
				reply := make([]byte, 65536)
				for {
					// Quiet sessions end after two minutes, like a NAT mapping.
					s.SetReadDeadline(time.Now().Add(2 * time.Minute))
					n, err := s.Read(reply)
					if err != nil {
						break
					}
					pc.WriteTo(reply[:n], from)
				}
				mu.Lock()
				delete(sessions, key)
				mu.Unlock()
				s.Close()
			}(from.String(), from, session)
		}
		mu.Unlock()
		session.Write(buf[:n])
	}
}

// relay copies both ways and passes half-closes on, so request/response protocols that
// shut down their sending side still get their answer.
func relay(a, b net.Conn) {
	var wg sync.WaitGroup
	wg.Add(2)
	pipe := func(dst, src net.Conn) {
		defer wg.Done()
		io.Copy(dst, src)
		if c, ok := dst.(interface{ CloseWrite() error }); ok {
			c.CloseWrite()
		} else {
			dst.Close()
		}
	}
	go pipe(a, b)
	go pipe(b, a)
	wg.Wait()
	a.Close()
	b.Close()
}

// startSocks runs the SOCKS5 server (RFC 1928, no auth, CONNECT only). A busy port is not
// fatal: everything else still works, status says why SOCKS does not.
func (d *daemon) startSocks(addr string) {
	d.socksAddr = addr
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		d.socksState = "chyba: " + err.Error()
		log.Printf("SOCKS5 %s: %v", addr, err)
		return
	}
	d.socksState = "běží"
	go func() {
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			go d.socksConn(conn)
		}
	}()
}

func (d *daemon) socksConn(conn net.Conn) {
	conn.SetDeadline(time.Now().Add(30 * time.Second))
	r := bufio.NewReader(conn)
	head := make([]byte, 2)
	if _, err := io.ReadFull(r, head); err != nil || head[0] != 5 {
		conn.Close()
		return
	}
	methods := make([]byte, head[1])
	if _, err := io.ReadFull(r, methods); err != nil || bytes.IndexByte(methods, 0) < 0 {
		conn.Write([]byte{5, 0xff})
		conn.Close()
		return
	}
	conn.Write([]byte{5, 0})
	req := make([]byte, 4)
	if _, err := io.ReadFull(r, req); err != nil {
		conn.Close()
		return
	}
	reply := func(code byte) { conn.Write([]byte{5, code, 0, 1, 0, 0, 0, 0, 0, 0}) }
	if req[1] != 1 { // CONNECT only
		reply(7)
		conn.Close()
		return
	}
	var dst netip.Addr
	switch req[3] {
	case 1:
		b := make([]byte, 4)
		io.ReadFull(r, b)
		dst = netip.AddrFrom4([4]byte(b))
	case 3:
		n, _ := r.ReadByte()
		name := make([]byte, n)
		io.ReadFull(r, name)
		a, err := resolveIPv4(string(name))
		if err != nil {
			io.ReadFull(r, make([]byte, 2))
			reply(4)
			conn.Close()
			return
		}
		dst = a
	default:
		reply(8) // IPv6: the stack is IPv4 only
		conn.Close()
		return
	}
	portBytes := make([]byte, 2)
	if _, err := io.ReadFull(r, portBytes); err != nil {
		conn.Close()
		return
	}
	target := netip.AddrPortFrom(dst, binary.BigEndian.Uint16(portBytes))
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	remote, err := d.ns.dialTCP(ctx, target)
	cancel()
	if err != nil {
		log.Printf("SOCKS5 %s: %v", target, err)
		reply(socksCode(err))
		conn.Close()
		return
	}
	conn.SetDeadline(time.Time{})
	reply(0)
	// Bytes the client sent right after the request are already in r's buffer.
	if n := r.Buffered(); n > 0 {
		pending, _ := r.Peek(n)
		remote.Write(pending)
	}
	relay(conn, remote)
}

func socksCode(err error) byte {
	text := err.Error()
	switch {
	case strings.Contains(text, "refused"):
		return 5
	case strings.Contains(text, "není") || strings.Contains(text, "route"):
		return 3
	case errors.Is(err, context.DeadlineExceeded) || strings.Contains(text, "timed out"):
		return 4
	}
	return 1
}
