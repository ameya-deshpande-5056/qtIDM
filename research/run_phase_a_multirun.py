import subprocess
import time
import json
import hashlib
import os

def run_tests():
    server = subprocess.Popen(["python3", "research/benchmark_server.py"], stdout=subprocess.PIPE, text=True)
    port = int(server.stdout.readline().strip())
    print(f"Benchmark server started on port {port}")

    runs = []
    expected_hash = "44f9296993796e201208c6c245b9515d36b62c87d0be4459ff347bfa054cd527"

    for i in range(1, 6):
        target = f"/tmp/throttled_run_{i}.bin"
        if os.path.exists(target):
            os.remove(target)
        cmd = ["./build/benchmark_runner", f"http://127.0.0.1:{port}/tail_throttled_10mb.bin", target, "4", "1"]
        p = subprocess.run(cmd, stdout=subprocess.PIPE, text=True)
        res = json.loads(p.stdout.strip())
        with open(target, "rb") as f:
            file_hash = hashlib.sha256(f.read()).hexdigest()
        res["run"] = i
        res["sha256"] = file_hash
        res["sha256_match"] = (file_hash == expected_hash)
        runs.append(res)
        print(f"Run {i}: Elapsed = {res['elapsed_ms']} ms | Throughput = {res['throughput_mb_s']:.2f} MB/s | Peak RSS = {res['peak_rss_kb']} KB | SHA-256 Valid = {res['sha256_match']}")

    server.terminate()

    elapsed_times = [r["elapsed_ms"] for r in runs]
    elapsed_sorted = sorted(elapsed_times)
    median_elapsed = elapsed_sorted[len(elapsed_sorted) // 2]
    print(f"\n==========================================")
    print(f"--- Phase A Multi-Run Summary (5 runs) ---")
    print(f"==========================================")
    print(f"Individual elapsed times (ms): {elapsed_times}")
    print(f"Sorted elapsed times (ms):     {elapsed_sorted}")
    print(f"Median Elapsed Time:           {median_elapsed} ms")
    print(f"Best Run:                      {min(elapsed_times)} ms")
    print(f"Worst Run:                     {max(elapsed_times)} ms")
    print(f"All 5 SHA-256 Checksums Match: {all(r['sha256_match'] for r in runs)}")
    print(f"Peak RSS Across All Runs:      {max(r['peak_rss_kb'] for r in runs) / 1024.0:.2f} MB")
    
    with open("research/phase_a_multirun_results.json", "w") as f:
        json.dump({"runs": runs, "median_ms": median_elapsed, "all_valid": all(r['sha256_match'] for r in runs)}, f, indent=2)

if __name__ == "__main__":
    run_tests()
