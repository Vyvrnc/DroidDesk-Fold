package main

import (
	"bufio"
	"crypto/rand"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

// FrameSource carries whole Ethernet frames (no FCS) to and from the wire.
type FrameSource interface {
	ReadFrame() ([]byte, error)
	WriteFrame([]byte) error
	MAC() net.HardwareAddr
	Mode() string // "ecm", "tap", ...
	Name() string // the USB device or TAP name
	Close() error
}

const bridgeSocket = "@droiddesk.usb"

// usbDevice is one line of the app's "list" answer.
type usbDevice struct {
	Name, VID, PID, Storage, Maker, Product, Held, Serial, Ethernet string
}

// listUSB asks the app for the attached USB devices (tab separated, ends with an empty line).
func listUSB() ([]usbDevice, error) {
	conn, err := net.DialTimeout("unix", bridgeSocket, 3*time.Second)
	if err != nil {
		return nil, errNoApp
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(10 * time.Second))
	if _, err := io.WriteString(conn, "list\n"); err != nil {
		return nil, err
	}
	var devices []usbDevice
	reader := bufio.NewReader(conn)
	for {
		line, err := reader.ReadString('\n')
		line = strings.TrimRight(line, "\r\n")
		if line == "" {
			return devices, nil
		}
		f := strings.Split(line, "\t")
		for len(f) < 9 {
			f = append(f, "")
		}
		devices = append(devices, usbDevice{f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]})
		if err != nil {
			return devices, nil
		}
	}
}

var errNoApp = errors.New("aplikace DroidDesk neběží (chybí USB most @droiddesk.usb)")

// bridgeSource is the app's USB bridge: after the "eth" handshake the socket carries frames
// as a 2-byte big-endian length followed by the frame.
type bridgeSource struct {
	conn    net.Conn
	reader  *bufio.Reader
	writeMu sync.Mutex
	mac     net.HardwareAddr
	mode    string
	device  string
}

// openBridge takes the adapter over. onPermission runs when the phone shows the USB
// permission dialog, so the user knows why nothing happens.
func openBridge(device string, onPermission func()) (*bridgeSource, error) {
	conn, err := net.Dial("unix", bridgeSocket)
	if err != nil {
		return nil, errNoApp
	}
	if _, err := io.WriteString(conn, "eth "+device+"\n"); err != nil {
		conn.Close()
		return nil, err
	}
	// The same buffered reader keeps serving the frames, so nothing read past the answer line
	// is lost.
	reader := bufio.NewReaderSize(conn, 64*1024)
	for {
		line, err := reader.ReadString('\n')
		if err != nil {
			conn.Close()
			return nil, errors.New("aplikace zavřela spojení bez odpovědi")
		}
		fields := strings.Fields(line)
		switch {
		case len(fields) == 0:
			continue
		case fields[0] == "permission":
			if onPermission != nil {
				onPermission()
			}
		case fields[0] == "ok" && len(fields) >= 3:
			mac, err := net.ParseMAC(fields[1])
			if err != nil {
				conn.Close()
				return nil, fmt.Errorf("neplatná MAC od aplikace: %q", fields[1])
			}
			return &bridgeSource{conn: conn, reader: reader, mac: mac, mode: fields[2], device: device}, nil
		case fields[0] == "err":
			conn.Close()
			return nil, errors.New(strings.TrimSpace(strings.TrimPrefix(strings.TrimSpace(line), "err")))
		default:
			conn.Close()
			return nil, fmt.Errorf("nečekaná odpověď aplikace: %q", strings.TrimSpace(line))
		}
	}
}

func (b *bridgeSource) ReadFrame() ([]byte, error) {
	var head [2]byte
	if _, err := io.ReadFull(b.reader, head[:]); err != nil {
		return nil, err
	}
	frame := make([]byte, binary.BigEndian.Uint16(head[:]))
	if _, err := io.ReadFull(b.reader, frame); err != nil {
		return nil, err
	}
	return frame, nil
}

func (b *bridgeSource) WriteFrame(frame []byte) error {
	// One write per frame keeps the length and the frame together even with several writers.
	out := make([]byte, 2+len(frame))
	binary.BigEndian.PutUint16(out, uint16(len(frame)))
	copy(out[2:], frame)
	b.writeMu.Lock()
	defer b.writeMu.Unlock()
	_, err := b.conn.Write(out)
	return err
}

func (b *bridgeSource) MAC() net.HardwareAddr { return b.mac }
func (b *bridgeSource) Mode() string          { return b.mode }
func (b *bridgeSource) Name() string          { return b.device }
func (b *bridgeSource) Close() error          { return b.conn.Close() }

// tapSource reads a Linux TAP device: the test harness bridges it to namespaces with real
// hosts, which exercises everything except USB.
type tapSource struct {
	file    *os.File
	name    string
	mac     net.HardwareAddr
	writeMu sync.Mutex
}

const (
	tunSetIff = 0x400454ca
	iffTap    = 0x0002
	iffNoPi   = 0x1000
)

func openTap(name string, mac net.HardwareAddr) (*tapSource, error) {
	fd, err := syscall.Open("/dev/net/tun", syscall.O_RDWR|syscall.O_CLOEXEC, 0)
	if err != nil {
		return nil, fmt.Errorf("/dev/net/tun: %w", err)
	}
	var ifr [40]byte
	copy(ifr[:16], name)
	binary.NativeEndian.PutUint16(ifr[16:], iffTap|iffNoPi)
	if _, _, errno := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), tunSetIff, uintptr(unsafe.Pointer(&ifr[0]))); errno != 0 {
		syscall.Close(fd)
		return nil, fmt.Errorf("TAP %s: %w", name, errno)
	}
	// Non-blocking lets the Go poller wake a blocked read when the source is closed.
	syscall.SetNonblock(fd, true)
	if mac == nil {
		mac = randomMAC()
	}
	return &tapSource{file: os.NewFile(uintptr(fd), "/dev/net/tun"), name: name, mac: mac}, nil
}

// randomMAC is locally administered and unicast, so it never collides with a vendor OUI.
func randomMAC() net.HardwareAddr {
	mac := make(net.HardwareAddr, 6)
	rand.Read(mac)
	mac[0] = mac[0]&0xfc | 0x02
	return mac
}

func (t *tapSource) ReadFrame() ([]byte, error) {
	buf := make([]byte, 65536)
	n, err := t.file.Read(buf)
	if err != nil {
		return nil, err
	}
	return buf[:n], nil
}

func (t *tapSource) WriteFrame(frame []byte) error {
	t.writeMu.Lock()
	defer t.writeMu.Unlock()
	_, err := t.file.Write(frame)
	return err
}

func (t *tapSource) MAC() net.HardwareAddr { return t.mac }
func (t *tapSource) Mode() string          { return "tap" }
func (t *tapSource) Name() string          { return t.name }
func (t *tapSource) Close() error          { return t.file.Close() }
