#!/usr/bin/env python3
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import base64
import threading
import time

SIZE = 256 * 1024
SLOW_SIZE = 1024 * 1024
flaky_requests = 0
active_slow_requests = 0
maximum_slow_requests = 0
stats_lock = threading.Lock()


def byte_at(index):
    return index % 251


def payload(start, end):
    return bytes(byte_at(i) for i in range(start, end + 1))


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        return

    def do_HEAD(self):
        self.handle_request(head_only=True)

    def do_GET(self):
        self.handle_request(head_only=False)

    def handle_request(self, head_only):
        global flaky_requests, maximum_slow_requests
        if self.path.startswith("/reset-stats"):
            with stats_lock:
                maximum_slow_requests = 0
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path.startswith("/stats"):
            with stats_lock:
                data = str(maximum_slow_requests).encode("ascii")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            if not head_only:
                self.wfile.write(data)
            return
        if self.path.startswith("/flaky.bin"):
            if not head_only and not self.headers.get("Range") and flaky_requests < 2:
                flaky_requests += 1
                self.send_response(503)
                self.send_header("Retry-After", "0")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.send_range(SIZE, head_only, accept_range=True)
            return
        if self.path.startswith("/auth.bin"):
            expected = "Basic " + base64.b64encode(b"user:pass").decode("ascii")
            if self.headers.get("Authorization") != expected:
                self.send_response(401)
                self.send_header("WWW-Authenticate", 'Basic realm="qtIDM"')
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.send_range(SIZE, head_only, accept_range=True)
            return
        if self.path.startswith("/range.bin"):
            self.send_range(SIZE, head_only, accept_range=True)
            return
        if self.path.startswith("/slow.bin"):
            self.send_range(SLOW_SIZE, head_only, accept_range=True, slow=True)
            return
        if self.path.startswith("/norange.bin"):
            self.send_response(200)
            self.send_header("Content-Length", str(SIZE))
            self.end_headers()
            if not head_only:
                self.wfile.write(payload(0, SIZE - 1))
            return
        if self.path.startswith("/unknown.bin"):
            self.send_response(200)
            self.send_header("Connection", "close")
            self.end_headers()
            if not head_only:
                self.wfile.write(payload(0, SIZE - 1))
            self.close_connection = True
            return
        if self.path.startswith("/disconnect.bin"):
            self.send_response(200)
            self.send_header("Content-Length", str(SIZE))
            self.end_headers()
            if not head_only:
                self.wfile.write(payload(0, SIZE // 4))
                self.close_connection = True
            return
        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def send_range(self, size, head_only, accept_range, slow=False):
        range_header = self.headers.get("Range")
        if range_header and range_header.startswith("bytes="):
            start_text, end_text = range_header[6:].split("-", 1)
            start = int(start_text or "0")
            end = int(end_text or str(size - 1))
            end = min(end, size - 1)
            if start > end:
                self.send_response(416)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.send_response(206)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.send_header("Content-Length", str(end - start + 1))
            self.end_headers()
            if not head_only:
                self.write_body(start, end, slow)
            return
        self.send_response(200)
        if accept_range:
            self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(size))
        self.end_headers()
        if not head_only:
            self.write_body(0, size - 1, slow)

    def write_body(self, start, end, slow):
        global active_slow_requests, maximum_slow_requests
        if slow:
            with stats_lock:
                active_slow_requests += 1
                maximum_slow_requests = max(maximum_slow_requests, active_slow_requests)
        chunk = 8192
        pos = start
        try:
            while pos <= end:
                next_end = min(end, pos + chunk - 1)
                self.wfile.write(payload(pos, next_end))
                self.wfile.flush()
                pos = next_end + 1
                if slow:
                    time.sleep(0.01)
        finally:
            if slow:
                with stats_lock:
                    active_slow_requests -= 1


if __name__ == "__main__":
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    print(server.server_port, flush=True)
    server.serve_forever()
