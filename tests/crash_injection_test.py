#!/usr/bin/env python3
import hashlib
import os
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest

SOURCE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(SOURCE_DIR, "build")
CRASH_WORKER_BIN = os.path.join(BUILD_DIR, "qtIDM_crash_worker")
SERVER_SCRIPT = os.path.join(SOURCE_DIR, "tests", "fixtures", "http_test_server.py")
CRASH_TEST_SIZE = 2 * 1024 * 1024


def expected_payload(size):
    return bytes((i % 251) for i in range(size))


class CrashInjectionTest(unittest.TestCase):
    def setUp(self):
        self.server_proc = subprocess.Popen(
            [sys.executable, SERVER_SCRIPT],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )
        line = self.server_proc.stdout.readline()
        self.port = int(line.strip())
        self.temp_dir = tempfile.TemporaryDirectory()
        self.db_path = os.path.join(self.temp_dir.name, "downloads.sqlite3")
        self.target_path = os.path.join(self.temp_dir.name, "crash_target.bin")

    def tearDown(self):
        if self.server_proc.poll() is None:
            self.server_proc.terminate()
            self.server_proc.wait()
        if self.server_proc.stdout:
            self.server_proc.stdout.close()
        if self.server_proc.stderr:
            self.server_proc.stderr.close()
        self.temp_dir.cleanup()

    def test_out_of_process_sigkill_crash_and_durable_resume(self):
        url = f"http://127.0.0.1:{self.port}/crash-test.bin"

        # 1. Start download worker
        worker = subprocess.Popen(
            [
                CRASH_WORKER_BIN,
                "--db", self.db_path,
                "--url", url,
                "--target", self.target_path,
                "--mode", "download",
                "--segments", "4",
                "--total", str(CRASH_TEST_SIZE)
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )

        # 2. Wait until at least one durable checkpoint has been committed to SQLite
        checkpoint_seen = False
        start_time = time.time()
        while time.time() - start_time < 10.0:
            line = worker.stdout.readline()
            if "CHECKPOINT COMMITTED" in line:
                checkpoint_seen = True
                break

        self.assertTrue(checkpoint_seen, "Worker did not commit a durable checkpoint in time")

        # 3. Inject SIGKILL (kill -9) directly to the running worker process
        worker_pid = worker.pid
        os.kill(worker_pid, signal.SIGKILL)
        worker.wait()
        if worker.stdout:
            worker.stdout.close()
        if worker.stderr:
            worker.stderr.close()

        self.assertEqual(worker.returncode, -signal.SIGKILL, "Process must be terminated by SIGKILL")

        # 4. Verify SQLite database durability and segment checkpoints
        conn = sqlite3.connect(self.db_path)
        cursor = conn.cursor()
        cursor.execute("SELECT segment_index, range_start, range_end, written, durable_written FROM segments ORDER BY segment_index")
        rows = cursor.fetchall()
        conn.close()

        self.assertGreater(len(rows), 0, "Database must retain committed segments after SIGKILL")
        total_durable = 0
        for seg_idx, start, end, written, durable_written in rows:
            self.assertLessEqual(durable_written, written, "durable_written must never exceed written")
            self.assertGreaterEqual(durable_written, 0)
            total_durable += durable_written

        self.assertGreater(total_durable, 0, "At least some bytes must have been durably synchronized")

        # 5. Launch worker in resume mode
        resume_worker = subprocess.run(
            [
                CRASH_WORKER_BIN,
                "--db", self.db_path,
                "--url", url,
                "--target", self.target_path,
                "--mode", "resume",
                "--segments", "4",
                "--total", str(CRASH_TEST_SIZE)
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=20
        )

        self.assertEqual(resume_worker.returncode, 0, f"Resume worker failed: {resume_worker.stderr}")

        # 6. Verify file integrity and exact SHA-256 match
        self.assertTrue(os.path.exists(self.target_path), "Target file must exist after completion")
        self.assertEqual(os.path.getsize(self.target_path), CRASH_TEST_SIZE)

        with open(self.target_path, "rb") as f:
            actual_data = f.read()

        expected_data = expected_payload(CRASH_TEST_SIZE)
        self.assertEqual(hashlib.sha256(actual_data).hexdigest(), hashlib.sha256(expected_data).hexdigest())


if __name__ == "__main__":
    unittest.main()
