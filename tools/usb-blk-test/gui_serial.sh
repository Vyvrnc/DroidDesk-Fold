#!/bin/bash
# USB disky window with a serial adapter (fake_serial_bridge.py) under Xvfb; screenshot /t/gui_serial.png.
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null && apt-get install -y -qq python3 python3-gi gir1.2-gtk-3.0 xvfb adwaita-icon-theme fonts-dejavu-core >/dev/null || exit 1
P=/tmp/prefix; mkdir -p $P/tmp /tmp/session
export DROIDDESK_PREFIX=$P TMPDIR=/tmp/session
python3 /t/fake_serial_bridge.py > /tmp/bridge.log 2>&1 &
sleep 1
Xvfb :9 -screen 0 1400x700x24 >/dev/null 2>&1 &
sleep 2
DISPLAY=:9 timeout 60 python3 /t/gui_serial_test.py 2>&1 | grep -v "Gtk-WARNING\|dbind-WARNING"
echo "== bridge"; cat /tmp/bridge.log
