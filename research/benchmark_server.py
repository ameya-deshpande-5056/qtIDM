#!/usr/bin/env python3
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

def make_pattern(size):
    # Generates deterministic pattern: byte i = i % 251
    return bytes(i % 251 for i in range(size))

PAYLOAD_10M = make_pattern(10 * 1024 * 1024)
PAYLOAD_50M = make_pattern(50 * 1024 * 1024)

class BenchmarkHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):
        return  # Suppress logging for performance

    def do_HEAD(self):
        self.handle_request(head_only=True)

    def do_GET(self):
        self.handle_request(head_only=False)

    def handle_request(self, head_only):
        path = self.path.split('?')[0]
        throttled = False
        throttle_threshold = 0
        throttle_rate_bps = 100 * 1024  # 100 KB/s

        if path == "/fast_10mb.bin":
            data = PAYLOAD_10M
        elif path == "/fast_50mb.bin":
            data = PAYLOAD_50M
        elif path == "/tail_throttled_10mb.bin":
            data = PAYLOAD_10M
            throttled = True
            throttle_threshold = int(len(data) * 0.75) # Last 25% throttled
        elif path == "/tail_throttled_20mb.bin":
            data = PAYLOAD_10M * 2
            throttled = True
            throttle_threshold = int(len(data) * 0.75)
        else:
            self.send_response(404)
            self.end_headers()
            return

        total_size = len(data)
        range_header = self.headers.get("Range")

        if range_header and range_header.startswith("bytes="):
            spec = range_header[6:].strip()
            parts = spec.split("-")
            start = int(parts[0]) if parts[0] else 0
            end = int(parts[1]) if parts[1] else total_size - 1
            if end >= total_size:
                end = total_size - 1
            length = end - start + 1

            self.send_response(206)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Range", f"bytes {start}-{end}/{total_size}")
            self.send_header("Content-Length", str(length))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("ETag", '"benchmark-v1"')
            self.end_headers()

            if not head_only:
                chunk_slice = data[start:end + 1]
                # Check if this range overlaps the throttled region
                if throttled and (end >= throttle_threshold):
                    chunk_size = 16 * 1024
                    for offset in range(0, len(chunk_slice), chunk_size):
                        sub_chunk = chunk_slice[offset:offset + chunk_size]
                        cur_byte_idx = start + offset
                        if cur_byte_idx >= throttle_threshold:
                            delay = len(sub_chunk) / throttle_rate_bps
                            time.sleep(delay)
                        self.wfile.write(sub_chunk)
                else:
                    self.wfile.write(chunk_slice)
        else:
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(total_size))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("ETag", '"benchmark-v1"')
            self.end_headers()

            if not head_only:
                self.wfile.write(data)

def run_server(port=0):
    server = ThreadingHTTPServer(("127.0.0.1", port), BenchmarkHandler)
    actual_port = server.server_address[1]
    print(f"{actual_port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()

if __name__ == "__main__":
    p = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    run_server(p)

