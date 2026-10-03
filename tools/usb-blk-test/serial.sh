#!/bin/bash
# Runs inside debian:trixie: droiddesk-serial against fake_serial_bridge.py (a simulated
# adapter that only answers with the right parameters). mbpoll (libmodbus) and pyserial
# use the pseudo terminal like a real /dev/ttyUSB0.
#
#   docker run --rm -v "$PWD/tools/usb-blk-test:/t" -v "$PWD/app/android/app/src/main:/src:ro" \
#       debian:trixie bash /t/serial.sh         # ends with SERIAL_ALL_OK
set -u
export DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8 LC_ALL=C.UTF-8
if ! command -v mbpoll >/dev/null; then
    apt-get update -qq >/dev/null && apt-get install -y -qq python3 python3-serial mbpoll procps >/dev/null 2>&1 || exit 1
fi
P=/tmp/prefix
rm -rf "$P"; mkdir -p "$P/tmp"
export DROIDDESK_PREFIX=$P TMPDIR=/tmp/session; mkdir -p /tmp/session
S="python3 /src/assets/droiddesk/droiddesk-serial.py"
python3 /t/fake_serial_bridge.py > /tmp/bridge.log 2>&1 &
sleep 1
fail=0
check() { if [ "$1" -ne 0 ]; then echo "FAIL: $2"; fail=1; else echo "ok: $2"; fi; }
seen() { grep -qx "$1" /tmp/bridge.log; check $? "bridge saw '$1'"; }

out=$($S list); echo "$out"
echo "$out" | grep -q "0403:6001" && ! echo "$out" | grep -q "090c:1000"; check $? "list shows only the adapter"

TTY=$($S open); check $? "open"
[ "$TTY" = "/tmp/session/ttyUSB0" ] && [ -L "$TTY" ]; check $? "path $TTY is a link"
[ "$($S open)" = "$TTY" ]; check $? "second open returns the same port"
sleep 0.5
seen "P 9600 8 1 0"
seen "M 1 1"

# Modbus RTU at 19200 8E1: the "adapter" only answers at these parameters.
r=$(timeout 10 mbpoll -m rtu -a 1 -b 19200 -P even -d 8 -s 1 -t 4 -r 1 -c 4 -1 -o 1 "$TTY" 2>&1)
echo "$r" | tail -5
echo "$r" | grep -q "\[4\]:.*33" ; check $? "mbpoll read 4 registers"
seen "P 19200 8 1 2"
timeout 10 mbpoll -m rtu -a 1 -b 19200 -P even -t 4 -r 11 -1 -o 1 "$TTY" 4321 >/dev/null 2>&1; check $? "mbpoll write register 11"
r=$(timeout 10 mbpoll -m rtu -a 1 -b 19200 -P even -t 4 -r 11 -c 1 -1 -o 1 "$TTY" 2>&1)
echo "$r" | grep -q "\[11\]:.*4321"; check $? "mbpoll read back 4321"
# Wrong parity: no answer, as on a real bus.
timeout 10 mbpoll -m rtu -a 1 -b 19200 -P none -t 4 -r 1 -c 1 -1 -o 0.5 "$TTY" >/dev/null 2>&1
[ $? -ne 0 ]; check $? "no answer with wrong parity"

# pyserial: other formats reach the adapter, echo at 115200 8N1 with 64 KiB of random data.
python3 - "$TTY" <<'EOF'
import os, serial, sys, time
# Each open configures the port in one tcsetattr, as pymodbus and serial_for_url do.
port = serial.Serial(sys.argv[1], 2400, bytesize=7, parity="O", stopbits=2, timeout=0.3)
port.write(b"x"); time.sleep(0.3); port.close()
port = serial.Serial(sys.argv[1], 57600, parity="M", timeout=0.3)
port.write(b"y"); time.sleep(0.3)
port.baudrate = 9600; port.write(b"w"); time.sleep(0.3)  # a later change on the open port
port.close()
port = serial.Serial(sys.argv[1], 115200, timeout=0.3)
time.sleep(0.3); port.reset_input_buffer()
data = os.urandom(65536)
got = bytearray()
for at in range(0, len(data), 4096):
    port.write(data[at:at + 4096])
    deadline = time.time() + 5
    while len(got) < at + 4096 and time.time() < deadline:
        got += port.read(at + 4096 - len(got))
print("echo", len(got), "bytes, same:", bytes(got) == data)
sys.exit(0 if bytes(got) == data else 1)
EOF
check $? "pyserial echo 64 KiB"
seen "P 2400 8 2 1"     # data bits do not come through the pty (always 8)
seen "P 57600 8 1 3"
seen "P 9600 8 1 3"
# Non-standard baud rate through TCSETS2 (pyserial uses BOTHER for it).
python3 -c "import serial,sys,time; p=serial.Serial(sys.argv[1], 250000); p.write(b'z'); time.sleep(0.4)" "$TTY"
seen "P 250000 8 1 0"

# pyserial with even parity (as pymodbus): the pty loses PARENB; "set" fixes the format.
cat > /tmp/rtu.py <<'EOF'
import serial, struct, sys
def crc(d):
    c = 0xFFFF
    for b in d:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return struct.pack("<H", c)
p = serial.Serial(sys.argv[1], 19200, parity="E", timeout=1)
req = bytes([1, 3, 0, 2, 0, 1]); p.write(req + crc(req))
r = p.read(7); print("reply", r.hex())
sys.exit(0 if len(r) == 7 and r[3:5] == struct.pack(">H", 22) else 1)
EOF
python3 /tmp/rtu.py "$TTY" >/dev/null 2>&1; [ $? -ne 0 ]; check $? "pyserial even parity alone: no answer (pty limit)"
sleep 1.5  # the daemon marks the terminal a second after the last change
python3 /tmp/rtu.py "$TTY" 2>&1 | grep -q "Invalid argument"; [ $? -ne 0 ]; check $? "reopen with the same settings: no EINVAL (glibc readback)"
$S set "$TTY" 8E1; check $? "set 8E1"
python3 /tmp/rtu.py "$TTY"; check $? "pyserial Modbus RTU with set 8E1"
$S list | grep -q "19200 8E1 (pevný formát)"; check $? "list shows the fixed format"
$S set "$TTY" auto; check $? "set auto"
$S set "$TTY" 9X1 2>/dev/null; [ $? -ne 0 ]; check $? "bad format refused"

# A program that clears only OPOST (cfmakeraw, picocom) keeps ONLCR from the terminal: the
# marker must never turn that into \n -> \r\n.
python3 - "$TTY" <<'EOF'
import os, sys, termios, time
fd = os.open(sys.argv[1], os.O_RDWR | os.O_NOCTTY)
a = termios.tcgetattr(fd); a[1] &= ~termios.OPOST; a[3] &= ~(termios.ICANON | termios.ECHO)
termios.tcsetattr(fd, termios.TCSANOW, a)
time.sleep(1.5)
os.write(fd, b"a\nb\n"); time.sleep(0.3)
print("oflag", oct(termios.tcgetattr(fd)[1]))
EOF
tail -2 /tmp/bridge.log | grep -qx "D 4"; check $? "no newline translation (4 bytes stay 4)"
# Taken name: refused, the live port keeps its link.
$S open --name ttyUSB0 2>/tmp/err; [ $? -ne 0 ] || [ "$($S list | grep -c ttyUSB0)" = 1 ]; check $? "second open of a live name does not take it over"
[ -e "$TTY" ]; check $? "live link intact"

$S lines "$TTY" --dtr 0; check $? "lines --dtr 0"
sleep 0.3; seen "M 0 1"
$S break /tmp/../$TTY 100; check $? "break"
sleep 0.3; seen "B 100"
out=$($S list); echo "$out"
echo "$out" | grep -q "ttyUSB0.*Ftdi.*DTR 0 RTS 1"; check $? "list shows the open port"

# Unplugged adapter: the session ends, link and state are gone.
PID=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['pid'])" "$P/tmp/droiddesk-serial_ttyUSB0.json")
touch /tmp/unplug
for _ in $(seq 30); do kill -0 "$PID" 2>/dev/null || break; sleep 0.1; done
! kill -0 "$PID" 2>/dev/null && [ ! -e "$TTY" ] && [ ! -e "$P/tmp/droiddesk-serial_ttyUSB0.json" ]; check $? "unplug ends the session and cleans up"
grep -q "device disconnected" "$P/tmp/droiddesk-serial_ttyUSB0.log"; check $? "unplug reason in the log"

# Open again, close by device name; killed daemon (-9) leaves a link that the next open reuses.
TTY=$($S open /dev/bus/usb/001/005 --format 7O2); check $? "reopen with --format 7O2"
sleep 0.3; tail -3 /tmp/bridge.log | grep -qx "P 9600 7 2 1"; check $? "--format reaches the adapter"
$S close /dev/bus/usb/001/005; check $? "close by device"
[ ! -e "$TTY" ]; check $? "close removes the link"
TTY=$($S open); PID=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['pid'])" "$P/tmp/droiddesk-serial_ttyUSB0.json")
kill -9 "$PID"; sleep 0.3
[ "$($S open)" = "/tmp/session/ttyUSB0" ]; check $? "open after kill -9 reuses ttyUSB0"
$S close all >/dev/null; check $? "close all"
$S open /dev/bus/usb/001/002 2>/tmp/err; [ $? -ne 0 ] && grep -q "není podporovaný" /tmp/err; check $? "disk refused"

# As from Debian: its /tmp is the session's TMPDIR on the host (here /hosttmp -> /tmp).
ln -sfn /tmp /hosttmp
D=$(DROIDDESK_DEBIAN_TMP=/hosttmp $S open --name ttyUSB7); check $? "open from Debian"
[ "$D" = /tmp/ttyUSB7 ] && [ -L /tmp/ttyUSB7 ]; check $? "Debian sees /tmp/ttyUSB7"
$S list | grep -q "/hosttmp/ttyUSB7"; check $? "session side sees the host path"
DROIDDESK_DEBIAN_TMP=/hosttmp $S close /tmp/ttyUSB7 >/dev/null && [ ! -e /tmp/ttyUSB7 ]; check $? "close from Debian"

[ $fail -eq 0 ] && echo SERIAL_ALL_OK || echo SERIAL_FAILED
