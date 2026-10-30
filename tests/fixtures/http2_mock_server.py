#!/usr/bin/env python3
"""
Mock HTTP/2 / HTTP/1.1 server fixture for testing protocol fallback, error handling,
and concurrent stream multiplexing over a single connection.
"""
import json
import socket
import struct
import sys
import threading
import time

PREFACE = b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
# SETTINGS frame (empty settings): length=0, type=4, flags=0, stream=0
SETTINGS_FRAME = b"\x00\x00\x00\x04\x00\x00\x00\x00\x00"
# GOAWAY frame: length=8, type=7, flags=0, stream=0, payload: stream=0, code=1 (PROTOCOL_ERROR)
GOAWAY_FRAME = b"\x00\x00\x08\x07\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01"

TEST_PAYLOAD = bytes((i % 251) for i in range(8192))


def make_frame(length, ftype, flags, stream_id, payload=b""):
    return struct.pack(">I", length)[1:] + struct.pack(">BB", ftype, flags) + struct.pack(">I", stream_id) + payload


class MockServer:
    def __init__(self, mode="fallback", stats_file=None):
        self.mode = mode
        self.stats_file = stats_file
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", 0))
        self.port = self.srv.getsockname()[1]
        self.srv.listen(16)
        self.attempts = 0
        self.connections = 0
        self.streams_served = []
        self.lock = threading.Lock()
        self.running = True
        self.ssl_ctx = None
        if self.mode.startswith("https_alpn"):
            import os
            import subprocess
            import ssl
            import tempfile
            self._tmpdir = tempfile.TemporaryDirectory()
            self.cert_file = os.path.join(self._tmpdir.name, "cert.pem")
            self.key_file = os.path.join(self._tmpdir.name, "key.pem")
            subprocess.run([
                "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-keyout", self.key_file, "-out", self.cert_file, "-days", "1",
                "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1"
            ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            self.ssl_ctx.load_cert_chain(self.cert_file, self.key_file)
            if self.mode == "https_alpn_http1":
                self.ssl_ctx.set_alpn_protocols(["http/1.1"])
            else:
                self.ssl_ctx.set_alpn_protocols(["h2", "http/1.1"])

    def save_stats(self, extra=None):
        if self.stats_file:
            data = {
                "connections": self.connections,
                "streams": self.streams_served,
                "attempts": self.attempts
            }
            if extra:
                data.update(extra)
            with open(self.stats_file, "w") as f:
                json.dump(data, f)

    def run(self):
        while self.running:
            try:
                self.srv.settimeout(0.5)
                conn, addr = self.srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with self.lock:
                self.attempts += 1
                att = self.attempts
            t = threading.Thread(target=self.handle_client, args=(conn, att))
            t.daemon = True
            t.start()

    def handle_client(self, conn, attempt):
        try:
            if self.mode.startswith("https_alpn"):
                tls_conn = self.ssl_ctx.wrap_socket(conn, server_side=True)
                proto = tls_conn.selected_alpn_protocol()
                if proto == "http/1.1" or self.mode == "https_alpn_http1":
                    with self.lock:
                        self.connections += 1
                    tls_conn.settimeout(5.0)
                    try:
                        req_chunk = tls_conn.recv(4096)
                    except Exception:
                        req_chunk = b""
                    resp = (
                        f"HTTP/1.1 200 OK\r\n"
                        f"Content-Length: {len(TEST_PAYLOAD)}\r\n"
                        f"Connection: close\r\n\r\n"
                    ).encode("ascii") + TEST_PAYLOAD
                    tls_conn.sendall(resp)
                    tls_conn.close()
                    self.save_stats({"alpn": "http/1.1", "connections": self.connections})
                    return

                with self.lock:
                    self.connections += 1

                conn_streams = []
                # Server sends initial SETTINGS frame
                tls_conn.sendall(make_frame(0, 4, 0, 0, b""))
                buf = bytearray()
                preface_consumed = False
                tls_conn.settimeout(6.0)
                while self.running:
                    try:
                        chunk = tls_conn.recv(4096)
                    except Exception:
                        break
                    if not chunk:
                        break
                    buf.extend(chunk)

                    if not preface_consumed:
                        if len(buf) >= len(PREFACE):
                            if buf.startswith(PREFACE):
                                buf = buf[len(PREFACE):]
                                preface_consumed = True
                            else:
                                break
                        else:
                            continue

                    while len(buf) >= 9:
                        length = (buf[0] << 16) | (buf[1] << 8) | buf[2]
                        ftype = buf[3]
                        flags = buf[4]
                        stream_id = struct.unpack(">I", buf[5:9])[0] & 0x7FFFFFFF
                        if len(buf) < 9 + length:
                            break
                        payload = buf[9:9 + length]
                        buf = buf[9 + length:]

                        if ftype == 4 and (flags & 0x01) == 0:
                            # Client SETTINGS -> send SETTINGS ACK
                            tls_conn.sendall(make_frame(0, 4, 1, 0, b""))
                            with self.lock:
                                conn_streams.append(1)
                                self.streams_served.append(1)
                                self.save_stats({
                                    "alpn": proto,
                                    "max_streams_on_single_connection": len(conn_streams),
                                    "multiplexed_stream_ids": conn_streams
                                })
                            h1 = make_frame(1, 1, 4, 1, b"\x88")
                            d1 = make_frame(len(TEST_PAYLOAD), 0, 1, 1, TEST_PAYLOAD)
                            tls_conn.sendall(h1 + d1)
                        elif ftype == 1:
                            # Stream 3 HEADERS
                            with self.lock:
                                conn_streams.append(stream_id)
                                self.streams_served.append(stream_id)
                                self.save_stats({
                                    "alpn": proto,
                                    "max_streams_on_single_connection": len(conn_streams),
                                    "multiplexed_stream_ids": conn_streams
                                })
                            h_res = make_frame(1, 1, 4, stream_id, b"\x88")
                            d_res = make_frame(len(TEST_PAYLOAD), 0, 1, stream_id, TEST_PAYLOAD)
                            tls_conn.sendall(h_res + d_res)
                            if len(conn_streams) >= 2:
                                time.sleep(0.05)
                                tls_conn.close()
                                return
                tls_conn.close()
                return

            req = conn.recv(4096)
            if not req:
                conn.close()
                return

            req_text = req.decode("utf-8", errors="replace")
            sys.stderr.write(f"[REQ]: {req_text[:100]}\n")
            sys.stderr.flush()

            if self.mode == "fallback":
                if attempt == 1 and "Upgrade: h2c" in req_text:
                    conn.sendall(b"HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: h2c\r\n\r\n")
                    conn.recv(len(PREFACE))
                    conn.sendall(SETTINGS_FRAME + GOAWAY_FRAME)
                    time.sleep(0.05)
                    conn.close()
                    return
                else:
                    resp = (
                        f"HTTP/1.1 200 OK\r\n"
                        f"Content-Length: {len(TEST_PAYLOAD)}\r\n"
                        f"Connection: close\r\n\r\n"
                    ).encode("ascii") + TEST_PAYLOAD
                    conn.sendall(resp)
                    conn.close()
                    return

            elif self.mode == "no_upgrade":
                resp = (
                    f"HTTP/1.1 200 OK\r\n"
                    f"Content-Length: {len(TEST_PAYLOAD)}\r\n"
                    f"Connection: close\r\n\r\n"
                ).encode("ascii") + TEST_PAYLOAD
                conn.sendall(resp)
                conn.close()
                return

            elif self.mode == "multiplex":
                # Handle Range probe (bytes=0-0)
                if "Range: bytes=0-0" in req_text:
                    resp = (
                        f"HTTP/1.1 206 Partial Content\r\n"
                        f"Accept-Ranges: bytes\r\n"
                        f"Content-Range: bytes 0-0/{len(TEST_PAYLOAD)}\r\n"
                        f"Content-Length: 1\r\n"
                        f"Connection: close\r\n\r\n"
                    ).encode("ascii") + TEST_PAYLOAD[:1]
                    conn.sendall(resp)
                    conn.close()
                    return

                # Handle HEAD probe
                if req_text.startswith("HEAD "):
                    resp = (
                        f"HTTP/1.1 200 OK\r\n"
                        f"Content-Length: {len(TEST_PAYLOAD)}\r\n"
                        f"Accept-Ranges: bytes\r\n"
                        f"Connection: close\r\n\r\n"
                    ).encode("ascii")
                    conn.sendall(resp)
                    conn.close()
                    return

                if "Upgrade: h2c" in req_text:
                    with self.lock:
                        self.connections += 1
                        self.streams_served.append(1)
                        self.save_stats()

                    conn.sendall(b"HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: h2c\r\n\r\n")
                    conn.sendall(make_frame(0, 4, 0, 0, b"")) # server SETTINGS

                    buf = bytearray()
                    preface_consumed = False
                    conn.settimeout(6.0)
                    while self.running:
                        try:
                            chunk = conn.recv(4096)
                        except socket.timeout:
                            break
                        if not chunk:
                            break
                        buf.extend(chunk)

                        if not preface_consumed:
                            if len(buf) >= len(PREFACE):
                                if buf.startswith(PREFACE):
                                    buf = buf[len(PREFACE):]
                                    preface_consumed = True
                                else:
                                    break
                            else:
                                continue

                        # Process HTTP/2 frames in buffer
                        while len(buf) >= 9:
                            length = (buf[0] << 16) | (buf[1] << 8) | buf[2]
                            ftype = buf[3]
                            flags = buf[4]
                            stream_id = struct.unpack(">I", buf[5:9])[0] & 0x7FFFFFFF
                            if len(buf) < 9 + length:
                                break
                            payload = buf[9:9 + length]
                            buf = buf[9 + length:]

                            if ftype == 4 and (flags & 0x01) == 0:
                                # Client SETTINGS -> send SETTINGS ACK
                                conn.sendall(make_frame(0, 4, 1, 0, b""))
                                # Send response for stream 1
                                chunk1 = TEST_PAYLOAD
                                h1 = make_frame(1, 1, 4, 1, b"\x88") # :status: 200
                                d1 = make_frame(len(chunk1), 0, 1, 1, chunk1) # END_STREAM
                                conn.sendall(h1 + d1)
                            elif ftype == 1:
                                # HEADERS frame on multiplexed stream (e.g. stream 3)
                                with self.lock:
                                    self.streams_served.append(stream_id)
                                    self.save_stats()
                                chunk2 = TEST_PAYLOAD
                                h_res = make_frame(1, 1, 4, stream_id, b"\x88")
                                d_res = make_frame(len(chunk2), 0, 1, stream_id, chunk2)
                                conn.sendall(h_res + d_res)
                                if len(self.streams_served) >= 2:
                                    time.sleep(0.05)
                                    conn.close()
                                    return
                    conn.close()
                    return
                else:
                    resp = (
                        f"HTTP/1.1 200 OK\r\n"
                        f"Content-Length: {len(TEST_PAYLOAD)}\r\n"
                        f"Connection: close\r\n\r\n"
                    ).encode("ascii") + TEST_PAYLOAD
                    conn.sendall(resp)
                    conn.close()
                    return

        except Exception:
            try:
                conn.close()
            except Exception:
                pass


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "fallback"
    stats_file = sys.argv[2] if len(sys.argv) > 2 else None
    server = MockServer(mode, stats_file)
    print(server.port, flush=True)
    if server.mode.startswith("https_alpn") and hasattr(server, "cert_file"):
        print(server.cert_file, flush=True)
    server.run()
