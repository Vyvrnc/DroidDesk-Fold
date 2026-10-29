#!/bin/bash
# droiddesk-screen-profile against a real xfconfd (Debian, Docker), from the repo root:
#   docker run --rm -v "$PWD:/repo" debian:trixie bash /repo/tools/screen-profile-test/run.sh
apt-get update -qq >/dev/null && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq xfconf dbus dbus-bin >/dev/null 2>&1 || exit 1
exec dbus-run-session -- bash /repo/tools/screen-profile-test/cases.sh
