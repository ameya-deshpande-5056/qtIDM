#!/usr/bin/env python3
"""Exercise a real browser extension download through native messaging."""

from __future__ import annotations

import argparse
import base64
import hashlib
import http.server
import json
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time


REPOSITORY = Path(__file__).resolve().parents[1]
NATIVE_HOST_NAME = "io.github.qtidm.native"
CHROME_TEST_KEY = (
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA0UiT8cUZyA+QhTLPZ4u4T3lWtYl63/"
    "ulmbm53hPT2Wgu5TXokxD1cooHgpJxN64ALr2eqs3nOirweKLu4eL+gN0AWqZHpGhCdJ9qafd"
    "OuhVT80ZFrMvK8F+1DIcYqO/q0zcgGXyt+KZX44MyyC4NaYsMQSQWIzq0/4at0FL55fUnVC3"
    "Knd9XyFxEk+ZT6ct3Z+tXl7E/oN1xRhUvzfVf7gaxwremn9WKENr2ZEPDTVjb2rSzeFXi2wx9"
    "z7bswKrZxady5OqloMtLTz05HQyrAPOMRg/LZFw4U73bMfE90cDHnALsQCErAMQ9t7DtYDZ70"
    "lg45ZNI1tPY6ETenkw9yQIDAQAB"
)


class FixtureHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        if self.path.startswith("/download.bin"):
            body = b"qtIDM browser extension end-to-end fixture\n"
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Disposition", 'attachment; filename="qtidm-e2e.bin"')
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        body = b"""<!doctype html>
<meta charset="utf-8">
<title>qtIDM extension E2E</title>
<script>
document.cookie = "qtidm_e2e=cookie; SameSite=Lax; path=/";
window.addEventListener("load", () => {
  const trigger = () => {
    const link = document.createElement("a");
    link.href = "/download.bin?from=e2e";
    link.download = "qtidm-e2e.bin";
    document.body.appendChild(link);
    link.click();
    link.remove();
  };
  setTimeout(trigger, 1500);
  setInterval(trigger, 4000);
});
</script>
<body>qtIDM extension end-to-end fixture</body>
"""
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Set-Cookie", "qtidm_e2e=cookie; SameSite=Lax; Path=/")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, _format: str, *_args: object) -> None:
        return


def chrome_extension_id(public_key: str) -> str:
    digest = hashlib.sha256(base64.b64decode(public_key)).hexdigest()[:32]
    return "".join(chr(ord("a") + int(character, 16)) for character in digest)


def write_mock_host(path: Path) -> None:
    path.write_text(
        """#!/usr/bin/env python3
import json
import os
import struct
import sys

raw_length = sys.stdin.buffer.read(4)
if len(raw_length) != 4:
    raise SystemExit(2)
length = struct.unpack("<I", raw_length)[0]
payload = sys.stdin.buffer.read(length)
if len(payload) != length:
    raise SystemExit(3)
message = json.loads(payload.decode("utf-8"))
with open(os.environ["QTIDM_E2E_NATIVE_LOG"], "a", encoding="utf-8") as output:
    output.write(json.dumps(message, separators=(",", ":")) + "\\n")
response = json.dumps({"ok": True, "message": "accepted by E2E host"}).encode("utf-8")
sys.stdout.buffer.write(struct.pack("<I", len(response)))
sys.stdout.buffer.write(response)
sys.stdout.buffer.flush()
""",
        encoding="utf-8",
    )
    path.chmod(0o755)


def install_native_manifests(home: Path, host: Path, chrome_id: str) -> None:
    chrome_manifest = {
        "name": NATIVE_HOST_NAME,
        "description": "qtIDM extension end-to-end test host",
        "path": str(host),
        "type": "stdio",
        "allowed_origins": [f"chrome-extension://{chrome_id}/"],
    }
    firefox_manifest = {
        "name": NATIVE_HOST_NAME,
        "description": "qtIDM extension end-to-end test host",
        "path": str(host),
        "type": "stdio",
        "allowed_extensions": ["qtidm@io.github.qtidm"],
    }

    chrome_directories = [
        *(
            home / ".config" / product / "NativeMessagingHosts"
            for product in (
                "google-chrome",
                "google-chrome-for-testing",
                "chrome-for-testing",
                "google-chrome-beta",
                "google-chrome-unstable",
                "chromium",
            )
        ),
    ]
    for directory in chrome_directories:
        directory.mkdir(parents=True, exist_ok=True)
        (directory / f"{NATIVE_HOST_NAME}.json").write_text(
            json.dumps(chrome_manifest), encoding="utf-8"
        )
    (host.parent / f"{NATIVE_HOST_NAME}.json").write_text(
        json.dumps(chrome_manifest), encoding="utf-8"
    )
    firefox_directory = home / ".mozilla" / "native-messaging-hosts"
    firefox_directory.mkdir(parents=True, exist_ok=True)
    (firefox_directory / f"{NATIVE_HOST_NAME}.json").write_text(
        json.dumps(firefox_manifest), encoding="utf-8"
    )


def prepare_chrome_extension(destination: Path) -> str:
    shutil.copytree(REPOSITORY / "browser" / "chrome", destination)
    manifest_path = destination / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["key"] = CHROME_TEST_KEY
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return chrome_extension_id(CHROME_TEST_KEY)


def find_chrome() -> str | None:
    configured = os.environ.get("QTIDM_E2E_CHROME")
    if configured:
        return configured
    for candidate in (
        "chrome",
        "chrome-for-testing",
        "google-chrome-stable",
        "google-chrome",
        "chromium",
        "chromium-browser",
    ):
        executable = shutil.which(candidate)
        if executable:
            return executable
    return None


def find_firefox() -> str | None:
    return os.environ.get("QTIDM_E2E_FIREFOX") or shutil.which("firefox")


def global_node_modules() -> str | None:
    configured = os.environ.get("QTIDM_E2E_NODE_PATH")
    if configured:
        return configured
    npm = shutil.which("npm")
    if not npm:
        return None
    try:
        return subprocess.check_output(
            [npm, "root", "--global"], text=True, stderr=subprocess.STDOUT, timeout=10
        ).strip()
    except (OSError, subprocess.SubprocessError):
        return None


def browser_command(browser: str, root: Path, url: str) -> tuple[list[str], str | None]:
    if browser == "chrome":
        executable = find_chrome()
        if not executable:
            return [], "Chrome/Chromium is not installed"
        node = shutil.which("node")
        node_modules = global_node_modules()
        if not node or not node_modules or not (Path(node_modules) / "puppeteer").is_dir():
            return [], "Node.js and the Puppeteer package are required"
        try:
            version = subprocess.check_output(
                [executable, "--version"], text=True, stderr=subprocess.STDOUT, timeout=5
            ).strip()
        except (OSError, subprocess.SubprocessError):
            version = ""
        if (
            "Google Chrome" in version
            and "for Testing" not in version
            and os.environ.get("QTIDM_E2E_ALLOW_BRANDED_CHROME") != "1"
        ):
            return [], (
                "branded Chrome no longer loads unpacked extensions from the command line; "
                "use Chrome for Testing or Chromium"
            )
        extension = root / "chrome-extension"
        prepare_chrome_extension(extension)
        return [
            node,
            str(REPOSITORY / "tests" / "chrome_extension_e2e.cjs"),
            executable,
            str(extension),
            str(root / "chrome-profile"),
            str(root / f"{NATIVE_HOST_NAME}.json"),
            url,
        ], None

    firefox = find_firefox()
    web_ext = os.environ.get("QTIDM_E2E_WEB_EXT") or shutil.which("web-ext")
    if not firefox:
        return [], "Firefox is not installed"
    if not web_ext:
        return [], "web-ext is not installed"
    return [
        web_ext,
        "run",
        "--source-dir",
        str(REPOSITORY / "browser" / "firefox"),
        "--firefox",
        firefox,
        "--no-reload",
        "--start-url",
        url,
    ], None


def load_messages(log_path: Path) -> list[dict[str, object]]:
    if not log_path.exists():
        return []
    messages: list[dict[str, object]] = []
    for line in log_path.read_text(encoding="utf-8").splitlines():
        try:
            messages.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return messages


def stop_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=5)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=5)


def validate_message(message: dict[str, object], base_url: str) -> None:
    expected_url = f"{base_url}/download.bin?from=e2e"
    if message.get("url") != expected_url:
        raise AssertionError(f"unexpected intercepted URL: {message.get('url')!r}")
    if message.get("suggestedFilename") != "qtidm-e2e.bin":
        raise AssertionError(f"browser filename was not forwarded: {message!r}")
    headers = message.get("headers")
    if not isinstance(headers, dict):
        raise AssertionError(f"missing native-message headers: {message!r}")
    if "qtidm_e2e=cookie" not in str(headers.get("Cookie", "")):
        raise AssertionError(f"browser cookie was not forwarded: {headers!r}")
    if not str(headers.get("Referer", "")).startswith(base_url):
        raise AssertionError(f"browser referrer was not forwarded: {headers!r}")
    if not str(headers.get("User-Agent", "")).strip():
        raise AssertionError(f"browser User-Agent was not forwarded: {headers!r}")


def run(browser: str, strict: bool) -> int:
    with tempfile.TemporaryDirectory(prefix=f"qtidm-{browser}-e2e-") as temporary:
        root = Path(temporary)
        home = root / "home"
        home.mkdir()
        log_path = root / "native-messages.jsonl"
        browser_log = root / "browser.log"
        host = root / "qtidm-e2e-native-host"
        write_mock_host(host)

        extension_id = chrome_extension_id(CHROME_TEST_KEY)
        if extension_id != "nnkhmdknhefaakkkojbmopapnibiimih":
            raise AssertionError(f"unexpected deterministic Chrome extension ID: {extension_id}")
        install_native_manifests(home, host, extension_id)

        command, missing = browser_command(browser, root, "http://127.0.0.1/")
        if missing:
            print(f"SKIP: {missing}", file=sys.stderr)
            return 1 if strict else 77

        try:
            server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FixtureHandler)
        except PermissionError as error:
            print(f"SKIP: the sandbox does not permit a localhost fixture server ({error})", file=sys.stderr)
            return 1 if strict else 77
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        base_url = f"http://127.0.0.1:{server.server_port}"
        command[-1] = base_url

        environment = os.environ.copy()
        environment.update(
            {
                "HOME": str(home),
                "XDG_CONFIG_HOME": str(home / ".config"),
                "QTIDM_E2E_NATIVE_LOG": str(log_path),
                "MOZ_HEADLESS": "1",
            }
        )
        node_modules = global_node_modules()
        if node_modules:
            environment["NODE_PATH"] = node_modules
        process: subprocess.Popen[bytes] | None = None
        try:
            with browser_log.open("wb") as output:
                process = subprocess.Popen(
                    command,
                    env=environment,
                    stdout=output,
                    stderr=subprocess.STDOUT,
                    start_new_session=True,
                )
                deadline = time.monotonic() + 40
                while time.monotonic() < deadline:
                    messages = load_messages(log_path)
                    if messages:
                        validate_message(messages[0], base_url)
                        print(f"PASS: {browser} intercepted a download through native messaging")
                        return 0
                    if process.poll() is not None:
                        break
                    time.sleep(0.25)
        finally:
            if process is not None:
                stop_process(process)
            server.shutdown()
            server.server_close()

        details = browser_log.read_text(encoding="utf-8", errors="replace")[-6000:]
        raise AssertionError(
            f"{browser} did not deliver an intercepted download to the native host.\n"
            f"Browser output:\n{details}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--browser", choices=("chrome", "firefox"), required=True)
    parser.add_argument(
        "--strict",
        action="store_true",
        default=os.environ.get("QTIDM_BROWSER_E2E_STRICT") == "1",
        help="fail instead of skipping when the requested browser tooling is unavailable",
    )
    arguments = parser.parse_args()
    try:
        return run(arguments.browser, arguments.strict)
    except Exception as error:  # keep CI output compact and actionable
        print(f"FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
