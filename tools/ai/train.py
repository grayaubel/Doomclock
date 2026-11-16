"""Train the Doomclock sleep/wake classifier and export it for the ESP32.

    pip install -r tools/ai/requirements.txt
    python3 tools/ai/train.py data/*.csv [--labels tools/ai/manual_labels.csv] [--out src/gateway/ai_model.h]

Input: the gateway's records exported from NETPIE (Feed -> Export), CSV or JSON, one or many files.
The loader accepts a wide table (one column per field) or a long table (time, name, value).

Labels, from either or both:
  * data-collection mode: every bed record carries `label` (ASLEEP / AWAKE_IN_BED / UP), set by
    tapping I'M AWAKE, missing a probe, and pressing the button when up;
  * --labels CSV with columns start,end,label (local time "2026-11-16 07:05"), from the paper log.

Classes: 0 ASLEEP, 1 AWAKE_IN_BED, 2 UP. Features come from features.py, which mirrors the
firmware exactly (see test_parity.py). The model is a small random forest (fits easily in flash).
Evaluation is leave-one-night-out, so the accuracy reported is on nights the model never saw;
it also reports the accuracy after the firmware's 3-window smoothing.
"""
import argparse, json, math, os, sys
from datetime import datetime, timedelta, timezone

import numpy as np
import pandas as pd

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from features import FeatureState, FEATURE_NAMES  # noqa: E402

CLASSES = ["ASLEEP", "AWAKE_IN_BED", "UP"]
TZ = timezone(timedelta(hours=7))                 # Asia/Bangkok
BOUNDARY_EXCLUDE_S = 60                           # drop windows this close to a label change
PROBE_EXCLUDE_S = 270                             # awake windows just before a missed probe are uncertain
TIME_COLS = ["timestamp", "time", "ts", "datetime", "date", "Time", "Timestamp", "Date"]


# ---------------------------------------------------------------- loading
def _to_epoch(series):
    s = series.dropna()
    if s.empty:
        return pd.Series(dtype=float)
    if pd.api.types.is_numeric_dtype(series):
        v = series.astype(float)
        return np.where(v > 1e12, v / 1000.0, v)                 # ms or s
    dt = pd.to_datetime(series, errors="coerce", utc=False)
    if getattr(dt.dt, "tz", None) is None:
        dt = dt.dt.tz_localize(TZ)
    return dt.dt.tz_convert("UTC").astype("int64") / 1e9


def load_file(path):
    if path.lower().endswith(".json"):
        with open(path) as f:
            raw = json.load(f)
        if isinstance(raw, dict):
            for key in ("data", "records", "items", "series"):
                if key in raw:
                    raw = raw[key]
                    break
        if isinstance(raw, list) and raw and isinstance(raw[0], dict) and "values" in raw[0]:
            rows = []                                            # [{"name":..., "values":[[ts, v], ...]}]
            for s in raw:
                for ts, v in s["values"]:
                    rows.append({"t": ts, "name": s.get("name") or s.get("attr"), "value": v})
            df = pd.DataFrame(rows)
        else:
            df = pd.json_normalize(raw)
            df.columns = [c.split(".")[-1] for c in df.columns]
    else:
        df = pd.read_csv(path)

    tcol = next((c for c in df.columns if c in TIME_COLS or c == "t"), None)
    if tcol is None:
        raise SystemExit(f"{path}: no time column (looked for {TIME_COLS})")
    namecol = next((c for c in df.columns if c.lower() in ("name", "datatag", "attr", "key", "field")), None)
    if namecol and "value" in [c.lower() for c in df.columns]:  # long format -> wide
        vcol = [c for c in df.columns if c.lower() == "value"][0]
        df = df.pivot_table(index=tcol, columns=namecol, values=vcol, aggfunc="last").reset_index()
    df["t"] = _to_epoch(df[tcol])
    return df


def load_all(paths):
    df = pd.concat([load_file(p) for p in paths], ignore_index=True, sort=False)
    df = df.dropna(subset=["t"]).sort_values("t").drop_duplicates(subset=["t"], keep="last")
    need = ["bed_jerk_mg", "bed_move_count", "bed_gyro_cdps", "bed_tof_valid", "bed_tof_n", "bed_tof_mm", "bed_tof_std_mm"]
    missing = [c for c in need if c not in df.columns]
    if missing:
        raise SystemExit(f"export is missing fields {missing}; check the NETPIE schema / export columns")
    df = df.dropna(subset=need).reset_index(drop=True)
    for c in need + ["lightIntensity", "wake_rel_min"]:
        df[c] = pd.to_numeric(df.get(c), errors="coerce")
    return df


# ---------------------------------------------------------------- features + labels
def add_features(df):
    st = FeatureState()
    feats = []
    for r in df.itertuples(index=False):
        feats.append(st.step(r.t, r.bed_jerk_mg, r.bed_move_count, r.bed_gyro_cdps, r.bed_tof_valid, r.bed_tof_n,
                             r.bed_tof_mm, r.bed_tof_std_mm,
                             float("nan") if pd.isna(getattr(r, "lightIntensity", float("nan"))) else r.lightIntensity,
                             float("nan") if pd.isna(getattr(r, "wake_rel_min", float("nan"))) else r.wake_rel_min))
    return np.array(feats, dtype=np.float32)


def labels_from_records(df):
    y = np.full(len(df), -1)
    if "label" not in df.columns:
        return y
    lab = df["label"].astype(str).str.upper().map({c: i for i, c in enumerate(CLASSES)}).fillna(-1).astype(int).values
    y[:] = lab
    t = df["t"].values
    # an AWAKE stretch that ends in a missed probe: the last probe interval is uncertain
    for i in range(1, len(df)):
        if lab[i - 1] == 1 and lab[i] == 0:
            y[(t > t[i] - PROBE_EXCLUDE_S) & (t <= t[i]) & (lab == 1)] = -1
    return y


def labels_from_intervals(df, path):
    y = np.full(len(df), -1)
    iv = pd.read_csv(path)
    for r in iv.itertuples(index=False):
        s = pd.Timestamp(r.start).tz_localize(TZ).timestamp()
        e = pd.Timestamp(r.end).tz_localize(TZ).timestamp()
        c = CLASSES.index(str(r.label).strip().upper())
        y[(df["t"].values >= s) & (df["t"].values < e)] = c
    return y


def drop_boundaries(t, y):
    y = y.copy()
    change = np.where((y[1:] != y[:-1]) & (y[1:] >= 0) & (y[:-1] >= 0))[0]
    for i in change:
        y[np.abs(t - t[i + 1]) < BOUNDARY_EXCLUDE_S] = -1
    return y


def night_id(t):
    return [(datetime.fromtimestamp(x, TZ) - timedelta(hours=12)).strftime("%Y-%m-%d") for x in t]


# ---------------------------------------------------------------- export
def export_header(rf, path, info):
    feat, thr, left, right, prob, roots = [], [], [], [], [], []
    for est in rf.estimators_:
        tr = est.tree_
        base = len(feat)
        roots.append(base)
        cls_idx = [int(c) for c in rf.classes_]
        for n in range(tr.node_count):
            leaf = tr.children_left[n] == -1
            feat.append(-1 if leaf else int(tr.feature[n]))
            thr.append(0.0 if leaf else float(tr.threshold[n]))
            left.append(-1 if leaf else base + int(tr.children_left[n]))
            right.append(-1 if leaf else base + int(tr.children_right[n]))
            v = tr.value[n][0].astype(float)
            v = v / v.sum() if v.sum() > 0 else v
            full = [0.0, 0.0, 0.0]
            for k, c in enumerate(cls_idx):
                full[c] = v[k]
            prob.append(full)
    if len(feat) > 32000:
        raise SystemExit("model too big for int16 indices; lower n_estimators / max_depth")

    def arr(xs, fmt):
        return ", ".join(fmt(x) for x in xs)

    with open(path, "w") as f:
        f.write("/* ai_model.h — GENERATED by tools/ai/train.py. Do not edit by hand.\n")
        f.write(f" * {info}\n")
        f.write(f" * features: {', '.join(FEATURE_NAMES)}\n */\n")
        f.write("#pragma once\n#include <stdint.h>\n\n#define AI_MODEL_TRAINED 1\n")
        f.write(f"#define AI_N_TREES {len(roots)}\n#define AI_N_NODES {len(feat)}\n")
        f.write(f"static const int16_t AI_ROOT[AI_N_TREES] = {{{arr(roots, str)}}};\n")
        f.write(f"static const int8_t  AI_FEAT[AI_N_NODES] = {{{arr(feat, str)}}};\n")
        f.write(f"static const float   AI_THR[AI_N_NODES] = {{{arr(thr, lambda x: repr(float(np.float32(x))) + 'f')}}};\n")
        f.write(f"static const int16_t AI_LEFT[AI_N_NODES] = {{{arr(left, str)}}};\n")
        f.write(f"static const int16_t AI_RIGHT[AI_N_NODES] = {{{arr(right, str)}}};\n")
        f.write("static const float   AI_PROB[AI_N_NODES][3] = {" +
                ", ".join("{" + ", ".join(f"{p:.5f}f" for p in row) + "}" for row in prob) + "};\n")
        f.write(f'static const char    AI_MODEL_INFO[] = "{info}";\n')


def smooth3(pred):
    out = pred.copy()
    for i in range(2, len(pred)):
        if pred[i - 1] == pred[i - 2] and pred[i - 1] != pred[i]:
            out[i] = pred[i - 1]
    return out


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data", nargs="+")
    ap.add_argument("--labels", help="CSV start,end,label from the paper log")
    ap.add_argument("--out", default=os.path.join(HERE, "..", "..", "src", "gateway", "ai_model.h"))
    ap.add_argument("--trees", type=int, default=15)
    ap.add_argument("--depth", type=int, default=6)
    a = ap.parse_args()

    from sklearn.ensemble import RandomForestClassifier
    from sklearn.metrics import classification_report, confusion_matrix
    from sklearn.model_selection import LeaveOneGroupOut

    df = load_all(a.data)
    X = add_features(df)
    t = df["t"].values.astype(float)
    y = labels_from_records(df)
    if a.labels:
        y2 = labels_from_intervals(df, a.labels)
        y = np.where(y >= 0, y, y2)
    y = drop_boundaries(t, y)
    keep = y >= 0
    nights = np.array(night_id(t))
    print(f"{len(df)} windows loaded, {keep.sum()} labelled; per class:",
          {CLASSES[c]: int((y == c).sum()) for c in range(3)}, "nights:", sorted(str(n) for n in set(nights[keep])))
    if keep.sum() < 50:
        raise SystemExit("not enough labelled windows yet")

    Xk, yk, gk = X[keep], y[keep], nights[keep]
    mk = lambda: RandomForestClassifier(n_estimators=a.trees, max_depth=a.depth, min_samples_leaf=5,
                                        class_weight="balanced", random_state=0)

    metrics = {}
    if len(set(gk)) >= 2:
        pred = np.full(len(yk), -1)
        for tr, te in LeaveOneGroupOut().split(Xk, yk, gk):
            m = mk().fit(Xk[tr], yk[tr])
            pred[te] = m.predict(Xk[te])
        sm = np.concatenate([smooth3(pred[gk == g]) for g in sorted(set(gk), key=list(gk).index)])
        ys = np.concatenate([yk[gk == g] for g in sorted(set(gk), key=list(gk).index)])
        labels = list(range(3))
        print("\nLeave-one-night-out (nights never seen in training):")
        print(classification_report(yk, pred, labels=labels, target_names=CLASSES, zero_division=0))
        print("confusion (rows = true, cols = predicted):\n", confusion_matrix(yk, pred, labels=labels))
        metrics = {"accuracy_lono": float((pred == yk).mean()), "accuracy_lono_smoothed": float((sm == ys).mean()),
                   "per_night": {str(g): float((pred[gk == g] == yk[gk == g]).mean()) for g in sorted(set(gk))}}
        print(f"accuracy {metrics['accuracy_lono']:.3f}, after 3-window smoothing {metrics['accuracy_lono_smoothed']:.3f}")
        print("per night:", {k: round(v, 3) for k, v in metrics["per_night"].items()})
    else:
        print("\nonly one night labelled: no honest test possible yet (need >= 2 nights)")

    model = mk().fit(Xk, yk)
    imp = sorted(zip(FEATURE_NAMES, model.feature_importances_), key=lambda x: -x[1])
    print("\nfeature importance:", ", ".join(f"{n} {v:.2f}" for n, v in imp))
    info = (f"random forest {a.trees} trees depth {a.depth}, {int(keep.sum())} windows, {len(set(gk))} nights, "
            f"trained {datetime.now(TZ).strftime('%Y-%m-%d %H:%M')}"
            + (f", LONO acc {metrics['accuracy_lono']:.3f}" if metrics else ""))
    export_header(model, a.out, info)
    with open(os.path.join(HERE, "metrics.json"), "w") as f:
        json.dump({"info": info, **metrics, "importance": dict(imp)}, f, indent=2)
    print(f"\nwrote {os.path.normpath(a.out)} — rebuild and upload the gateway")


if __name__ == "__main__":
    main()
