#!/usr/bin/env python3
"""Static server for web/ that also accepts screenshots from the ?auto= smoke test.

usage: devserver.py [port] [shot_dir]
POST /shot?n=<i>  body = canvas data URL  -> <shot_dir>/shot<i>.png
POST /log         body = text             -> printed
POST /done                                -> server exits
"""
import base64
import http.server
import os
import sys
import threading
import urllib.parse

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'web')
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
SHOTS = sys.argv[2] if len(sys.argv) > 2 else None


class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=ROOT, **k)

    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, '.wasm': 'application/wasm'}

    def log_message(self, *a):
        pass

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        if u.path == '/shot' and SHOTS:
            n = q.get('n', ['0'])[0]
            data = body.split(b',', 1)[1]
            os.makedirs(SHOTS, exist_ok=True)
            with open(os.path.join(SHOTS, 'shot%s.png' % n), 'wb') as f:
                f.write(base64.b64decode(data))
            print('shot', n, q.get('info', [''])[0], flush=True)
        elif u.path == '/log':
            print('log:', body.decode('utf8', 'replace'), flush=True)
        elif u.path == '/done':
            print('done', flush=True)
            threading.Thread(target=self.server.shutdown).start()
        self.send_response(200)
        self.end_headers()


if __name__ == '__main__':
    http.server.ThreadingHTTPServer(('127.0.0.1', PORT), H).serve_forever()
