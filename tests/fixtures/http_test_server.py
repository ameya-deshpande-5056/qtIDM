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
unknown_disconnects = {}
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
        if self.path.startswith("/forbidden.bin"):
            self.send_response(403)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path.startswith("/range.bin"):
            self.send_range(SIZE, head_only, accept_range=True)
            return
        if self.path.startswith("/changed-etag.bin"):
            self.send_range(SIZE, head_only, accept_range=True, etag='"new-entity"')
            return
        if self.path.startswith("/head-rejected-range.bin"):
            if head_only:
                self.send_response(405)
                self.send_header("Content-Length", "0")
                self.end_headers()
            else:
                self.send_range(SIZE, False, accept_range=True)
            return
        if self.path.startswith("/head-no-length-range.bin"):
            if head_only:
                self.send_response(200)
                self.send_header("Connection", "close")
                self.end_headers()
                self.close_connection = True
            else:
                self.send_range(SIZE, False, accept_range=True)
            return
        if self.path.startswith("/unknown-resume.bin"):
            self.send_unknown_resume(SIZE, head_only, ignore_resume=False)
            return
        if self.path.startswith("/ignored-unknown-resume.bin"):
            self.send_unknown_resume(SIZE, head_only, ignore_resume=True)
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

    def send_range(self, size, head_only, accept_range, slow=False, etag=None):
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
            if etag:
                self.send_header("ETag", etag)
            self.end_headers()
            if not head_only:
                self.write_body(start, end, slow)
            return
        self.send_response(200)
        if accept_range:
            self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(size))
        if etag:
            self.send_header("ETag", etag)
        self.end_headers()
        if not head_only:
            self.write_body(0, size - 1, slow)

    def send_unknown_resume(self, size, head_only, ignore_resume):
        if head_only:
            self.send_response(200)
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            return

        range_header = self.headers.get("Range")
        if range_header and range_header.startswith("bytes="):
            start_text, end_text = range_header[6:].split("-", 1)
            start = int(start_text or "0")
            requested_end = int(end_text) if end_text else size - 1
            end = min(requested_end, size - 1)
            if start == 0 and end == 0:
                self.send_response(206)
                self.send_header("Content-Range", "bytes 0-0/*")
                self.send_header("Content-Length", "1")
                self.end_headers()
                self.wfile.write(payload(0, 0))
                return
            if ignore_resume:
                self.send_response(200)
                self.send_header("Content-Length", str(size))
                self.end_headers()
                self.wfile.write(payload(0, size - 1))
                return
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/*")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(payload(start, end))
            self.close_connection = True
            return

        key = self.path.split("?", 1)[0]
        with stats_lock:
            attempt = unknown_disconnects.get(key, 0)
            unknown_disconnects[key] = attempt + 1
        if attempt == 0:
            partial = payload(0, size // 4 - 1)
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            self.wfile.write(f"{len(partial):x}\r\n".encode("ascii"))
            self.wfile.write(partial)
            self.wfile.write(b"\r\n")
            self.wfile.flush()
            self.close_connection = True
            return
        self.send_response(200)
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(payload(0, size - 1))
        self.close_connection = True

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
