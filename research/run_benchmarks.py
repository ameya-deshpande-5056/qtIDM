#!/usr/bin/env python3
import subprocess
import time
import json
import os
import shutil

SERVER_PY = "research/benchmark_server.py"
RUNNER_BIN = "./build/benchmark_runner"
OUT_DIR = "/tmp/qtidm_benchmarks"

def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    server_proc = subprocess.Popen(
        ["python3", SERVER_PY],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )

    port_line = server_proc.stdout.readline().strip()
    port = int(port_line)
    print(f"Benchmark server started on port {port}")

    scenarios = [
        {"name": "10MB_1_segment", "path": "/fast_10mb.bin", "segments": 1, "dynamic": 0},
        {"name": "10MB_4_segments_static", "path": "/fast_10mb.bin", "segments": 4, "dynamic": 0},
        {"name": "10MB_8_segments_dynamic", "path": "/fast_10mb.bin", "segments": 8, "dynamic": 1},
        {"name": "50MB_8_segments_dynamic", "path": "/fast_50mb.bin", "segments": 8, "dynamic": 1},
        {"name": "10MB_tail_throttled_4_segments", "path": "/tail_throttled_10mb.bin", "segments": 4, "dynamic": 0},
        {"name": "10MB_tail_throttled_8_segments_prealloc", "path": "/tail_throttled_10mb.bin", "segments": 8, "dynamic": 1},
    ]

    results = []

    try:
        for sc in scenarios:
            target_file = os.path.join(OUT_DIR, f"{sc['name']}.bin")
            if os.path.exists(target_file):
                os.remove(target_file)
            part_file = target_file + ".part"
            if os.path.exists(part_file):
                os.remove(part_file)

            url = f"http://127.0.0.1:{port}{sc['path']}"
            cmd = [RUNNER_BIN, url, target_file, str(sc['segments']), str(sc['dynamic'])]
            
            p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if p.returncode != 0:
                print(f"Error running {sc['name']}: {p.stderr}")
                continue

            try:
                data = json.loads(p.stdout.strip())
                data["scenario"] = sc["name"]
                data["segments"] = sc["segments"]
                data["dynamic"] = sc["dynamic"]
                results.append(data)
                print(f"[{sc['name']}] Elapsed: {data['elapsed_ms']}ms | Throughput: {data['throughput_mb_s']:.2f} MB/s | Peak RSS: {data['peak_rss_kb']} KB | Max Conn: {data['max_active_connections']}")
            except json.JSONDecodeError:
                print(f"Failed to parse JSON for {sc['name']}: {p.stdout}")

    finally:
        server_proc.terminate()
        server_proc.wait()
        if os.path.exists(OUT_DIR):
            shutil.rmtree(OUT_DIR, ignore_errors=True)

    with open("research/baseline_benchmark_results.json", "w") as f:
        json.dump(results, f, indent=2)
    print("Saved results to research/baseline_benchmark_results.json")

if __name__ == "__main__":
    main()

