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
echo "== wide (first run, nothing changes)"; bash $S wide
check xsettings /Gdk/WindowScalingFactor 2
echo "== desktop (defaults)"; bash $S desktop
check xsettings /Gdk/WindowScalingFactor 1; check xsettings /Xft/DPI 96; check xsettings /Gtk/CursorThemeSize 24
check xfwm4 /general/theme Fluent-Dark; check xfce4-panel /panels/panel-2/size 40; check xfce4-panel /panels/panel-2/icon-size 28
echo "== user tweaks the monitor"; xfconf-query -c xfce4-panel -p /panels/panel-2/size -s 36
echo "== back to wide (phone values)"; bash $S wide
check xsettings /Gdk/WindowScalingFactor 2; check xsettings /Xft/DPI 100; check xsettings /Gtk/CursorThemeSize 60
check xfwm4 /general/theme Fluent-Dark-xxhdpi; check xfce4-panel /panels/panel-2/size 65; check xfce4-panel /panels/panel-2/icon-size 55
echo "== narrow (no profile: takes wide)"; bash $S narrow
check xfce4-panel /panels/panel-2/size 65
echo "== desktop again (keeps the tweak)"; bash $S desktop
check xfce4-panel /panels/panel-2/size 36; check xsettings /Gdk/WindowScalingFactor 1
echo "== same screen again (no-op)"; bash $S desktop; check xfce4-panel /panels/panel-2/size 36
[ $fail = 0 ] && echo SCREEN_PROFILE_OK || echo SCREEN_PROFILE_FAIL
