set -u
export HOME=/root
mkdir -p $HOME/.local/share/themes/Fluent-Dark/xfwm4 $HOME/.local/share/themes/Fluent-Dark-xxhdpi/xfwm4
S=/repo/app/android/app/src/main/assets/droiddesk/droiddesk-screen-profile
q() { xfconf-query -c "$1" -p "$2"; }
fail=0
check() { local got; got=$(q "$1" "$2"); if [ "$got" = "$3" ]; then echo "ok   $1 $2 = $got"; else echo "FAIL $1 $2 = $got (want $3)"; fail=1; fi; }
# The Fold's values.
xfconf-query -c xsettings -p /Gdk/WindowScalingFactor -n -t int -s 2
xfconf-query -c xsettings -p /Xft/DPI -n -t int -s 100
xfconf-query -c xsettings -p /Gtk/CursorThemeSize -n -t int -s 60
xfconf-query -c xfwm4 -p /general/theme -n -t string -s Fluent-Dark-xxhdpi
xfconf-query -c xfce4-panel -p /panels -n -t int -s 2 -a
xfconf-query -c xfce4-panel -p /panels/panel-2/size -n -t uint -s 65
xfconf-query -c xfce4-panel -p /panels/panel-2/icon-size -n -t uint -s 55
xfconf-query -c xfce4-panel -p /plugins/plugin-27 -n -t string -s systray
xfconf-query -c xfce4-panel -p /plugins/plugin-27/icon-size -n -t int -s 48
xfconf-query -c xfce4-panel -p /plugins/plugin-26 -n -t string -s genmon
echo "== wide (first run, nothing changes)"; bash $S wide
check xsettings /Gdk/WindowScalingFactor 2
echo "== desktop (defaults)"; bash $S desktop
check xsettings /Gdk/WindowScalingFactor 1; check xsettings /Xft/DPI 96; check xsettings /Gtk/CursorThemeSize 24
check xfwm4 /general/theme Fluent-Dark; check xfce4-panel /panels/panel-2/size 40; check xfce4-panel /panels/panel-2/icon-size 28
check xfce4-panel /plugins/plugin-27/icon-size 24
echo "== user tweaks the monitor"; xfconf-query -c xfce4-panel -p /panels/panel-2/size -s 36
echo "== back to wide (phone values)"; bash $S wide
check xsettings /Gdk/WindowScalingFactor 2; check xsettings /Xft/DPI 100; check xsettings /Gtk/CursorThemeSize 60
check xfwm4 /general/theme Fluent-Dark-xxhdpi; check xfce4-panel /panels/panel-2/size 65; check xfce4-panel /panels/panel-2/icon-size 55
check xfce4-panel /plugins/plugin-27/icon-size 48
echo "== narrow (no profile: takes wide)"; bash $S narrow
check xfce4-panel /panels/panel-2/size 65
echo "== desktop again (keeps the tweak)"; bash $S desktop
check xfce4-panel /panels/panel-2/size 36; check xsettings /Gdk/WindowScalingFactor 1
echo "== same screen again (no-op)"; bash $S desktop; check xfce4-panel /panels/panel-2/size 36
echo "== two runs at once (wide, then desktop twice): the phone profile must survive"
bash $S wide
bash $S desktop & bash $S desktop & wait
bash $S wide
check xsettings /Gdk/WindowScalingFactor 2; check xfce4-panel /panels/panel-2/size 65
echo "== a lock left by a killed run (dead owner) is taken over"
L=$HOME/.config/droiddesk/screen-profiles/.lock
sh -c 'exit 0' & dead=$!; wait $dead
mkdir -p "$L" && echo "$dead" > "$L/pid"
bash $S desktop; rc=$?
[ "$rc" = 0 ] || { echo "FAIL exit $rc with a dead owner's lock"; fail=1; }
check xsettings /Gdk/WindowScalingFactor 1
[ -d "$L" ] && { echo "FAIL lock left behind"; fail=1; }
echo "== a lock of a live owner: busy (exit 75), nothing changed"
sleep 60 & live=$!
mkdir -p "$L" && echo "$live" > "$L/pid"
sed 's/seq 1 100/seq 1 3/' $S > /tmp/sp-short
bash /tmp/sp-short wide; rc=$?
[ "$rc" = 75 ] || { echo "FAIL exit $rc with a live owner's lock"; fail=1; }
check xsettings /Gdk/WindowScalingFactor 1
kill $live; rm -rf "$L"
[ $fail = 0 ] && echo SCREEN_PROFILE_OK || echo SCREEN_PROFILE_FAIL
