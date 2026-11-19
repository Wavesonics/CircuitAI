#!/usr/bin/env python3
"""Bridge between BARb brain servers and the browser viewer.

Connects to every AI brain port (see PROTOCOL.md), keeps the latest meta,
state and recent decisions per AI, and serves the viewer:

  GET  /                      viewer (web/)
  GET  /api/ais               connected AIs and their hello info
  GET  /api/meta?port=N       meta for one AI
  GET  /events?port=N         Server-Sent Events: state and decision messages
  POST /api/send?port=N       forward one JSON object to the AI; replies
                              (ack, error) arrive on /events

Standard library only. Binds to 127.0.0.1.
"""

import argparse
import collections
import http.server
import json
import mimetypes
import os
import queue
import socket
import threading
import time
import urllib.parse

WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")
RECENT_DECISIONS = 300
SUBSCRIBER_QUEUE = 256
RECONNECT_SECONDS = 2.0
HEARTBEAT_SECONDS = 15.0


class AILink:
    """One TCP connection to one AI brain server, reconnecting as needed."""

    def __init__(self, port):
        self.port = port
        self.lock = threading.Lock()
        self.send_lock = threading.Lock()  # keeps concurrent commands from interleaving
        self.sock = None
        self.hello = None
        self.meta = None  # raw JSON line
        self.state = None  # raw JSON line
        self.decisions = collections.deque(maxlen=RECENT_DECISIONS)
        self.subscribers = set()
        self.waiters = {}  # command id: [threading.Event, ack dict or None]
        threading.Thread(target=self._run, name=f"ai-{port}", daemon=True).start()

    @property
    def connected(self):
        return self.sock is not None

    def subscribe(self):
        q = queue.Queue(maxsize=SUBSCRIBER_QUEUE)
        with self.lock:
            # Late joiners get the current picture first
            if self.state is not None:
                q.put_nowait(("state", self.state))
            for line in self.decisions:
                if q.full():
                    break
                q.put_nowait(("decision", line))
            self.subscribers.add(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            self.subscribers.discard(q)

    def send(self, obj):
        data = (json.dumps(obj, separators=(",", ":")) + "\n").encode()
        with self.lock:
            sock = self.sock
        if sock is None:
            return False
        try:
            with self.send_lock:
                sock.sendall(data)
            return True
        except OSError:
            return False

    def request(self, obj, timeout=5.0):
        """Sends a command with obj["id"] set and returns its ack dict, or None
        if the AI is not connected or does not answer in time."""
        waiter = [threading.Event(), None]
        with self.lock:
            self.waiters[obj["id"]] = waiter
        try:
            if not self.send(obj) or not waiter[0].wait(timeout):
                return None
            return waiter[1]
        finally:
            with self.lock:
                self.waiters.pop(obj["id"], None)

    def _publish(self, kind, line):
        with self.lock:
            subscribers = list(self.subscribers)
        for q in subscribers:
            try:
                q.put_nowait((kind, line))
            except queue.Full:
                # A slow browser loses messages rather than stalling the bridge
                pass

    def _run(self):
        while True:
            try:
                sock = socket.create_connection(("127.0.0.1", self.port), timeout=RECONNECT_SECONDS)
            except OSError:
                time.sleep(RECONNECT_SECONDS)
                continue
            sock.settimeout(None)
            with self.lock:
                self.sock = sock
            try:
                self._read(sock)
            except (OSError, ValueError):
                pass  # dropped or garbled connection; reconnect below
            with self.lock:
                self.sock = None
                self.hello = None
                self.meta = None
                self.state = None
                self.decisions.clear()
            sock.close()
            self._publish("status", json.dumps({"t": "status", "connected": False}))
            time.sleep(RECONNECT_SECONDS)

    def _read(self, sock):
        stream = sock.makefile("rb")
        for raw in stream:
            line = raw.decode("utf-8", "replace").rstrip("\n")
            if not line:
                continue
            # The AI always writes "t" first, so the type is read without parsing
            # the (often large) message
            if not line.startswith('{"t":"'):
                continue
            kind = line[6:line.find('"', 6)]
            if kind == "hello":
                with self.lock:
                    self.hello = json.loads(line)
                self._publish("status", json.dumps({"t": "status", "connected": True}))
            elif kind == "meta":
                with self.lock:
                    self.meta = line
                self._publish("meta", line)
            elif kind == "state":
                with self.lock:
                    self.state = line
                self._publish("state", line)
            elif kind == "decision":
                with self.lock:
                    self.decisions.append(line)
                self._publish("decision", line)
            elif kind in ("ack", "error"):
                if kind == "ack":
                    self._resolve(json.loads(line))
                # Replies to commands; every viewer of this AI sees them. SSE
                # reserves "error" for connection errors, so it is renamed.
                self._publish("aierror" if kind == "error" else kind, line)

    def _resolve(self, ack):
        with self.lock:
            waiter = self.waiters.get(ack.get("id"))
        if waiter is not None:
            waiter[1] = ack
            waiter[0].set()


class Handler(http.server.BaseHTTPRequestHandler):
    links = {}
    allowed_hosts = set()
    protocol_version = "HTTP/1.1"

    def _host_ok(self):
        # Rejects DNS-rebinding pages, which reach us under a foreign Host name
        if self.headers.get("Host", "") in self.allowed_hosts:
            return True
        self._json(403, {"error": "unexpected Host header"})
        return False

    def log_message(self, fmt, *args):
        pass

    def _link(self, query):
        try:
            port = int(query.get("port", [""])[0])
        except ValueError:
            return None
        return self.links.get(port)

    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if not self._host_ok():
            return None
        url = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(url.query)
        if url.path == "/api/ais":
            ais = []
            for port, link in sorted(self.links.items()):
                with link.lock:
                    ais.append({"port": port, "connected": link.connected, "hello": link.hello})
            return self._json(200, ais)
        if url.path == "/api/meta":
            link = self._link(query)
            if link is None or link.meta is None:
                return self._json(404, {"error": "no meta for that port"})
            body = link.meta.encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return None
        if url.path == "/events":
            link = self._link(query)
            if link is None:
                return self._json(404, {"error": "unknown port"})
            return self._events(link)
        return self._static(url.path)

    def do_POST(self):
        if not self._host_ok():
            return None
        url = urllib.parse.urlparse(self.path)
        if url.path != "/api/send":
            return self._json(404, {"error": "not found"})
        # Requiring JSON forces a CORS preflight, so other sites cannot post here
        if self.headers.get("Content-Type", "").split(";")[0].strip() != "application/json":
            return self._json(415, {"error": "expected application/json"})
        link = self._link(urllib.parse.parse_qs(url.query))
        if link is None:
            return self._json(404, {"error": "unknown port"})
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = -1
        if length <= 0 or length > 64 * 1024:
            return self._json(400, {"error": "bad length"})
        try:
            obj = json.loads(self.rfile.read(length))
        except ValueError:
            return self._json(400, {"error": "malformed json"})
        if not isinstance(obj, dict) or not isinstance(obj.get("t"), str):
            return self._json(400, {"error": "expected an object with a string 't'"})
        return self._json(200 if link.send(obj) else 503, {"sent": link.connected})

    def _events(self, link):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        q = link.subscribe()
        try:
            while True:
                try:
                    kind, line = q.get(timeout=HEARTBEAT_SECONDS)
                    self.wfile.write(f"event: {kind}\ndata: {line}\n\n".encode())
                except queue.Empty:
                    self.wfile.write(b": keepalive\n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            link.unsubscribe(q)

    def _static(self, path):
        rel = "index.html" if path in ("", "/") else path.lstrip("/")
        full = os.path.realpath(os.path.join(WEB_DIR, rel))
        if not full.startswith(os.path.realpath(WEB_DIR) + os.sep) or not os.path.isfile(full):
            return self._json(404, {"error": "not found"})
        with open(full, "rb") as f:
            body = f.read()
        self.send_response(200)
        self.send_header("Content-Type", mimetypes.guess_type(full)[0] or "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)
        return None


def parse_ports(spec):
    ports = set()
    for part in spec.split(","):
        if "-" in part:
            lo, hi = part.split("-", 1)
            ports.update(range(int(lo), int(hi) + 1))
        elif part:
            ports.add(int(part))
    return sorted(ports)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ports", default="7710-7717", help="AI brain ports, e.g. 7710-7713 or 7710,7720")
    ap.add_argument("--listen", type=int, default=8770, help="HTTP port for the viewer")
    args = ap.parse_args()

    Handler.links = {p: AILink(p) for p in parse_ports(args.ports)}
    Handler.allowed_hosts = {f"127.0.0.1:{args.listen}", f"localhost:{args.listen}"}
    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.listen), Handler)
    server.daemon_threads = True
    print(f"brainview: http://127.0.0.1:{args.listen}/  (AI ports {args.ports})", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
