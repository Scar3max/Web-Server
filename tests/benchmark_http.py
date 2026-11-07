"""
Phase 7 HTTP Benchmark Suite.
Benchmarks the HTTP static file and API server at varying concurrency levels (50, 100, 200).
Collects throughput (req/sec, MB/s) and latency percentiles (p50, p95, p99).
"""

import subprocess
import re
import sys
import json

HOST = "127.0.0.1"
PORT = 8080
PATH = "/"

def run_ab(requests, concurrency):
    url = f"http://{HOST}:{PORT}{PATH}"
    cmd = ["ab", "-n", str(requests), "-c", str(concurrency), url]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, check=True)
        output = proc.stdout
    except subprocess.CalledProcessError as e:
        print(f"Error running ab: {e.stderr}", file=sys.stderr)
        return None

    results = {
        "concurrency": concurrency,
        "total_requests": requests,
        "rps": 0.0,
        "transfer_rate_kb": 0.0,
        "time_per_request_ms": 0.0,
        "failed_requests": 0,
        "p50_ms": 0.0,
        "p90_ms": 0.0,
        "p95_ms": 0.0,
        "p99_ms": 0.0,
        "p100_ms": 0.0
    }

    # Extract metrics using regex
    m_rps = re.search(r"Requests per second:\s+([\d.]+)", output)
    if m_rps:
        results["rps"] = float(m_rps.group(1))

    m_tr = re.search(r"Transfer rate:\s+([\d.]+)", output)
    if m_tr:
        results["transfer_rate_kb"] = float(m_tr.group(1))

    m_tpr = re.search(r"Time per request:\s+([\d.]+)\s+\[ms\]\s+\(mean\)", output)
    if m_tpr:
        results["time_per_request_ms"] = float(m_tpr.group(1))

    m_fail = re.search(r"Failed requests:\s+(\d+)", output)
    if m_fail:
        results["failed_requests"] = int(m_fail.group(1))

    m_50 = re.search(r"50%\s+(\d+)", output)
    if m_50:
        results["p50_ms"] = float(m_50.group(1))

    m_90 = re.search(r"90%\s+(\d+)", output)
    if m_90:
        results["p90_ms"] = float(m_90.group(1))

    m_95 = re.search(r"95%\s+(\d+)", output)
    if m_95:
        results["p95_ms"] = float(m_95.group(1))

    m_99 = re.search(r"99%\s+(\d+)", output)
    if m_99:
        results["p99_ms"] = float(m_99.group(1))

    m_100 = re.search(r"100%\s+(\d+)\s+\(longest request\)", output)
    if m_100:
        results["p100_ms"] = float(m_100.group(1))

    return results

def main():
    concurrency_levels = [50, 100, 200]
    total_requests = 10000

    print("================================================================================")
    print("                      PHASE 7 HTTP PERFORMANCE BENCHMARK                       ")
    print("================================================================================")
    print(f"Target: http://{HOST}:{PORT}{PATH} | Total Requests per Level: {total_requests}")
    print("--------------------------------------------------------------------------------")
    print(f"{'Concurrency':<12} | {'Req/sec (RPS)':<15} | {'p50 (ms)':<10} | {'p95 (ms)':<10} | {'p99 (ms)':<10} | {'Failures':<8}")
    print("--------------------------------------------------------------------------------")

    all_results = []
    for c in concurrency_levels:
        res = run_ab(total_requests, c)
        if res:
            all_results.append(res)
            print(f"{res['concurrency']:<12} | {res['rps']:<15.2f} | {res['p50_ms']:<10.1f} | {res['p95_ms']:<10.1f} | {res['p99_ms']:<10.1f} | {res['failed_requests']:<8}")
        else:
            print(f"Failed at concurrency {c}")

    print("================================================================================")
    return all_results

if __name__ == "__main__":
    main()
