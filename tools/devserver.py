#!/usr/bin/env python3
"""Static server for web/ with the online-multiplayer relay; also accepts screenshots from the ?auto= smoke test.

usage: devserver.py [port] [shot_dir] [--host ADDR]
  --host 0.0.0.0 makes the game (and online play) reachable from other machines; the default is localhost only.

GET  /ws                                  -> WebSocket: multiplayer rooms (see Relay below)
POST /shot?n=<i>  body = canvas data URL  -> <shot_dir>/shot<i>.png
POST /log         body = text             -> printed
POST /done                                -> server exits
"""
import argparse
import base64
import hashlib
import http.server
import json
import os
import random
import socket
import struct
import threading
import urllib.parse

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'web')
WS_GUID = b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
MAX_MESSAGE = 1 << 16
MAX_PLAYERS = 8  # grid slots on every track
ROOM_CHARS = 'ABCDEFGHJKLMNPQRSTUVWXYZ'  # no I/O, they look like 1/0


class Peer:
    def __init__(self, sock):
        self.sock = sock
        self.lock = threading.Lock()
        self.id = 0
        self.name = ''
        self.room = None

    def send(self, opcode, payload):
        n = len(payload)
        if n < 126:
            head = struct.pack('!BB', 0x80 | opcode, n)
        elif n < 1 << 16:
            head = struct.pack('!BBH', 0x80 | opcode, 126, n)
        else:
            head = struct.pack('!BBQ', 0x80 | opcode, 127, n)
        try:
            with self.lock:
                self.sock.sendall(head + payload)
        except OSError:
            pass  # the reader thread notices the dead socket and cleans up

    def send_json(self, obj):
        self.send(0x1, json.dumps(obj).encode())


class Room:
    def __init__(self, code):
        self.code = code
        self.peers = {}  # id -> Peer, in join order
        self.host = 0
        self.next_id = 1


class Relay:
    """Rooms of up to MAX_PLAYERS peers. The server only manages membership; the game protocol is between peers.

    Client -> server (JSON text):
      {"t": "create", "name": str}            new room; the creator is host
      {"t": "join", "room": str, "name": str}
      any other JSON object                   forwarded to the room ("to": id sends to one peer only)
      binary                                  forwarded to every other peer in the room (car states)
    Server -> client:
      {"t": "welcome", "id": int, "room": str}
      {"t": "peers", "host": int, "players": [{"id": int, "name": str}, ...]}   after every membership change
      {"t": "error", "msg": str}
      forwarded JSON gets "from": <sender id>
    """

    def __init__(self):
        self.lock = threading.Lock()
        self.rooms = {}

    def _broadcast_peers(self, room):
        msg = {'t': 'peers', 'host': room.host, 'players': [{'id': p.id, 'name': p.name} for p in room.peers.values()]}
        for p in list(room.peers.values()):
            p.send_json(msg)

    def on_text(self, peer, text):
        try:
            msg = json.loads(text)
        except ValueError:
            return
        if not isinstance(msg, dict):
            return
        t = msg.get('t')
        name = str(msg.get('name', ''))[:20].strip() or 'Player'
        if t in ('create', 'join'):
            if peer.room:
                self.leave(peer)
            with self.lock:
                if t == 'create':
                    code = ''.join(random.choice(ROOM_CHARS) for _ in range(4))
                    while code in self.rooms:
                        code = ''.join(random.choice(ROOM_CHARS) for _ in range(4))
                    room = self.rooms[code] = Room(code)
                else:
                    room = self.rooms.get(str(msg.get('room', '')).upper().strip())
                    if not room:
                        peer.send_json({'t': 'error', 'msg': 'No room with that code.'})
                        return
                    if len(room.peers) >= MAX_PLAYERS:
                        peer.send_json({'t': 'error', 'msg': 'That room is full.'})
                        return
                peer.id = room.next_id
                room.next_id += 1
                peer.name = name
                peer.room = room
                room.peers[peer.id] = peer
                if not room.host:
                    room.host = peer.id
            peer.send_json({'t': 'welcome', 'id': peer.id, 'room': room.code})
            self._broadcast_peers(room)
            return
        room = peer.room
        if not room:
            return
        msg['from'] = peer.id
        data = json.dumps(msg).encode()
        to = msg.get('to')
        for p in list(room.peers.values()):
            if p is not peer and (to is None or p.id == to):
                p.send(0x1, data)

    def on_binary(self, peer, data):
        room = peer.room
        if room:
            for p in list(room.peers.values()):
                if p is not peer:
                    p.send(0x2, data)

    def leave(self, peer):
        room = peer.room
        if not room:
            return
        with self.lock:
            room.peers.pop(peer.id, None)
            peer.room = None
            if not room.peers:
                self.rooms.pop(room.code, None)
                return
            if room.host == peer.id:
                room.host = next(iter(room.peers))  # longest-standing member takes over
        self._broadcast_peers(room)


RELAY = Relay()


def recv_exact(f, n):
    data = f.read(n)
    if len(data) < n:
        raise ConnectionError
    return data


def serve_websocket(handler):
    key = handler.headers.get('Sec-WebSocket-Key', '')
    accept = base64.b64encode(hashlib.sha1(key.encode() + WS_GUID).digest()).decode()
    handler.wfile.write(('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                         'Sec-WebSocket-Accept: %s\r\n\r\n' % accept).encode())
    handler.wfile.flush()
    sock = handler.connection
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    peer = Peer(sock)
    f = handler.rfile
    parts, part_op = [], 0
    try:
        while True:
            b0, b1 = recv_exact(f, 2)
            fin, opcode, masked, n = b0 & 0x80, b0 & 0x0F, b1 & 0x80, b1 & 0x7F
            if n == 126:
                n = struct.unpack('!H', recv_exact(f, 2))[0]
            elif n == 127:
                n = struct.unpack('!Q', recv_exact(f, 8))[0]
            if not masked or n > MAX_MESSAGE:
                break  # clients must mask; refuse oversized frames
            mask = recv_exact(f, 4)
            payload = bytes(c ^ mask[i & 3] for i, c in enumerate(recv_exact(f, n)))
            if opcode == 0x8:  # close
                peer.send(0x8, payload[:2])
                break
            if opcode == 0x9:  # ping
                peer.send(0xA, payload)
                continue
            if opcode == 0xA:
                continue
            if opcode in (0x1, 0x2):
                parts, part_op = [payload], opcode
            elif opcode == 0x0 and parts:
                parts.append(payload)
            else:
                break
            if sum(len(p) for p in parts) > MAX_MESSAGE:
                break
            if not fin:
                continue
            msg, parts = b''.join(parts), []
            if part_op == 0x1:
                RELAY.on_text(peer, msg.decode('utf8', 'replace'))
            else:
                RELAY.on_binary(peer, msg)
    except (ConnectionError, OSError, ValueError):
        pass
    finally:
        RELAY.leave(peer)
        handler.close_connection = True


class H(http.server.SimpleHTTPRequestHandler):
    shots = None

    def __init__(self, *a, **k):
        super().__init__(*a, directory=ROOT, **k)

    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, '.wasm': 'application/wasm'}

    def log_message(self, *a):
        pass

    def do_GET(self):
        if urllib.parse.urlparse(self.path).path == '/ws' and self.headers.get('Upgrade', '').lower() == 'websocket':
            serve_websocket(self)
        else:
            super().do_GET()

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        if u.path == '/shot' and self.shots:
            n = q.get('n', ['0'])[0]
            data = body.split(b',', 1)[1]
            os.makedirs(self.shots, exist_ok=True)
            with open(os.path.join(self.shots, 'shot%s.png' % n), 'wb') as f:
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
    ap = argparse.ArgumentParser()
    ap.add_argument('port', nargs='?', type=int, default=8000)
    ap.add_argument('shot_dir', nargs='?')
    ap.add_argument('--host', default='127.0.0.1')
    args = ap.parse_args()
    H.shots = args.shot_dir
    server = http.server.ThreadingHTTPServer((args.host, args.port), H)
    server.daemon_threads = True
    print('serving web/ on http://%s:%d' % (args.host, args.port), flush=True)
    server.serve_forever()
