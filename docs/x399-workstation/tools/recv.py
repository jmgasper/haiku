#!/usr/bin/env python3
"""Accept PUT uploads from the workstation into evidence/uploads."""
import http.server, os, time
ROOT = "/mnt/HaikuWork/x399/evidence/uploads"
class H(http.server.BaseHTTPRequestHandler):
    def do_PUT(self):
        name = os.path.basename(self.path) or "upload"
        n = int(self.headers.get("Content-Length", 0))
        data = self.rfile.read(n)
        with open(os.path.join(ROOT, time.strftime("%H%M%S-") + name), "wb") as f:
            f.write(data)
        self.send_response(200); self.end_headers(); self.wfile.write(b"ok\n")
    do_POST = do_PUT
http.server.ThreadingHTTPServer(("0.0.0.0", 8399), H).serve_forever()
