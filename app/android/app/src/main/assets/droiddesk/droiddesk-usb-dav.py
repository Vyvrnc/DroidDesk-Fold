#!/usr/bin/env python3
"""droiddesk-usb-dav: the files of a held USB disk over WebDAV, for Thunar (gvfs dav backend).

  droiddesk-usb-dav DEVICE [--port N] [--part N] [--lun N] [--secret]   start (prints the URL)
  droiddesk-usb-dav DEVICE --stop                                        stop it

Serves the first usable partition (FAT, exFAT, NTFS, ext2/3/4; or --part N) of DEVICE, a name
from "droiddesk-usb list", on 127.0.0.1 only: dav://localhost:PORT/ (Thunar: Další umístění /
Připojit k serveru). --port 0 (the default) takes a free port. Starting it again for a disk that
already has a server only prints that server's URL.

State: $PREFIX/tmp/droiddesk-usb-dav_dev_bus_usb_BBB_DDD.json, JSON
  {"pid", "port", "device", "url", "http", "part", "fs", "started"}
written once the port listens, removed at exit. The URL is the only line on stdout; after it,
the server runs in the foreground and logs to the same path with .log instead of .json. The server ends on SIGTERM (after the running
disk operation) and by itself when the device is no longer held by Linux.

Every request takes the disk's lock (the one droiddesk-usb uses) only while it works on the
disk, so droiddesk-usb and the USB window can run in between; requests are serialized.
Listings are cached for 2 s, files read by GET are kept in a small cache (Range requests of
players and editors); both are dropped on every write here and whenever another droiddesk-usb
command changed the disk.

No authentication: it listens on 127.0.0.1 and refuses requests whose Host is not localhost
(DNS rebinding) and requests from web pages (Origin header). Every app on the phone can reach
127.0.0.1 though; --secret puts the files below a random path (dav://localhost:PORT/SECRET/)
that only the state file tells.
"""
import contextlib
import html
import importlib.util
import itertools
import json
import mimetypes
import os
import secrets
import shutil
import signal
import sys
import tempfile
import threading
import time
import urllib.parse
import uuid
import xml.etree.ElementTree as ET
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("droiddesk_usb", os.path.join(HERE, "droiddesk-usb.py"))
usb = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(usb)

LISTING_TTL = 2.0
CACHED_FILES = 3
CHUNK = 1 << 20
ALLOWED_HOSTS = {"localhost", "127.0.0.1", "::1"}


class Failure(Exception):
    """An HTTP error answer."""

    def __init__(self, status, text=""):
        super().__init__(text)
        self.status, self.text = status, text


def state_path(name):
    return os.path.join(usb.status_dir(), "droiddesk-usb-dav" + name.replace("/", "_") + ".json")


def normalize(raw):
    """URL path (percent-encoded) -> "/a/b" or "/"; refuses "..", control characters, quotes."""
    path = urllib.parse.unquote(urllib.parse.urlsplit(raw).path, errors="strict")
    parts = []
    for part in path.split("/"):
        if part in ("", "."):
            continue
        if part == ".." or any(c in part for c in '"\t\n\r\0'):
            raise Failure(400, "názvy s uvozovkami, tabulátorem nebo koncem řádku neumím")
        parts.append(part)
    return "/" + "/".join(parts)


class Fetch:
    """A file being read from the disk into the cache (GET streams it while it grows)."""

    def __init__(self, path, size, local):
        self.path, self.size, self.local = path, size, local
        self.done = threading.Event()
        self.error = None
        self.readers = 0


class Disk:
    """The served partition: the Files object and caches, behind one lock."""

    def __init__(self, dev, lun, number):
        self.dev, self.lun, self.number = dev, lun, number
        self.files = None
        self.part = None
        self.lock = threading.RLock()
        self.depth = 0
        self.writing = False
        self.listings = {}
        self.fetches = {}
        os.makedirs(usb.status_dir(), exist_ok=True)
        self.tmp = tempfile.mkdtemp(prefix="droiddesk-usb-dav-", dir=usb.status_dir())
        self.counter = itertools.count(1)
        self.status_seen = self._status_stamp()

    def _status_stamp(self):
        try:
            return os.stat(usb.status_path(self.dev)).st_mtime_ns
        except OSError:
            return None

    def _others_changed(self):
        """Another droiddesk-usb command wrote the disk since the last request: forget caches,
        and the partition itself after format / flash / check --write."""
        stamp = self._status_stamp()
        if stamp == self.status_seen:
            return
        self.status_seen = stamp
        self.invalidate()
        try:
            with open(usb.status_path(self.dev), encoding="utf-8") as status:
                op = json.load(status).get("op")
        except (OSError, ValueError):
            op = None
        if op in ("format", "flash", "check", None) and self.files is not None:
            self.files.close()
            self.files = None

    @contextlib.contextmanager
    def op(self, write=False):
        """One disk operation: the server's lock and, outermost, the disk's flock."""
        with self.lock:
            self.depth += 1
            try:
                if self.depth == 1:
                    with usb.device_locked(self.dev):
                        self._others_changed()
                        if self.files is None:
                            part = usb.pick_part(self.dev, self.lun, self.number)
                            if part["fs"] in usb.BATCH_TOOLS:
                                self.files = usb.BatchFiles(self.dev, self.lun, part, part["fs"])
                            elif part["fs"].startswith("fat"):
                                self.files = usb.FatFiles(self.dev, self.lun, part)
                            else:
                                self.files = usb.ExtFiles(self.dev, self.lun, part)
                            self.part = part
                        self.writing = write
                        try:
                            yield self.files
                        finally:
                            self.writing = False
                            if write:
                                self.invalidate()
                else:
                    yield self.files
                    if write:
                        self.invalidate()
            finally:
                self.depth -= 1

    def invalidate(self):
        self.listings.clear()
        for fetch in list(self.fetches.values()):
            if fetch.done.is_set():
                self._drop(fetch)

    def _drop(self, fetch):
        self.fetches.pop(fetch.path, None)
        try:
            os.unlink(fetch.local)  # open readers keep their file
        except OSError:
            pass

    def temp(self, kind):
        folder = os.path.join(self.tmp, f"{kind}-{next(self.counter)}")
        os.makedirs(folder)
        return folder

    # ── reading ──

    def listing(self, path):
        with self.op() as files:
            hit = self.listings.get(path)
            if hit and time.monotonic() - hit[0] < LISTING_TTL:
                return hit[1]
            entries = files.ls(path, missing_ok=True)
            self.listings[path] = (time.monotonic(), entries)
            return entries

    def stat(self, path):
        """(is_dir, size, name) or None."""
        if path == "/":
            return (True, 0, "")
        parent, name = usb.split_path(path)
        with self.op():
            up = self.stat(parent)
            if up is None or not up[0]:
                return None
            return next((e for e in self.listing(parent) if e[2] == name), None)

    def fetch(self, path, size):
        """The cache file of path (being) read from the disk."""
        with self.op():
            fetch = self.fetches.get(path)
            if fetch and fetch.size == size and fetch.error is None and (
                    not fetch.done.is_set() or os.path.exists(fetch.local)):
                return fetch
            if fetch:
                self._drop(fetch)
            fetch = Fetch(path, size, os.path.join(self.temp("get"), "data"))
            self.fetches[path] = fetch
            done = [f for f in self.fetches.values() if f.done.is_set() and f is not fetch]
            for old in done[:max(0, len(self.fetches) - CACHED_FILES)]:
                self._drop(old)

        def run():
            try:
                with self.op() as files:
                    files.download(path, fetch.local)
            except SystemExit as error:
                fetch.error = str(error.code)
            except Exception as error:  # noqa: BLE001 - reported to the reader
                fetch.error = str(error)
            finally:
                fetch.done.set()
        threading.Thread(target=run, daemon=True).start()
        return fetch

    def close(self):
        if self.files is not None:
            self.files.close()
        shutil.rmtree(self.tmp, ignore_errors=True)


DISK = None
PREFIX_PATH = ""  # "/SECRET" with --secret


def href(path, is_dir):
    full = PREFIX_PATH + (path if path != "/" else "/")
    if is_dir and not full.endswith("/"):
        full += "/"
    return urllib.parse.quote(full, safe="/")


def prop_xml(path, entry):
    is_dir, size, name = entry
    if path == "/":
        name = (DISK.part or {}).get("label") or "USB"
    props = [f"<D:displayname>{escape(name)}</D:displayname>"]
    if is_dir:
        props.append("<D:resourcetype><D:collection/></D:resourcetype>")
        props.append("<D:getcontenttype>httpd/unix-directory</D:getcontenttype>")
    else:
        props.append("<D:resourcetype/>")
        props.append(f"<D:getcontentlength>{size}</D:getcontentlength>")
        kind = mimetypes.guess_type(name)[0] or "application/octet-stream"
        props.append(f"<D:getcontenttype>{escape(kind)}</D:getcontenttype>")
    props.append("<D:supportedlock><D:lockentry><D:lockscope><D:exclusive/></D:lockscope>"
                 "<D:locktype><D:write/></D:locktype></D:lockentry><D:lockentry><D:lockscope>"
                 "<D:shared/></D:lockscope><D:locktype><D:write/></D:locktype></D:lockentry>"
                 "</D:supportedlock>")
    return (f"<D:response><D:href>{escape(href(path, is_dir))}</D:href><D:propstat><D:prop>"
            + "".join(props) + "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>")


def lock_xml(token, depth, owner, timeout):
    return ('<?xml version="1.0" encoding="utf-8"?>\n<D:prop xmlns:D="DAV:"><D:lockdiscovery><D:activelock>'
            "<D:locktype><D:write/></D:locktype><D:lockscope><D:exclusive/></D:lockscope>"
            f"<D:depth>{escape(depth)}</D:depth>{owner}<D:timeout>{escape(timeout)}</D:timeout>"
            f"<D:locktoken><D:href>{escape(token)}</D:href></D:locktoken>"
            "</D:activelock></D:lockdiscovery></D:prop>").encode()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "droiddesk-usb-dav"

    def log_message(self, fmt, *args):
        sys.stderr.write("droiddesk-usb-dav: %s %s\n" % (self.address_string(), fmt % args))

    # ── plumbing ──

    def setup(self):
        super().setup()
        self.body_read = True

    def send(self, status, body=b"", headers=(), content_type="text/plain; charset=utf-8"):
        if isinstance(body, str):
            body = body.encode()
        if not self.body_read:
            self.close_connection = True  # the request body is still on the wire
        self.send_response(status)
        if body or status not in (204, 304):
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
        for key, value in headers:
            self.send_header(key, value)
        if self.close_connection:
            self.send_header("Connection", "close")
        self.end_headers()
        if body and self.command != "HEAD":
            self.wfile.write(body)

    def body_length(self):
        if "chunked" in self.headers.get("Transfer-Encoding", "").lower():
            return -1
        try:
            return int(self.headers.get("Content-Length") or 0)
        except ValueError:
            raise Failure(400, "Content-Length")

    def read_body_into(self, out):
        """Copies the request body into the file out; returns its size."""
        length, total = self.body_length(), 0
        self.body_read = True
        if length >= 0:
            while total < length:
                data = self.rfile.read(min(CHUNK, length - total))
                if not data:
                    self.close_connection = True
                    raise Failure(400, "tělo požadavku je kratší")
                out.write(data)
                total += len(data)
            return total
        while True:  # chunked
            line = self.rfile.readline(1024)
            try:
                size = int(line.split(b";")[0].strip(), 16)
            except ValueError:
                self.close_connection = True
                raise Failure(400, "chunked")
            if size == 0:
                while self.rfile.readline(1024) not in (b"\r\n", b"\n", b""):
                    pass
                return total
            left = size
            while left:
                data = self.rfile.read(min(CHUNK, left))
                if not data:
                    self.close_connection = True
                    raise Failure(400, "chunked")
                out.write(data)
                left -= len(data)
                total += len(data)
            self.rfile.readline(16)

    def read_body(self, limit=1 << 20):
        length = self.body_length()
        if length == 0:
            self.body_read = True
            return b""
        if length > limit:
            raise Failure(413)
        import io
        buffer = io.BytesIO()
        self.read_body_into(buffer)
        return buffer.getvalue()

    def check_host(self):
        host = self.headers.get("Host", "")
        name = urllib.parse.urlsplit("//" + host).hostname or ""
        if name not in ALLOWED_HOSTS:
            raise Failure(403, "jen pro localhost")
        if self.headers.get("Origin"):
            raise Failure(403, "ne z webových stránek")

    def disk_path(self, raw=None):
        path = normalize(self.path if raw is None else raw)
        if PREFIX_PATH:
            if path != PREFIX_PATH and not path.startswith(PREFIX_PATH + "/"):
                raise Failure(404)
            path = path[len(PREFIX_PATH):] or "/"
        return path

    def destination(self):
        raw = self.headers.get("Destination")
        if not raw:
            raise Failure(400, "chybí Destination")
        parts = urllib.parse.urlsplit(raw)
        if parts.netloc and (parts.hostname or "") not in ALLOWED_HOSTS:
            raise Failure(502, "cíl na jiném serveru")
        return self.disk_path(parts.path)

    def dispatch(self):
        try:
            self.body_read = False
            self.body_read = self.body_length() == 0
            self.check_host()
            getattr(self, "dav_" + self.command.lower())()
        except Failure as failure:
            self.send(failure.status, failure.text + "\n" if failure.text else "")
        except SystemExit as error:  # droiddesk-usb's errors
            text = str(error.code).removeprefix("droiddesk-usb: ")
            status = 404 if "není" in text and "prázdn" not in text and "složka" not in text else 409
            self.send(status, text + "\n")
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True
        except Exception as error:  # noqa: BLE001 - answer instead of dropping the connection
            self.log_message("chyba: %r", error)
            self.send(500, f"{error}\n")

    do_OPTIONS = do_PROPFIND = do_PROPPATCH = do_GET = do_HEAD = do_PUT = do_MKCOL = dispatch
    do_DELETE = do_MOVE = do_COPY = do_LOCK = do_UNLOCK = dispatch

    # ── methods ──

    def dav_options(self):
        self.send(200, headers=[("DAV", "1, 2"), ("MS-Author-Via", "DAV"),
                                ("Allow", "OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, MOVE, COPY, PROPFIND, "
                                          "PROPPATCH, LOCK, UNLOCK")])

    def dav_propfind(self):
        self.read_body()
        path = self.disk_path()
        depth = self.headers.get("Depth", "infinity")
        entry = DISK.stat(path)
        if entry is None:
            raise Failure(404)
        parts = [prop_xml(path, entry)]
        if entry[0] and depth != "0":
            for child in sorted(DISK.listing(path), key=lambda e: e[2].lower()):
                parts.append(prop_xml(f"{path.rstrip('/')}/{child[2]}", child))
        body = ('<?xml version="1.0" encoding="utf-8"?>\n<D:multistatus xmlns:D="DAV:">'
                + "".join(parts) + "</D:multistatus>\n")
        self.send(207, body, content_type='application/xml; charset="utf-8"')

    def dav_proppatch(self):
        # Properties are not stored; answer that each one was "set" (clients only set times).
        body = self.read_body()
        path = self.disk_path()
        entry = DISK.stat(path)
        if entry is None:
            raise Failure(404)
        names = []
        try:
            for prop in ET.fromstring(body).iter("{DAV:}prop"):
                names += [child.tag for child in prop]
        except ET.ParseError:
            raise Failure(400, "XML")
        props = []
        for i, tag in enumerate(names):
            space, _, local = tag[1:].rpartition("}") if tag.startswith("{") else ("", "", tag)
            props.append(f'<p{i}:{local} xmlns:p{i}="{escape(space)}"/>' if space else f"<{local}/>")
        props = "".join(props)
        body = ('<?xml version="1.0" encoding="utf-8"?>\n<D:multistatus xmlns:D="DAV:"><D:response>'
                f"<D:href>{escape(href(path, entry[0]))}</D:href><D:propstat><D:prop>{props}</D:prop>"
                "<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response></D:multistatus>\n")
        self.send(207, body, content_type='application/xml; charset="utf-8"')

    def dav_head(self):
        self.dav_get()

    def dav_get(self):
        path = self.disk_path()
        entry = DISK.stat(path)
        if entry is None:
            raise Failure(404)
        if entry[0]:
            rows = "".join(f'<li><a href="{html.escape(href(f"{path.rstrip("/")}/{e[2]}", e[0]))}">'
                           f'{html.escape(e[2])}{"/" if e[0] else ""}</a></li>'
                           for e in sorted(DISK.listing(path), key=lambda e: (not e[0], e[2].lower())))
            self.send(200, f"<!DOCTYPE html><meta charset=utf-8><title>{html.escape(path)}</title>"
                           f"<h1>{html.escape(path)}</h1><ul>{rows}</ul>", content_type="text/html; charset=utf-8")
            return
        size = entry[1]
        start, end, status = 0, size - 1, 200
        wanted = self.headers.get("Range", "")
        if wanted.startswith("bytes=") and "," not in wanted:
            first, _, last = wanted[6:].strip().partition("-")
            try:
                if first:
                    start, end = int(first), (int(last) if last else size - 1)
                else:
                    start, end = max(0, size - int(last)), size - 1
            except ValueError:
                start, end = 0, size - 1
            else:
                if start >= size or start > end:
                    self.send(416, headers=[("Content-Range", f"bytes */{size}")])
                    return
                end, status = min(end, size - 1), 206
        kind = mimetypes.guess_type(entry[2])[0] or "application/octet-stream"
        length = max(0, end - start + 1)
        fetch = None if self.command == "HEAD" or size == 0 else DISK.fetch(path, size)
        if fetch:
            # Errors before the first byte still get a proper answer.
            while not fetch.done.is_set() and not os.path.exists(fetch.local):
                fetch.done.wait(0.1)
            if fetch.error:
                raise Failure(500, fetch.error)
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "bytes")
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        if fetch:
            self.stream(fetch, start, length)

    def stream(self, fetch, start, length):
        position, end = start, start + length
        fetch.readers += 1
        try:
            with open(fetch.local, "rb") as data:
                while position < end:
                    available = os.fstat(data.fileno()).st_size
                    if available > position:
                        data.seek(position)
                        chunk = data.read(min(CHUNK, end - position, available - position))
                        self.wfile.write(chunk)
                        position += len(chunk)
                    elif fetch.done.is_set():
                        if os.fstat(data.fileno()).st_size <= position:
                            self.log_message("čtení %s skončilo předčasně: %s", fetch.path, fetch.error)
                            self.close_connection = True  # the client sees a short body
                            return
                    else:
                        fetch.done.wait(0.05)
        finally:
            fetch.readers -= 1

    def dav_put(self):
        path = self.disk_path()
        if path == "/":
            raise Failure(405)
        parent, name = usb.split_path(path)
        folder = DISK.temp("put")
        try:
            local = os.path.join(folder, name)
            with open(local, "wb") as out:
                self.read_body_into(out)
            with DISK.op(write=True) as files:
                up = DISK.stat(parent)
                if up is None or not up[0]:
                    raise Failure(409, "nadřazená složka neexistuje")
                existing = DISK.stat(path)
                if existing and existing[0]:
                    raise Failure(405, "je to složka")
                files.upload([local], parent)
        finally:
            shutil.rmtree(folder, ignore_errors=True)
        self.send(204 if existing else 201)

    def dav_mkcol(self):
        if self.body_length() != 0:
            raise Failure(415)
        path = self.disk_path()
        with DISK.op(write=True) as files:
            if DISK.stat(path) is not None:
                raise Failure(405, "už existuje")
            up = DISK.stat(usb.split_path(path)[0]) if path != "/" else None
            if up is None or not up[0]:
                raise Failure(409, "nadřazená složka neexistuje")
            files.mkdir(path)
        self.send(201)

    def dav_delete(self):
        path = self.disk_path()
        if path == "/":
            raise Failure(403)
        with DISK.op(write=True) as files:
            if DISK.stat(path) is None:
                raise Failure(404)
            files.remove(path, True)
        self.send(204)

    def _prepare_target(self, files, src, dst, entry):
        """Checks for MOVE/COPY; returns whether dst existed (removed unless file-over-file MOVE)."""
        if dst == "/" or src == "/":
            raise Failure(403)
        if dst == src or dst.startswith(src.rstrip("/") + "/"):
            raise Failure(403, "cíl je zdroj nebo v něm")
        up = DISK.stat(usb.split_path(dst)[0])
        if up is None or not up[0]:
            raise Failure(409, "nadřazená složka cíle neexistuje")
        target = DISK.stat(dst)
        if target is None:
            return False
        if self.headers.get("Overwrite", "T").upper() == "F":
            raise Failure(412, "cíl existuje")
        if not (self.command == "MOVE" and not target[0] and not entry[0]):
            files.remove(dst, True)
            DISK.invalidate()
        return True

    def dav_move(self):
        src, dst = self.disk_path(), self.destination()
        with DISK.op(write=True) as files:
            entry = DISK.stat(src)
            if entry is None:
                raise Failure(404)
            existed = self._prepare_target(files, src, dst, entry)
            files.move(src, dst)  # a file replaces a file in one step (editors save that way)
        self.send(204 if existed else 201)

    def dav_copy(self):
        src, dst = self.disk_path(), self.destination()
        folder = DISK.temp("copy")
        try:
            with DISK.op(write=True) as files:
                entry = DISK.stat(src)
                if entry is None:
                    raise Failure(404)
                existed = self._prepare_target(files, src, dst, entry)
                name = usb.split_path(dst)[1]
                if entry[0] and self.headers.get("Depth", "infinity") == "0":
                    files.mkdir(dst)
                else:
                    os.makedirs(os.path.join(folder, "from"))
                    files.download(src, os.path.join(folder, "from"))
                    os.makedirs(os.path.join(folder, "to"))
                    local = os.path.join(folder, "to", name)
                    os.rename(os.path.join(folder, "from", entry[2]), local)
                    files.upload([local], usb.split_path(dst)[0])
        finally:
            shutil.rmtree(folder, ignore_errors=True)
        self.send(204 if existed else 201)

    def dav_lock(self):
        body = self.read_body()
        path = self.disk_path()
        timeout = self.headers.get("Timeout", "Second-3600").split(",")[0].strip() or "Second-3600"
        depth = self.headers.get("Depth", "infinity")
        if not body.strip():  # refresh: the token from the If header
            token = self.headers.get("If", "").strip("()<> ")
            if DISK.stat(path) is None:
                raise Failure(404)
            self.send(200, lock_xml(token or "opaquelocktoken:" + str(uuid.uuid4()), depth, "", timeout),
                      content_type='application/xml; charset="utf-8"')
            return
        owner = ""
        try:
            found = ET.fromstring(body).find("{DAV:}owner")
            if found is not None:
                ET.register_namespace("D", "DAV:")
                owner = ET.tostring(found, encoding="unicode")
        except ET.ParseError:
            raise Failure(400, "XML")
        token = "opaquelocktoken:" + str(uuid.uuid4())
        status = 200
        if DISK.stat(path) is None:  # a lock on a new name creates an empty file (RFC 4918 7.3)
            parent, name = usb.split_path(path)
            folder = DISK.temp("lock")
            try:
                open(os.path.join(folder, name), "wb").close()
                with DISK.op(write=True) as files:
                    up = DISK.stat(parent)
                    if path == "/" or up is None or not up[0]:
                        raise Failure(409, "nadřazená složka neexistuje")
                    files.upload([os.path.join(folder, name)], parent)
            finally:
                shutil.rmtree(folder, ignore_errors=True)
            status = 201
        self.send(status, lock_xml(token, depth, owner, timeout), headers=[("Lock-Token", f"<{token}>")],
                  content_type='application/xml; charset="utf-8"')

    def dav_unlock(self):
        self.send(204)


def held(name):
    try:
        return any(d["name"] == name and d["held"] for d in usb.devices())
    except SystemExit:  # the app does not answer
        return None


def running(state):
    """The pid of a live server from a state file, else None."""
    try:
        with open(state, encoding="utf-8") as source:
            info = json.load(source)
        with open(f"/proc/{info['pid']}/cmdline", "rb") as cmdline:
            if b"droiddesk-usb-dav" not in cmdline.read():
                return None
        return info
    except (OSError, ValueError, KeyError):
        return None


def main(argv):
    argv, port = usb.option(argv, "--port", int)
    argv, number = usb.option(argv, "--part", int)
    argv, lun = usb.option(argv, "--lun", int)
    stop, secret = "--stop" in argv, "--secret" in argv
    args = [a for a in argv if a not in ("--stop", "--secret")]
    if len(args) != 1 or args[0] in ("-h", "--help"):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    name = args[0]
    state = state_path(name)
    info = running(state)
    if stop:
        if not info:
            print("droiddesk-usb-dav: neběží")
            return 0
        os.kill(info["pid"], signal.SIGTERM)
        for _ in range(600):
            if not os.path.exists(f"/proc/{info['pid']}"):
                break
            time.sleep(0.1)
        return 0
    if info:
        print(info["url"])
        return 0
    dev = usb._pick(name)
    if not dev["held"]:
        sys.exit(f"droiddesk-usb-dav: {name} není připojené do Linuxu (droiddesk-usb attach)")

    global DISK, PREFIX_PATH
    DISK = Disk(dev, lun, number)
    try:
        with DISK.op() as files:  # fails early when there is nothing to serve
            files.ls("/", missing_ok=True)
    except BaseException:
        DISK.close()
        raise
    if secret:
        PREFIX_PATH = "/" + secrets.token_hex(12)
    server = ThreadingHTTPServer(("127.0.0.1", port or 0), Handler)
    server.daemon_threads = True
    bound = server.server_address[1]
    url = f"dav://localhost:{bound}{PREFIX_PATH}/"
    info = {"pid": os.getpid(), "port": bound, "device": name, "url": url,
            "http": f"http://127.0.0.1:{bound}{PREFIX_PATH}/", "part": DISK.part["number"],
            "fs": DISK.part["fs"], "started": time.time()}
    tmp = state + ".tmp"
    with open(tmp, "w", encoding="utf-8") as out:
        json.dump(info, out, ensure_ascii=False)
    os.replace(tmp, state)
    print(url, flush=True)
    # From here on output goes to a log file: a caller that reads only the URL line (or
    # never reads stderr) must not block the server on a full pipe.
    log = os.open(state[:-len(".json")] + ".log", os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.dup2(log, 1)
    os.dup2(log, 2)
    os.close(log)

    stopping = threading.Event()

    def stop_server(*_):
        if not stopping.is_set():
            stopping.set()
            threading.Thread(target=server.shutdown, daemon=True).start()

    for signum in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(signum, stop_server)

    def watch():
        misses = 0
        while not stopping.wait(3):
            state_now = held(name)
            misses = misses + 1 if not state_now else 0
            if state_now is False or misses >= 3:
                sys.stderr.write(f"droiddesk-usb-dav: {name} už není připojené, končím\n")
                stop_server()
    threading.Thread(target=watch, daemon=True).start()
    try:
        server.serve_forever(poll_interval=0.5)
    finally:
        # A read may be cut short; a write finishes first (the disk must stay consistent).
        if not DISK.writing:
            for child in list(usb.CHILDREN):
                try:
                    child.terminate()
                except OSError:
                    pass
        with DISK.lock:
            server.server_close()
            DISK.close()
            if (running(state) or {}).get("pid") == os.getpid():
                os.unlink(state)
    return 0


if __name__ == "__main__":
    try:
        code = main(sys.argv[1:])
    except SystemExit as error:
        code = error.code
    if isinstance(code, str):
        print(usb.shown(code), file=sys.stderr)
        code = 1
    sys.exit(code or 0)
