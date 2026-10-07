package main

import (
	"bufio"
	"bytes"
	"context"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/netip"
	"os"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

// daemonCommand is the hidden subcommand `start` re-executes itself with.
const daemonCommand = "__daemon"

type frameEvent struct {
	at    time.Time
	frame []byte // shared by all subscribers, never modified
	out   bool
}

// subscriber gets a copy of the frame stream (capture, watch, arpscan, dhcp). A slow reader
// loses frames instead of stalling the wire.
type subscriber struct {
	ch    chan frameEvent
	drops atomic.Uint64
}

type dhcpLease struct {
	Addr    netip.Prefix
	Router  netip.Addr
	Server  netip.Addr
	DNS     []netip.Addr
	Expires time.Time
}

type daemon struct {
	src     FrameSource
	ns      *netStack
	mac     net.HardwareAddr
	started time.Time

	rxFrames, rxBytes, txFrames, txBytes atomic.Uint64
	captureDrops                         atomic.Uint64

	subsMu sync.Mutex
	subs   map[*subscriber]struct{}

	socksAddr, socksState string

	leaseMu sync.Mutex
	lease   *dhcpLease
}

func runDaemon(args []string) {
	fs := flag.NewFlagSet("daemon", flag.ExitOnError)
	device := fs.String("device", "", "")
	tapName := fs.String("tap", "", "")
	macText := fs.String("mac", "", "")
	socksAddr := fs.String("socks", "127.0.0.1:1090", "")
	fs.Parse(args)

	log.SetFlags(log.LstdFlags | log.Lmicroseconds)
	// fd 3 is the pipe to `start`, which waits for the outcome of the takeover.
	notify := func(string) {}
	if f := os.NewFile(3, "notify"); f != nil {
		if _, err := f.Stat(); err == nil {
			notify = func(line string) { fmt.Fprintln(f, line) }
			defer f.Close()
		}
	}

	var src FrameSource
	var err error
	if *tapName != "" {
		var mac net.HardwareAddr
		if *macText != "" {
			if mac, err = net.ParseMAC(*macText); err != nil {
				notify("err neplatná MAC " + *macText)
				log.Fatal(err)
			}
		}
		src, err = openTap(*tapName, mac)
	} else {
		src, err = openBridge(*device, func() {
			log.Print("aplikace ukazuje dialog s oprávněním k USB")
			notify("permission")
		})
	}
	if err != nil {
		notify("err " + err.Error())
		log.Fatal(err)
	}
	ns, err := newNetStack(src.MAC())
	if err != nil {
		notify("err " + err.Error())
		log.Fatal(err)
	}
	d := &daemon{src: src, ns: ns, mac: src.MAC(), started: time.Now(), subs: map[*subscriber]struct{}{}}

	ctl, err := listenControl()
	if err != nil {
		src.Close()
		notify("err " + err.Error())
		log.Fatal(err)
	}
	d.startSocks(*socksAddr)
	go d.inboundLoop()
	go d.outboundLoop()

	signals := make(chan os.Signal, 1)
	signalNotify(signals)
	go func() {
		s := <-signals
		log.Printf("signál %v, končím", s)
		d.shutdown(0)
	}()

	log.Printf("běží: %s %s MAC %s", src.Mode(), src.Name(), src.MAC())
	notify(fmt.Sprintf("ok %s %s", src.MAC(), src.Mode()))
	for {
		conn, err := ctl.Accept()
		if err != nil {
			log.Fatal(err)
		}
		go d.handleControl(conn)
	}
}

func listenControl() (net.Listener, error) {
	path := controlPath()
	if conn, err := net.Dial("unix", path); err == nil {
		conn.Close()
		return nil, fmt.Errorf("droiddesk-eth už běží (%s)", path)
	}
	os.Remove(path) // stale socket of a daemon that died
	old := syscall.Umask(0o077)
	ln, err := net.Listen("unix", path)
	syscall.Umask(old)
	if err != nil {
		return nil, err
	}
	os.Chmod(path, 0o600)
	return ln, nil
}

func (d *daemon) shutdown(code int) {
	// Closing the bridge socket is what gives the adapter back to Android.
	d.src.Close()
	os.Remove(controlPath())
	log.Print("ukončeno")
	os.Exit(code)
}

func (d *daemon) inboundLoop() {
	for {
		frame, err := d.src.ReadFrame()
		if err != nil {
			log.Printf("zdroj rámců skončil: %v", err)
			d.shutdown(1)
		}
		d.rxFrames.Add(1)
		d.rxBytes.Add(uint64(len(frame)))
		d.publish(frameEvent{at: time.Now(), frame: frame})
		// The adapter is promiscuous for capture and watch; only what a NIC would accept goes
		// to the stack.
		if len(frame) >= 14 && (frame[0]&1 == 1 || bytes.Equal(frame[0:6], d.mac)) {
			d.ns.inject(frame)
		}
	}
}

func (d *daemon) outboundLoop() {
	for {
		frame := d.ns.next(context.Background())
		if frame == nil {
			return
		}
		d.send(frame)
	}
}

// send writes a frame from the stack or from a raw tool, so captures see both directions.
func (d *daemon) send(frame []byte) {
	if len(frame) < 60 {
		// Pad to the Ethernet minimum here: not every USB adapter does it in CDC mode.
		frame = append(frame, make([]byte, 60-len(frame))...)
	}
	if err := d.src.WriteFrame(frame); err != nil {
		log.Printf("zápis rámce selhal: %v", err)
		return
	}
	d.txFrames.Add(1)
	d.txBytes.Add(uint64(len(frame)))
	d.publish(frameEvent{at: time.Now(), frame: frame, out: true})
}

func (d *daemon) subscribe(size int) *subscriber {
	s := &subscriber{ch: make(chan frameEvent, size)}
	d.subsMu.Lock()
	d.subs[s] = struct{}{}
	d.subsMu.Unlock()
	return s
}

func (d *daemon) unsubscribe(s *subscriber) {
	d.subsMu.Lock()
	delete(d.subs, s)
	d.subsMu.Unlock()
}

func (d *daemon) publish(ev frameEvent) {
	d.subsMu.Lock()
	defer d.subsMu.Unlock()
	for s := range d.subs {
		select {
		case s.ch <- ev:
		default:
			s.drops.Add(1)
			d.captureDrops.Add(1)
		}
	}
}

// Control protocol: the client sends one line, the daemon answers lines ("ok ...",
// "err ...", data lines) or, for capture, a pcap stream.
func (d *daemon) handleControl(conn net.Conn) {
	defer conn.Close()
	reader := bufio.NewReader(conn)
	line, err := reader.ReadString('\n')
	if err != nil {
		return
	}
	f := strings.Fields(line)
	if len(f) == 0 {
		return
	}
	w := &lineWriter{w: conn}
	switch {
	case f[0] == "status":
		d.writeStatus(w)
	case f[0] == "stop":
		w.line("ok")
		d.shutdown(0)
	case f[0] == "ip-add" && len(f) == 2, f[0] == "ip-del" && len(f) == 2:
		p, err := netip.ParsePrefix(f[1])
		if err != nil || !p.Addr().Is4() {
			w.line("err neplatná adresa " + f[1] + " (čekám IPv4 CIDR, např. 192.168.140.250/24)")
			return
		}
		if f[0] == "ip-add" {
			err = d.ns.addAddress(p)
		} else {
			err = d.ns.delAddress(p)
		}
		w.result(err)
	case f[0] == "route-add" && len(f) == 2:
		gw, err := netip.ParseAddr(f[1])
		if err != nil || !gw.Is4() {
			w.line("err neplatná brána " + f[1])
			return
		}
		w.result(d.ns.setGateway(gw))
	case f[0] == "route-del":
		w.result(d.ns.setGateway(netip.Addr{}))
	case f[0] == "dhcp":
		d.runDHCP(w)
	case f[0] == "arpscan" && len(f) == 3:
		p, err := netip.ParsePrefix(f[1])
		ms, err2 := strconv.Atoi(f[2])
		if err != nil || err2 != nil || !p.Addr().Is4() {
			w.line("err neplatný rozsah " + f[1])
			return
		}
		d.arpscan(w, p.Masked(), time.Duration(ms)*time.Millisecond)
	case f[0] == "ping" && len(f) == 3:
		count, _ := strconv.Atoi(f[2])
		d.ping(w, f[1], count)
	case f[0] == "capture":
		d.capture(conn, reader)
	case f[0] == "forward" && len(f) == 5:
		d.forward(conn, reader, w, f[1], f[2], f[3], f[4])
	default:
		w.line("err neznámý požadavek")
	}
}

// lineWriter serialises answer lines written from several goroutines (arpscan collects
// replies while it still sends).
type lineWriter struct {
	mu sync.Mutex
	w  io.Writer
}

func (l *lineWriter) line(format string, args ...any) {
	l.mu.Lock()
	defer l.mu.Unlock()
	fmt.Fprintf(l.w, format+"\n", args...)
}

func (l *lineWriter) result(err error) {
	if err != nil {
		l.line("err %s", err)
	} else {
		l.line("ok")
	}
}

func (d *daemon) writeStatus(w *lineWriter) {
	addrs, gw := d.ns.snapshot()
	w.line("pid\t%d", os.Getpid())
	w.line("mode\t%s", d.src.Mode())
	w.line("device\t%s", d.src.Name())
	w.line("mac\t%s", d.mac)
	w.line("uptime\t%d", int(time.Since(d.started).Seconds()))
	for _, a := range addrs {
		w.line("addr\t%s", a)
	}
	if gw.IsValid() {
		w.line("gateway\t%s", gw)
	}
	d.leaseMu.Lock()
	if l := d.lease; l != nil {
		w.line("dhcp\t%s\t%s\t%d", l.Addr, l.Server, l.Expires.Unix())
	}
	d.leaseMu.Unlock()
	w.line("rx\t%d\t%d", d.rxFrames.Load(), d.rxBytes.Load())
	w.line("tx\t%d\t%d", d.txFrames.Load(), d.txBytes.Load())
	d.subsMu.Lock()
	w.line("subscribers\t%d", len(d.subs))
	d.subsMu.Unlock()
	w.line("drops\t%d", d.captureDrops.Load())
	w.line("socks\t%s\t%s", d.socksAddr, d.socksState)
	w.line("")
}

// capture streams pcap until the client goes away.
func (d *daemon) capture(conn net.Conn, reader *bufio.Reader) {
	sub := d.subscribe(8192)
	defer d.unsubscribe(sub)
	done := make(chan struct{})
	go func() {
		io.Copy(io.Discard, reader) // returns when the client closes
		close(done)
	}()
	out := bufio.NewWriterSize(conn, 256*1024)
	out.WriteString("ok\n")
	out.Write(pcapHeader())
	out.Flush()
	for {
		select {
		case <-done:
			return
		case ev := <-sub.ch:
			out.Write(pcapRecord(ev.at, ev.frame))
			// Flush when the burst is over, so `tcpdump -r -` shows frames as they come.
			if len(sub.ch) == 0 {
				if err := out.Flush(); err != nil {
					return
				}
			}
		}
	}
}

// resolveIPv4 accepts an address or a name (system resolver: /etc/hosts or Android's DNS,
// not through the stack).
func resolveIPv4(host string) (netip.Addr, error) {
	if a, err := netip.ParseAddr(host); err == nil {
		if !a.Is4() {
			return netip.Addr{}, fmt.Errorf("%s není IPv4", host)
		}
		return a, nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	addrs, err := net.DefaultResolver.LookupNetIP(ctx, "ip4", host)
	if err != nil || len(addrs) == 0 {
		return netip.Addr{}, fmt.Errorf("neznámý host %s", host)
	}
	return addrs[0].Unmap(), nil
}
