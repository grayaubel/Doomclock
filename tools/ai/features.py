"""Python mirror of src/gateway/ai_features.c — keep the two identical (test_parity.py checks it)."""
import math

AIF_N = 11
AIF_HIST = 40
AIF_GAP_RESET_S = 120.0
AIF_ACTIVE_MOVES = 3.0

FEATURE_NAMES = [
    "jerk_mg", "move_count", "gyro_dps", "moves_1min", "moves_5min", "min_since_move",
    "tof_valid_frac", "tof_mm", "tof_std_mm", "lux_log", "wake_rel_min",
]


def _clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


def _nan(v):
    return v is None or (isinstance(v, float) and math.isnan(v))


class FeatureState:
    def __init__(self):
        self.reset()

    def reset(self):
        self.hist = []          # (t, moves), newest last, at most AIF_HIST
        self.last_t = 0.0
        self.last_active_t = 0.0
        self.have = False

    def step(self, t, jerk_mg, move_count, gyro_cdps, tof_valid, tof_n, tof_mm, tof_std_mm, lux, wake_rel_min):
        if (not self.have) or (t - self.last_t > AIF_GAP_RESET_S) or (t < self.last_t):
            self.reset()
            self.have = True
            self.last_active_t = t
        self.hist.append((t, move_count))
        if len(self.hist) > AIF_HIST:
            self.hist.pop(0)
        if move_count >= AIF_ACTIVE_MOVES:
            self.last_active_t = t
        self.last_t = t

        m1 = sum(m for (ht, m) in self.hist if ht > t - 60.0)
        m5 = sum(m for (ht, m) in self.hist if ht > t - 300.0)
        return [
            float(jerk_mg),
            float(move_count),
            float(gyro_cdps) / 100.0,
            float(m1),
            float(m5),
            _clamp((t - self.last_active_t) / 60.0, 0.0, 60.0),
            (tof_valid / tof_n) if tof_n > 0 else 0.0,
            float(tof_mm) if tof_valid > 0 else 0.0,
            float(tof_std_mm),
            0.0 if (_nan(lux) or lux < 0) else math.log10(1.0 + lux),
            0.0 if _nan(wake_rel_min) else _clamp(wake_rel_min, -180.0, 180.0),
        ]
