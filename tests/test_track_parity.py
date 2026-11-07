#!/usr/bin/env python3
"""
Phase 6.1 Server-Browser Track Geometry Parity Verification Harness

Runs C++ Track geometry generator and JavaScript (browser) Track geometry generator,
comparing outputs across:
1. Total track length (tolerance <= 1e-3m)
2. Continuous scan across the entire circuit (pos <= 1e-4m, tan/norm <= 1e-5, angle <= 1e-5 rad)
3. Projected world coordinates toWorldPosition across multiple lateral offsets (<= 1e-4m)
4. Road boundary calculations (<= 1e-4m)
5. Wrap-around distance handling (< 0 and > totalLength)
6. Closed-loop C1 continuity across start/finish line
7. Degenerate segment handling
"""

import subprocess
import json
import math
import sys
import os

def run_cmd(cmd):
    proc = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if proc.returncode != 0:
        print(f"Command failed: {cmd}\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}", file=sys.stderr)
        sys.exit(1)
    return proc.stdout

def main():
    print("==================================================")
    print("Phase 6.1 Track Geometry Parity Verification")
    print("==================================================")

    base_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    os.chdir(base_dir)

    print("[1/5] Generating C++ Track outputs...")
    cpp_out_raw = run_cmd("./build/track_parity_cpp")
    cpp_data = json.loads(cpp_out_raw)

    print("[2/5] Generating JavaScript Track outputs via node.exe...")
    js_out_raw = run_cmd("node.exe tests/track_parity.mjs")
    js_data = json.loads(js_out_raw)

    print("[3/5] Verifying Total Track Length and Parameters...")
    cpp_len = cpp_data["totalLength"]
    js_len = js_data["totalLength"]
    diff_len = abs(cpp_len - js_len)
    print(f"  -> C++ Track Length: {cpp_len:.6f} m")
    print(f"  -> JS Track Length:  {js_len:.6f} m")
    print(f"  -> Delta Length:     {diff_len:.8f} m (threshold <= 1e-3m)")
    assert diff_len <= 1e-3, f"Track totalLength discrepancy: cpp={cpp_len}, js={js_len}, diff={diff_len}"
    assert cpp_data["trackWidth"] == js_data["trackWidth"], "Track width mismatch"

    print("[4/5] Comparing Continuous Scan Across Entire Circuit...")
    cpp_scan = cpp_data["scan"]
    js_scan = js_data["scan"]
    assert len(cpp_scan) == len(js_scan), f"Scan sample counts differ: cpp={len(cpp_scan)}, js={len(js_scan)}"

    max_pos_err = 0.0
    max_tan_err = 0.0
    max_norm_err = 0.0
    max_angle_err = 0.0

    for i in range(len(cpp_scan)):
        c = cpp_scan[i]
        j = js_scan[i]
        s = c["s"]

        # Centerline position error
        pos_err = math.hypot(c["cx"] - j["cx"], c["cy"] - j["cy"])
        if pos_err > max_pos_err: max_pos_err = pos_err

        # Tangent vector error
        tan_err = math.hypot(c["tx"] - j["tx"], c["ty"] - j["ty"])
        if tan_err > max_tan_err: max_tan_err = tan_err

        # Normal vector error
        norm_err = math.hypot(c["nx"] - j["nx"], c["ny"] - j["ny"])
        if norm_err > max_norm_err: max_norm_err = norm_err

        # Heading / Angle error (handling 2pi wrap if any)
        d_angle = abs(c["angle"] - j["angle"])
        if d_angle > math.pi: d_angle = 2.0 * math.pi - d_angle
        if d_angle > max_angle_err: max_angle_err = d_angle

        assert pos_err <= 1e-4, f"Pos error at s={s}: {pos_err} > 1e-4"
        assert tan_err <= 1e-5, f"Tan error at s={s}: {tan_err} > 1e-5"
        assert norm_err <= 1e-5, f"Norm error at s={s}: {norm_err} > 1e-5"
        assert d_angle <= 1e-5, f"Angle error at s={s}: {d_angle} > 1e-5"

    print(f"  -> Max Centerline Pos Error: {max_pos_err:.2e} m (limit 1e-4 m)")
    print(f"  -> Max Tangent Vector Error: {max_tan_err:.2e}   (limit 1e-5)")
    print(f"  -> Max Normal Vector Error:  {max_norm_err:.2e}   (limit 1e-5)")
    print(f"  -> Max Heading Angle Error:  {max_angle_err:.2e} rad (limit 1e-5 rad)")

    print("[5/5] Comparing World Projections, Boundaries, Wraps & Degenerate Cases...")
    
    # Projections
    cpp_proj = cpp_data["projections"]
    js_proj = js_data["projections"]
    assert len(cpp_proj) == len(js_proj)
    max_proj_err = 0.0
    for i in range(len(cpp_proj)):
        cp = cpp_proj[i]
        jp = js_proj[i]
        p_err = math.hypot(cp["wx"] - jp["wx"], cp["wy"] - jp["wy"])
        if p_err > max_proj_err: max_proj_err = p_err
        assert p_err <= 1e-4, f"Projection error at s={cp['s']}, d={cp['d']}: {p_err}"
    print(f"  -> Max World Projection Error: {max_proj_err:.2e} m (limit 1e-4 m)")

    # Boundaries
    cpp_b = cpp_data["boundaries"]
    js_b = js_data["boundaries"]
    assert len(cpp_b) == len(js_b)
    max_bound_err = 0.0
    for i in range(len(cpp_b)):
        cb = cpp_b[i]
        jb = js_b[i]
        l_err = math.hypot(cb["lx"] - jb["lx"], cb["ly"] - jb["ly"])
        r_err = math.hypot(cb["rx"] - jb["rx"], cb["ry"] - jb["ry"])
        max_bound_err = max(max_bound_err, l_err, r_err)
        assert l_err <= 1e-4 and r_err <= 1e-4
    print(f"  -> Max Road Boundary Error:    {max_bound_err:.2e} m (limit 1e-4 m)")

    # Wrap-arounds
    cpp_w = cpp_data["wraps"]
    js_w = js_data["wraps"]
    assert len(cpp_w) == len(js_w)
    max_wrap_err = 0.0
    for i in range(len(cpp_w)):
        cw = cpp_w[i]
        jw = js_w[i]
        w_err = math.hypot(cw["cx"] - jw["cx"], cw["cy"] - jw["cy"])
        if w_err > max_wrap_err: max_wrap_err = w_err
        assert w_err <= 1e-4
    print(f"  -> Max Wrap-Around Error:     {max_wrap_err:.2e} m (limit 1e-4 m)")

    # Degenerate Segment
    cd = cpp_data["degenerate"]
    jd = js_data["degenerate"]
    d_err = math.hypot(cd["cx"] - jd["cx"], cd["cy"] - jd["cy"])
    assert d_err <= 1e-4
    print(f"  -> Degenerate Segment Error:   {d_err:.2e} m")

    # Closed-loop C1 continuity across finish line
    s_end = cpp_data["totalLength"]
    sample_start = cpp_scan[0] # s = 0.0
    sample_end = cpp_scan[-1]   # s = totalLength
    finish_jump = math.hypot(sample_start["cx"] - sample_end["cx"], sample_start["cy"] - sample_end["cy"])
    tan_dot = sample_start["tx"] * sample_end["tx"] + sample_start["ty"] * sample_end["ty"]
    print(f"  -> Closed-Loop C0 Jump at s=0: {finish_jump:.2e} m")
    print(f"  -> Closed-Loop C1 Tangent Alignment (T_start · T_end): {tan_dot:.8f}")
    assert finish_jump <= 1e-3, f"Closed-loop positional seam jump: {finish_jump} m"
    assert abs(tan_dot - 1.0) <= 1e-4, f"Closed-loop tangent alignment error: {tan_dot}"

    print("==================================================")
    print("SERVER-BROWSER TRACK GEOMETRY PARITY CONFIRMED 100%!")
    print("==================================================")

if __name__ == "__main__":
    main()
