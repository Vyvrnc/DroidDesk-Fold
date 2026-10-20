"""Fake GitHub API + release files for tools/craft-test/run.sh (python3 -I mock_github.py PORT).

/rl/...          403 "API rate limit exceeded" (fallback to the web redirect)
/bad/...         release v9.9.9 whose SHA256SUMS.txt does not match the .deb
/nodeb/...       release without a linux-aarch64 .deb
/nosums/...      release without SHA256SUMS.txt
/e500/...        HTTP 500
/files/NAME      the files of the "bad" release
"""
import hashlib
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = int(sys.argv[1])
BASE = f"http://127.0.0.1:{PORT}/files"
DEB = b"not really a deb\n"
FILES = {
    "gridcraft-9.9.9-linux-aarch64.deb": DEB,
    "SHA256SUMS.txt": (
        hashlib.sha256(b"something else").hexdigest() + "  gridcraft-9.9.9-linux-aarch64.deb\n"
    ).encode(),
}


def release(names):
    return {
        "tag_name": "v9.9.9",
        "assets": [{"name": n, "browser_download_url": f"{BASE}/{n}"} for n in names],
    }


class Handler(BaseHTTPRequestHandler):
    def send(self, code, body, ctype="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, indent=2).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        p = self.path
        if p.startswith("/rl/"):
            self.send(403, {"message": "API rate limit exceeded for 127.0.0.1. (But here's the good news: ...)"})
        elif p.startswith("/bad/"):
            self.send(200, release(["gridcraft-9.9.9-linux-aarch64.deb", "SHA256SUMS.txt"]))
        elif p.startswith("/nodeb/"):
            self.send(200, release(["gridcraft-9.9.9-linux-x86_64.deb", "SHA256SUMS.txt"]))
        elif p.startswith("/nosums/"):
            self.send(200, release(["gridcraft-9.9.9-linux-aarch64.deb"]))
        elif p.startswith("/e500/"):
            self.send(500, {"message": "boom"})
        elif p.startswith("/files/") and p[7:] in FILES:
            self.send(200, FILES[p[7:]], "application/octet-stream")
        else:
            self.send(404, {"message": "Not Found"})

    def log_message(self, *args):
        pass


HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
