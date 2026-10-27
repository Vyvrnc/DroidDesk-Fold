#!/usr/bin/env python3
"""droiddesk-camera: which phone camera Linux programs get (DroidDesk CameraBridge).

  droiddesk-camera              same as status
  droiddesk-camera status       idle, or the running stream (camera, size, rotation)
  droiddesk-camera front        switch a running call to the front camera
  droiddesk-camera back         switch a running call to the back camera

Firefox (firefox-cam) sees two cameras, "DroidDesk – přední kamera" (/dev/video0) and
"DroidDesk – zadní kamera" (/dev/video1); pick one in the page's own camera menu.
"""
import socket
import sys

SOCKET = "\0droiddesk.camera"


def main():
    args = sys.argv[1:]
    if args in (["front"], ["back"]):
        request = "facing " + args[0]
    elif args in ([], ["status"]):
        request = "status"
    else:
        print("použití: droiddesk-camera [status|front|back]", file=sys.stderr)
        return 2
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    try:
        s.connect(SOCKET)
    except OSError as error:
        print(f"droiddesk-camera: kamera DroidDesku neodpovídá ({error}); běží plocha?", file=sys.stderr)
        return 1
    s.sendall((request + "\n").encode())
    answer = s.makefile(encoding="utf-8").readline().strip()
    print(answer)
    return 0 if answer.startswith("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
