package main

import (
	"bufio"
	"flag"
	"fmt"
	"io"
	"net"
	"net/netip"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"
)

// Client side: every command but start talks to the daemon over the control socket.

func dial() net.Conn {
	conn, err := net.Dial("unix", controlPath())
	if err != nil {
		fail("neběží (spusť droiddesk-eth start)")
	}
	return conn
}

func request(line string) (net.Conn, *bufio.Reader) {
	conn := dial()
	if _, err := io.WriteString(conn, line+"\n"); err != nil {
		fail("spojení s démonem selhalo: %v", err)
	}
	return conn, bufio.NewReader(conn)
}

// simple sends a request answered by one "ok" / "err ..." line.
func simple(line string) {
	conn, reader := request(line)
	defer conn.Close()
	answer, _ := reader.ReadString('\n')
	answer = strings.TrimSpace(answer)
	if answer != "ok" {
		fail("%s", strings.TrimPrefix(orNoAnswer(answer), "err "))
	}
}

func orNoAnswer(s string) string {
	if s == "" {
		return "démon neodpověděl"
	}
	return s
}

type status struct {
	fields map[string][]string
	addrs  []string
}

func readStatus() (status, bool) {
	conn, err := net.Dial("unix", controlPath())
	if err != nil {
		return status{}, false
	}
	defer conn.Close()
	io.WriteString(conn, "status\n")
	st := status{fields: map[string][]string{}}
	scanner := bufio.NewScanner(conn)
	for scanner.Scan() {
		line := scanner.Text()
		if line == "" {
			break
		}
		f := strings.Split(line, "\t")
		if f[0] == "addr" {
			st.addrs = append(st.addrs, f[1])
		} else {
			st.fields[f[0]] = f[1:]
		}
	}
	return st, true
}

func (s status) get(key string, i int) string {
	if v := s.fields[key]; len(v) > i {
		return v[i]
	}
	return ""
}

func cmdStart(args []string) {
	fs := flag.NewFlagSet("start", flag.ExitOnError)
	device := fs.String("device", "", "USB device name from the app's list (/dev/bus/usb/BBB/DDD)")
	tapName := fs.String("tap", "", "use a Linux TAP device as the frame source (tests)")
	mac := fs.String("mac", "", "MAC for --tap (default: random, locally administered)")
	socks := fs.String("socks", "127.0.0.1:1090", "SOCKS5 listen address")
	fs.Parse(args)
	if fs.NArg() == 1 && *device == "" {
		*device = fs.Arg(0) // `start DEVICE`, as in the plan
	} else if fs.NArg() > 0 {
		usageExit()
	}
	if _, running := readStatus(); running {
		fail("už běží (droiddesk-eth status)")
	}
	if *tapName == "" && *device == "" {
		*device = pickDevice()
	}

	daemonArgs := []string{daemonCommand, "--socks", *socks}
	if *tapName != "" {
		daemonArgs = append(daemonArgs, "--tap", *tapName)
		if *mac != "" {
			daemonArgs = append(daemonArgs, "--mac", *mac)
		}
	} else {
		daemonArgs = append(daemonArgs, "--device", *device)
	}
	exe, err := os.Executable()
	if err != nil {
		fail("nenašel jsem vlastní binárku: %v", err)
	}
	logFile, err := os.OpenFile(logPath(), os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o600)
	if err != nil {
		fail("log %s: %v", logPath(), err)
	}
	notifyR, notifyW, err := os.Pipe()
	if err != nil {
		fail("%v", err)
	}
	cmd := exec.Command(exe, daemonArgs...)
	cmd.Stdout, cmd.Stderr = logFile, logFile
	cmd.ExtraFiles = []*os.File{notifyW} // fd 3 in the daemon
	// Own session: closing the terminal (or Ctrl+C in it) must not take the daemon down.
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
	if err := cmd.Start(); err != nil {
		fail("spuštění démona selhalo: %v", err)
	}
	notifyW.Close()
	logFile.Close()

	// The phone may show a permission dialog; give the user time to confirm it.
	timer := time.AfterFunc(3*time.Minute, func() {
		cmd.Process.Kill()
		fail("převzetí adaptéru trvá příliš dlouho, démon ukončen (log: %s)", logPath())
	})
	scanner := bufio.NewScanner(notifyR)
	for scanner.Scan() {
		f := strings.Fields(scanner.Text())
		switch {
		case len(f) == 0:
		case f[0] == "permission":
			fmt.Println("Potvrď na telefonu přístup k USB adaptéru…")
		case f[0] == "ok" && len(f) >= 3:
			timer.Stop()
			pid := cmd.Process.Pid
			cmd.Process.Release()
			source := "USB " + *device
			if *tapName != "" {
				source = "TAP " + *tapName
			}
			fmt.Printf("Běží: %s, MAC %s, režim %s (pid %d)\n", source, f[1], f[2], pid)
			if st, ok := readStatus(); ok {
				if strings.HasPrefix(st.get("socks", 1), "chyba") {
					fmt.Printf("SOCKS5 %s nejede: %s\n", st.get("socks", 0), st.get("socks", 1))
				} else {
					fmt.Printf("SOCKS5 %s; adresu přidá droiddesk-eth ip add CIDR nebo dhcp\n", st.get("socks", 0))
				}
			}
			if *tapName == "" {
				fmt.Println("Android teď Ethernet nemá; droiddesk-eth stop ho vrátí.")
			}
			return
		case f[0] == "err":
			timer.Stop()
			fail("%s", strings.TrimSpace(strings.TrimPrefix(scanner.Text(), "err")))
		}
	}
	timer.Stop()
	fail("démon skončil bez odpovědi, konec logu %s:\n%s", logPath(), logTail(10))
}

func logTail(lines int) string {
	data, _ := os.ReadFile(logPath())
	all := strings.Split(strings.TrimRight(string(data), "\n"), "\n")
	if len(all) > lines {
		all = all[len(all)-lines:]
	}
	return strings.Join(all, "\n")
}

// pickDevice: the app marks adapters it can take over with "ecm" in the 9th field; exactly one
// such adapter is chosen without asking.
func pickDevice() string {
	devices, err := listUSB()
	if err != nil {
		fail("%v", err)
	}
	var ecm []usbDevice
	for _, d := range devices {
		if d.Ethernet == "ecm" {
			ecm = append(ecm, d)
		}
	}
	printList := func() {
		if len(devices) == 0 {
			fmt.Fprintln(os.Stderr, "  (žádné USB zařízení)")
		}
		for _, d := range devices {
			note := ""
			if d.Ethernet != "" {
				note = "  [" + d.Ethernet + "]"
			}
			fmt.Fprintf(os.Stderr, "  %-22s %s:%s  %s %s%s\n", d.Name, d.VID, d.PID, d.Maker, d.Product, note)
		}
	}
	switch len(ecm) {
	case 1:
		return ecm[0].Name
	case 0:
		fmt.Fprintln(os.Stderr, "droiddesk-eth: není připojený žádný podporovaný Ethernet adaptér.")
		fmt.Fprintln(os.Stderr, "Podporované jsou adaptéry s CDC-ECM (např. Realtek RTL8153/RTL8156); ASIX AX88179 zatím ne.")
		fmt.Fprintln(os.Stderr, "USB zařízení:")
		printList()
		os.Exit(1)
	default:
		fmt.Fprintln(os.Stderr, "droiddesk-eth: připojených adaptérů je víc, vyber jeden: droiddesk-eth start --device NAME")
		printList()
		os.Exit(1)
	}
	return ""
}

func cmdStop() {
	st, running := readStatus()
	if !running {
		fail("neběží")
	}
	conn, reader := request("stop")
	reader.ReadString('\n')
	conn.Close()
	// Wait until the socket is gone, so a following `start` does not race the old daemon.
	for i := 0; i < 50; i++ {
		if _, err := os.Stat(controlPath()); err != nil {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	if st.get("mode", 0) == "tap" {
		fmt.Println("Zastaveno.")
	} else {
		fmt.Println("Zastaveno, adaptér se vrací Androidu.")
	}
}

func cmdStatus() {
	st, running := readStatus()
	if !running {
		fmt.Println("droiddesk-eth neběží (droiddesk-eth start)")
		os.Exit(3)
	}
	uptime, _ := strconv.Atoi(st.get("uptime", 0))
	mode := st.get("mode", 0)
	source := "USB " + st.get("device", 0)
	if mode == "tap" {
		source = "TAP " + st.get("device", 0)
	}
	fmt.Printf("droiddesk-eth běží %s (pid %s)\n", time.Duration(uptime)*time.Second, st.get("pid", 0))
	fmt.Printf("  Zdroj:     %s, režim %s\n", source, mode)
	mac, _ := net.ParseMAC(st.get("mac", 0))
	fmt.Printf("  MAC:       %s\n", mac)
	if len(st.addrs) == 0 {
		fmt.Println("  Adresy:    žádné (droiddesk-eth ip add CIDR nebo dhcp)")
	} else {
		fmt.Printf("  Adresy:    %s\n", strings.Join(st.addrs, ", "))
	}
	if gw := st.get("gateway", 0); gw != "" {
		fmt.Printf("  Trasa:     výchozí přes %s\n", gw)
	}
	if lease := st.get("dhcp", 0); lease != "" {
		line := fmt.Sprintf("  DHCP:      %s od %s", lease, st.get("dhcp", 1))
		if exp, _ := strconv.ParseInt(st.get("dhcp", 2), 10, 64); exp > 0 {
			line += ", platí do " + time.Unix(exp, 0).Format("15:04:05") + " (neprodlužuje se)"
		}
		fmt.Println(line)
	}
	fmt.Printf("  Přijato:   %s rámců, %s\n", st.get("rx", 0), humanBytes(st.get("rx", 1)))
	fmt.Printf("  Odesláno:  %s rámců, %s\n", st.get("tx", 0), humanBytes(st.get("tx", 1)))
	if drops := st.get("drops", 0); drops != "0" {
		fmt.Printf("  Zahozeno:  %s rámců pro pomalé odběratele (capture/watch)\n", drops)
	}
	fmt.Printf("  SOCKS5:    %s (%s)\n", st.get("socks", 0), st.get("socks", 1))
	fmt.Printf("  Odběratelé rámců: %s\n", st.get("subscribers", 0))
}

func humanBytes(s string) string {
	n, _ := strconv.ParseFloat(s, 64)
	switch {
	case n >= 1<<30:
		return fmt.Sprintf("%.1f GiB", n/(1<<30))
	case n >= 1<<20:
		return fmt.Sprintf("%.1f MiB", n/(1<<20))
	case n >= 1<<10:
		return fmt.Sprintf("%.1f KiB", n/(1<<10))
	}
	return fmt.Sprintf("%.0f B", n)
}

// interrupted closes conn on Ctrl+C / SIGTERM, which ends the streaming command cleanly.
func interrupted(conn net.Conn) chan struct{} {
	signals := make(chan os.Signal, 1)
	signal.Notify(signals, os.Interrupt, syscall.SIGTERM, syscall.SIGHUP)
	stopped := make(chan struct{})
	go func() {
		<-signals
		close(stopped)
		conn.Close()
	}()
	return stopped
}

func cmdCapture(args []string) {
	fs := flag.NewFlagSet("capture", flag.ExitOnError)
	file := fs.String("w", "", "write to FILE instead of stdout")
	fs.Parse(args)
	conn, reader := request("capture")
	answer, err := reader.ReadString('\n')
	if strings.TrimSpace(answer) != "ok" || err != nil {
		fail("%s", strings.TrimPrefix(orNoAnswer(strings.TrimSpace(answer)), "err "))
	}
	var out io.Writer = os.Stdout
	if *file != "" {
		f, err := os.Create(*file)
		if err != nil {
			fail("%v", err)
		}
		defer f.Close()
		out = f
		fmt.Fprintf(os.Stderr, "Zachytávám do %s, Ctrl+C ukončí\n", *file)
	}
	interrupted(conn)
	n, _ := io.Copy(out, reader)
	if *file != "" {
		fmt.Fprintf(os.Stderr, "Zapsáno %s.\n", humanBytes(strconv.FormatInt(n, 10)))
	}
}

func cmdIP(args []string) {
	if len(args) != 2 || (args[0] != "add" && args[0] != "del") {
		fail("použití: droiddesk-eth ip add|del CIDR")
	}
	if !strings.Contains(args[1], "/") {
		fail("chybí délka prefixu, např. %s/24", args[1])
	}
	simple("ip-" + args[0] + " " + args[1])
}

func cmdRoute(args []string) {
	switch {
	case len(args) == 4 && args[0] == "add" && args[1] == "default" && args[2] == "via":
		simple("route-add " + args[3])
	case len(args) == 2 && args[0] == "del" && args[1] == "default":
		simple("route-del")
	default:
		fail("použití: droiddesk-eth route add default via GW | route del default")
	}
}

func cmdDHCP() {
	fmt.Println("Žádám o adresu přes DHCP…")
	conn, reader := request("dhcp")
	defer conn.Close()
	answer, _ := reader.ReadString('\n')
	f := strings.Fields(answer)
	if len(f) < 5 || f[0] != "ok" {
		fail("%s", strings.TrimPrefix(orNoAnswer(strings.TrimSpace(answer)), "err "))
	}
	fmt.Printf("Adresa:  %s (server %s)\n", f[1], f[3])
	if f[2] != "-" {
		fmt.Printf("Brána:   %s\n", f[2])
	}
	if len(f) > 5 && f[5] != "" {
		fmt.Printf("DNS:     %s (jen informace; jména se překládají systémem)\n", f[5])
	}
	if exp, _ := strconv.ParseInt(f[4], 10, 64); exp > 0 {
		fmt.Printf("Platí do %s; neprodlužuje se, při delší práci spusť dhcp znovu.\n", time.Unix(exp, 0).Format("15:04:05"))
	}
}

func cmdArpscan(args []string) {
	fs := flag.NewFlagSet("arpscan", flag.ExitOnError)
	timeout := fs.Float64("timeout", 2, "seconds to wait for late replies")
	cidr := ""
	// Accept the CIDR before or after the flags.
	if len(args) > 0 && !strings.HasPrefix(args[0], "-") {
		cidr, args = args[0], args[1:]
	}
	fs.Parse(args)
	if cidr == "" && fs.NArg() == 1 {
		cidr = fs.Arg(0)
	}
	p, err := netip.ParsePrefix(cidr)
	if err != nil {
		fail("použití: droiddesk-eth arpscan CIDR [--timeout SEC], např. 192.168.140.0/24")
	}
	conn, reader := request(fmt.Sprintf("arpscan %s %d", p.Masked(), int(*timeout*1000)))
	defer conn.Close()
	interrupted(conn)
	macsByIP := map[string][]string{}
	flowbox := 0
	for {
		line, err := reader.ReadString('\n')
		if err != nil {
			break
		}
		f := strings.Fields(line)
		switch {
		case len(f) == 2 && f[0] == "source":
			how := "z adresy " + f[1]
			if f[1] == "0.0.0.0" {
				how = "ARP probe z 0.0.0.0 (vlastní adresa v rozsahu není)"
			}
			fmt.Printf("Skenuji %s, %s…\n", p.Masked(), how)
		case len(f) == 3 && f[0] == "host":
			mac, _ := net.ParseMAC(f[2])
			if isFlowbox(mac) {
				flowbox++
			}
			note := ""
			if prev := macsByIP[f[1]]; len(prev) > 0 {
				note = "  ⚠ konflikt: tutéž IP hlásí i " + strings.Join(prev, ", ")
			}
			macsByIP[f[1]] = append(macsByIP[f[1]], f[2])
			fmt.Printf("  %-16s %s  %s%s\n", f[1], f[2], vendorOf(mac), note)
		case len(f) == 2 && f[0] == "done":
			summary := fmt.Sprintf("Odpovědělo %d zařízení.", len(macsByIP))
			if flowbox > 0 {
				summary += fmt.Sprintf(" ⭐ Flowbox FBX3100: %d.", flowbox)
			}
			fmt.Println(summary)
			return
		case f[0] == "err":
			fail("%s", strings.TrimSpace(strings.TrimPrefix(line, "err")))
		}
	}
	fail("sken přerušen")
}

func cmdPing(args []string) {
	fs := flag.NewFlagSet("ping", flag.ExitOnError)
	count := fs.Int("c", 4, "number of echo requests")
	host := ""
	if len(args) > 0 && !strings.HasPrefix(args[0], "-") {
		host, args = args[0], args[1:]
	}
	fs.Parse(args)
	if host == "" && fs.NArg() == 1 {
		host = fs.Arg(0)
	}
	if host == "" {
		fail("použití: droiddesk-eth ping HOST [-c N]")
	}
	conn, reader := request(fmt.Sprintf("ping %s %d", host, *count))
	defer conn.Close()
	interrupted(conn)
	dst := host
	for {
		line, err := reader.ReadString('\n')
		if err != nil {
			fail("ping přerušen")
		}
		f := strings.Fields(line)
		switch {
		case len(f) == 3 && f[0] == "start":
			dst = f[1]
			fmt.Printf("PING %s z %s\n", f[1], f[2])
		case len(f) == 3 && f[0] == "reply":
			us, _ := strconv.Atoi(f[2])
			fmt.Printf("odpověď od %s: seq=%s čas=%.2f ms\n", dst, f[1], float64(us)/1000)
		case len(f) == 2 && f[0] == "timeout":
			fmt.Printf("seq=%s bez odpovědi\n", f[1])
		case len(f) == 3 && f[0] == "done":
			fmt.Printf("%s/%s odpovědí\n", f[2], f[1])
			if f[2] == "0" {
				os.Exit(1)
			}
			return
		case f[0] == "err":
			fail("%s", strings.TrimSpace(strings.TrimPrefix(line, "err")))
		}
	}
}

func proxychainsConf(socks string) string {
	host, port, err := net.SplitHostPort(socks)
	if err != nil {
		host, port = "127.0.0.1", "1090"
	}
	// No proxy_dns: names are resolved locally (hosts on a diagnostic LAN are mostly IPs),
	// and the SOCKS server only speaks to the wire.
	return "strict_chain\nquiet_mode\ntcp_read_time_out 15000\ntcp_connect_time_out 10000\n" +
		"localnet 127.0.0.0/255.0.0.0\n[ProxyList]\n" + fmt.Sprintf("socks5 %s %s\n", host, port)
}

func cmdRun(args []string) {
	if len(args) == 0 {
		fail("použití: droiddesk-eth run CMD [ARGS...]")
	}
	st, running := readStatus()
	if !running {
		fail("neběží (spusť droiddesk-eth start)")
	}
	path := filepath.Join(tmpDir(), "droiddesk-eth-proxychains.conf")
	if err := os.WriteFile(path, []byte(proxychainsConf(st.get("socks", 0))), 0o600); err != nil {
		fail("%v", err)
	}
	proxychains, err := exec.LookPath("proxychains4")
	if err != nil {
		fail("chybí proxychains4 (Debian: apt install proxychains4, Termux: pkg install proxychains-ng)")
	}
	err = syscall.Exec(proxychains, append([]string{"proxychains4", "-q", "-f", path}, args...), os.Environ())
	fail("spuštění proxychains4 selhalo: %v", err)
}

func cmdForward(args []string) {
	if len(args) != 4 || (args[0] != "tcp" && args[0] != "udp") {
		fail("použití: droiddesk-eth forward tcp|udp LPORT HOST PORT")
	}
	conn, reader := request("forward " + strings.Join(args, " "))
	answer, _ := reader.ReadString('\n')
	f := strings.Fields(answer)
	if len(f) != 2 || f[0] != "ok" {
		fail("%s", strings.TrimPrefix(orNoAnswer(strings.TrimSpace(answer)), "err "))
	}
	fmt.Printf("127.0.0.1:%s/%s -> %s:%s přes droiddesk-eth; Ctrl+C ukončí\n", f[1], args[0], args[2], args[3])
	stopped := interrupted(conn)
	// The daemon forwards until this connection closes.
	io.Copy(io.Discard, reader)
	select {
	case <-stopped:
	default:
		fail("démon skončil")
	}
}
