"""Proves that tools/ai/features.py computes exactly what the ESP32 computes.

Compiles src/gateway/ai_features.c for this computer (needs `cc`: Xcode command line tools on a Mac),
feeds the same random sequence of windows (with gaps and quiet/busy stretches) to both, and compares.
Run:  python3 tools/ai/test_parity.py
"""
import ctypes, math, os, random, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "..", "src", "gateway", "ai_features.c")
sys.path.insert(0, HERE)
from features import FeatureState, AIF_N, FEATURE_NAMES  # noqa: E402


class AifInput(ctypes.Structure):
    _fields_ = [("t", ctypes.c_double), ("jerk_mg", ctypes.c_float), ("move_count", ctypes.c_float),
                ("gyro_cdps", ctypes.c_float), ("tof_valid", ctypes.c_float), ("tof_n", ctypes.c_float),
                ("tof_mm", ctypes.c_float), ("tof_std_mm", ctypes.c_float), ("lux", ctypes.c_float),
                ("wake_rel_min", ctypes.c_float)]


def build_lib(tmp):
    lib = os.path.join(tmp, "libaif.so")
    subprocess.check_call(["cc", "-O2", "-shared", "-fPIC", "-o", lib, SRC, "-lm"])
    return ctypes.CDLL(lib)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        lib = build_lib(tmp)
        state = ctypes.create_string_buffer(4096)          # larger than sizeof(AifState)
        lib.aif_reset(state)
        out = (ctypes.c_float * AIF_N)()
        py = FeatureState()
        rnd = random.Random(42)
        t = 1_790_000_000.0
        worst = 0.0
        for i in range(5000):
            t += 10.0 if rnd.random() > 0.01 else rnd.choice([130.0, 400.0, 11.0])   # occasional gaps
            busy = (i // 300) % 2 == 0
            row = dict(
                jerk_mg=float(rnd.randint(0, 400 if busy else 15)),
                move_count=float(rnd.randint(0, 120 if busy else 2)),
                gyro_cdps=float(rnd.randint(0, 3000)),
                tof_n=50.0, tof_valid=float(rnd.randint(0, 50)),
                tof_mm=float(rnd.randint(100, 2000)), tof_std_mm=float(rnd.randint(0, 80)),
                lux=float("nan") if rnd.random() < 0.05 else rnd.uniform(0, 800),
                wake_rel_min=float("nan") if rnd.random() < 0.02 else rnd.uniform(-400, 400),
            )
            inp = AifInput(t=t, **row)
            lib.aif_step(state, ctypes.byref(inp), out)
            ref = py.step(t, **row)
            for k in range(AIF_N):
                c, p = out[k], ref[k]
                err = abs(c - p) / max(1.0, abs(p))
                worst = max(worst, err)
                if err > 1e-4:
                    print(f"MISMATCH step {i} feature {FEATURE_NAMES[k]}: C={c} py={p}")
                    sys.exit(1)
        print(f"parity OK: 5000 windows x {AIF_N} features, worst relative error {worst:.2e}")


if __name__ == "__main__":
    main()
