# USB serial adapters — plan (2026-10-03)

Goal: USB-RS232/RS485/TTL adapters (FTDI, CP210x, CH340/CH341/CH343/CH9102, PL2303, CDC-ACM) as a
serial port in DroidDesk's Linux, without root: `mbpoll`, `picocom`, pymodbus, vendor tools work
with a path like `/tmp/ttyUSB0`.

## Pieces
- **App (Kotlin)**: usb-serial-for-android (MIT, mik3y, 3.11.0, JitPack) drives the adapter.
  UsbBridge request `serial <name> <port>`: permission, open, then framed data both ways on the
  socket. `list` gets an 8th field: number of serial ports (0 = not a supported adapter).
- **Framing** after the reply line `ok <driver> <ports>`: `type(1) length(2, BE) payload`.
  Client → app: `D` data, `P` parameters (baud u32, data bits u8, stop bits u8 1/2/3=1.5,
  parity u8 0 none/1 odd/2 even/3 mark/4 space), `M` lines (DTR u8, RTS u8), `B` break (ms u16).
  App → client: `D` data, `S` modem status (CTS 1, DSR 2, CD 4, RI 8), `E` error text.
- **Linux side** `droiddesk-serial` (Termux python; also from Debian): opens a pty pair, links
  `$PREFIX/tmp/ttyUSB<n>` (= `/tmp/ttyUSB<n>` in Debian) to the slave, copies bytes, and watches
  the pty's termios (tcgetattr on the master shows what the program set on the slave) to send
  baud/format changes to the adapter. DTR/RTS (a pty has no modem lines) through the daemon's
  control socket: `droiddesk-serial lines /tmp/ttyUSB0 --dtr 1 --rts 0`.
- **Harness**: fake bridge with a simulated Modbus RTU slave behind `serial`; `mbpoll` in Debian
  through the pty (parameters must reach the "adapter"), pyserial echo, open/close/kill.
- **USB disky window** (later): serial adapters listed with a button to open/close the port and
  the path shown.

## Findings from the harness (2026-10-03)
- Linux's pty driver (`pty_set_termios`) forces CS8 and clears PARENB on every change: baud
  (also non-standard through TCSETS2), CSTOPB, PARODD and CMSPAR come through, data bits and
  even parity do not. Even parity is taken from INPCK (libmodbus/mbpoll, picocom set it);
  pymodbus/pyserial need `--format 8E1` / `set` / Formát… in the window.
- glibc 2.41 (Debian trixie) reads the terminal back after tcsetattr and returns EINVAL when
  none of the requested changes took effect, so pyserial opening with even parity at an
  unchanged baud failed. The daemon switches OPOST back on 200 ms after the settings settle;
  every program clears it, so its call always changes something. OPOST alone (no ONLCR, OCRNL,
  OLCUC, XTABS) does not touch the bytes. Remaining edge: two property setters of an open
  pyserial port right after each other (microseconds) with parity on — the second can still
  fail; opening with all parameters (pymodbus, serial_for_url) is fine.
- The slave end stays open in the daemon (raw from the start: a cooked pty would echo the
  adapter's bytes back to it); when nobody reads, bytes beyond the pty buffer are dropped and
  counted (`list`).

## Open
- Timing: Modbus RTU frames go through pty + socket + USB; latency adds to the reply time, not to
  gaps inside a frame (the program writes a frame at once). Inter-frame gaps on the bus are the
  adapter's business.
- RS485 direction: automatic on common adapters (FTDI TXDEN, CH340/CH343 boards with auto-DE).
