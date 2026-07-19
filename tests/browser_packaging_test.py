#!/usr/bin/env python3
"""Exercise browser package orchestration without external signing services."""

from __future__ import annotations

import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile
import textwrap
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[1]
BUILD_SCRIPT = ROOT / "packaging" / "browser" / "build-extensions.sh"
ID_SCRIPT = ROOT / "packaging" / "browser" / "chrome-extension-id.sh"


class BrowserPackagingTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("openssl") and shutil.which("zip"), "openssl and zip are required")
    def test_development_and_release_packages(self) -> None:
        with tempfile.TemporaryDirectory(prefix="qtidm-browser-packaging-") as directory:
            work = Path(directory)
            tools = work / "bin"
            tools.mkdir()
            self._write_executable(
                tools / "web-ext",
                """
                #!/usr/bin/env python3
                import sys
                from pathlib import Path
                import zipfile

                command = sys.argv[1]
                args = sys.argv[2:]
                if command == "lint":
                    raise SystemExit(0)

                def value(flag):
                    return args[args.index(flag) + 1]

                source = Path(value("--source-dir"))
                artifacts = Path(value("--artifacts-dir"))
                artifacts.mkdir(parents=True, exist_ok=True)
                if command == "build":
                    destination = artifacts / value("--filename")
                elif command == "sign":
                    destination = artifacts / "qtidm-signed.xpi"
                else:
                    raise SystemExit(f"unsupported mock command: {command}")
                with zipfile.ZipFile(destination, "w") as package:
                    for path in source.rglob("*"):
                        if path.is_file():
                            package.write(path, path.relative_to(source))
                    if command == "sign":
                        package.writestr("META-INF/mozilla.rsa", b"test signature")
                """,
            )
            self._write_executable(
                tools / "chrome",
                """
                #!/usr/bin/env python3
                import sys
                from pathlib import Path

                source_arg = next(arg for arg in sys.argv[1:] if arg.startswith("--pack-extension="))
                source = Path(source_arg.split("=", 1)[1])
                source.with_suffix(".crx").write_bytes(b"Cr24test package")
                """,
            )

            key = work / "chrome.pem"
            subprocess.run(
                ["openssl", "genrsa", "-out", str(key), "2048"],
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            extension_id = subprocess.check_output(
                ["sh", str(ID_SCRIPT), str(key)], text=True
            ).strip()
            self.assertRegex(extension_id, re.compile(r"^[a-p]{32}$"))

            env = os.environ.copy()
            env["PATH"] = f"{tools}{os.pathsep}{env['PATH']}"

            development = work / "development"
            subprocess.run(
                ["sh", str(BUILD_SCRIPT), "--mode", "development", "--output", str(development)],
                cwd=ROOT,
                env=env,
                check=True,
            )
            self.assertTrue((development / "qtidm-chrome.zip").is_file())
            self.assertTrue((development / "qtidm-firefox-unsigned.xpi").is_file())
            self._assert_package_branding(development / "qtidm-chrome.zip")
            self._assert_package_branding(development / "qtidm-firefox-unsigned.xpi")

            release = work / "release"
            env.update(
                {
                    "QTIDM_CHROME_EXECUTABLE": str(tools / "chrome"),
                    "QTIDM_CHROME_EXTENSION_KEY": str(key),
                    "QTIDM_CHROME_EXTENSION_ID": extension_id,
                    "WEB_EXT_API_KEY": "test-api-key",
                    "WEB_EXT_API_SECRET": "test-api-secret",
                }
            )
            mismatched_env = env.copy()
            mismatched_env["QTIDM_CHROME_EXTENSION_ID"] = "a" * 32
            mismatch = subprocess.run(
                [
                    "sh",
                    str(BUILD_SCRIPT),
                    "--mode",
                    "release",
                    "--output",
                    str(work / "mismatched-release"),
                ],
                cwd=ROOT,
                env=mismatched_env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertNotEqual(mismatch.returncode, 0)
            self.assertIn("expected", mismatch.stderr)

            subprocess.run(
                ["sh", str(BUILD_SCRIPT), "--mode", "release", "--output", str(release)],
                cwd=ROOT,
                env=env,
                check=True,
            )
            self.assertEqual((release / "qtidm-chrome.crx").read_bytes()[:4], b"Cr24")
            with zipfile.ZipFile(release / "qtidm-chrome.zip") as package:
                chrome_manifest = json.loads(package.read("manifest.json"))
                public_key = base64.b64decode(chrome_manifest["key"])
                digest = hashlib.sha256(public_key).hexdigest()[:32]
                packaged_id = "".join(
                    chr(ord("a") + int(character, 16)) for character in digest
                )
                self.assertEqual(packaged_id, extension_id)
            self._assert_package_branding(release / "qtidm-chrome.zip")
            with zipfile.ZipFile(release / "qtidm-firefox.xpi") as package:
                self.assertIn("META-INF/mozilla.rsa", package.namelist())
            self._assert_package_branding(release / "qtidm-firefox.xpi")

            template = json.loads(
                (ROOT / "browser" / "chrome" / "external-extension.json.in")
                .read_text(encoding="utf-8")
                .replace("@QTIDM_BROWSER_EXTENSION_VERSION@", "1.2.3")
            )
            self.assertEqual(template["external_version"], "1.2.3")
            self.assertEqual(
                template["external_crx"],
                "/usr/share/qtidm/browser/packages/qtidm-chrome.crx",
            )

    @staticmethod
    def _assert_package_branding(path: Path) -> None:
        with zipfile.ZipFile(path) as package:
            names = package.namelist()
            assert "popup.css" in names
            assert "popup.html" in names
            assert "icons/icon-48.png" in names
            css = package.read("popup.css").decode("utf-8")
            popup = package.read("popup.html").decode("utf-8")
            assert "--brand: #6d2f45" in css
            assert "#2457a6" not in css.lower()
            assert 'src="icons/icon-48.png"' in popup

    @staticmethod
    def _write_executable(path: Path, source: str) -> None:
        path.write_text(textwrap.dedent(source).lstrip(), encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)


if __name__ == "__main__":
    unittest.main()
