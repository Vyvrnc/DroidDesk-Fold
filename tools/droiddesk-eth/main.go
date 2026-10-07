// droiddesk-eth: a userspace network stack on an Ethernet adapter the DroidDesk app took over.
//
// Android gives the USB adapter to the app (no root, no kernel interface for Linux), the app
// carries raw Ethernet frames over the "droiddesk.usb" socket and this program runs TCP/IP on
// them (gVisor netstack) plus raw tools that need to see the wire: capture, ARP scan, watch.
package main

import (
	"fmt"
	"os"
	"path/filepath"
)

const usage = `droiddesk-eth: Linux diagnostics on an Ethernet adapter taken over from Android.

The app hands the USB adapter to Linux (Android has no Ethernet meanwhile); a daemon runs a
userspace TCP/IP stack on its frames:

  droiddesk-eth start [--device NAME]          take the adapter over and start the daemon
                                               (NAME from the app's USB list; picked automatically
                                               when exactly one adapter supports it)
  droiddesk-eth start --tap NAME [--mac MAC]   frames from a Linux TAP instead (tests)
  droiddesk-eth stop                           stop the daemon, the adapter goes back to Android
  droiddesk-eth status                         MAC, mode, addresses, routes, counters, uptime
  droiddesk-eth capture [-w FILE]              pcap of all frames both ways until Ctrl+C:
                                               droiddesk-eth capture | tcpdump -n -r -
  droiddesk-eth ip add CIDR                    e.g. 192.168.140.250/20 (several, any subnets)
  droiddesk-eth ip del CIDR
  droiddesk-eth dhcp                           get a lease, add the address and default route
  droiddesk-eth route add default via GW
  droiddesk-eth route del default
  droiddesk-eth arpscan CIDR [--timeout SEC]   who answers ARP in the subnet (IP, MAC, vendor)
  droiddesk-eth watch                          passive: DHCP requests, ARP probes/announcements,
                                               LLDP/CDP neighbours, every MAC+IP seen
  droiddesk-eth ping HOST [-c N]               ICMP echo through the stack
  droiddesk-eth run CMD [ARGS...]              CMD through the SOCKS5 proxy (proxychains4)
  droiddesk-eth forward tcp|udp LPORT HOST PORT
                                               127.0.0.1:LPORT -> HOST:PORT through the stack;
                                               Ctrl+C ends it

SOCKS5 on 127.0.0.1:1090 runs with the daemon (TCP CONNECT; curl --socks5 127.0.0.1:1090).
Control socket and log: $TMPDIR/droiddesk-eth.sock, $TMPDIR/droiddesk-eth.log (TMPDIR=/tmp).`

func tmpDir() string {
	if dir := os.Getenv("TMPDIR"); dir != "" {
		return dir
	}
	return "/tmp"
}

func controlPath() string { return filepath.Join(tmpDir(), "droiddesk-eth.sock") }
func logPath() string     { return filepath.Join(tmpDir(), "droiddesk-eth.log") }

// fail prints a user-facing message (Czech, like the other droiddesk tools) and exits.
func fail(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "droiddesk-eth: "+format+"\n", args...)
	os.Exit(1)
}

func usageExit() {
	fmt.Fprintln(os.Stderr, usage)
	os.Exit(2)
}

func main() {
	args := os.Args[1:]
	if len(args) == 0 || args[0] == "-h" || args[0] == "--help" || args[0] == "help" {
		fmt.Println(usage)
		return
	}
	command, rest := args[0], args[1:]
	switch command {
	case "start":
		cmdStart(rest)
	case daemonCommand:
		runDaemon(rest)
	case "stop":
		cmdStop()
	case "status":
		cmdStatus()
	case "capture":
		cmdCapture(rest)
	case "ip":
		cmdIP(rest)
	case "dhcp":
		cmdDHCP()
	case "route":
		cmdRoute(rest)
	case "arpscan":
		cmdArpscan(rest)
	case "watch":
		cmdWatch(rest)
	case "ping":
		cmdPing(rest)
	case "run":
		cmdRun(rest)
	case "forward":
		cmdForward(rest)
	default:
		usageExit()
	}
}
