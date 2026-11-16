/* ai_features.h — feature extraction for the sleep/wake classifier.
 *
 * Plain C on purpose: the SAME file is compiled into the firmware and into a host library that
 * tools/ai/test_parity.py loads, so training (Python) and inference (ESP32) provably compute
 * identical features. If you change anything here, change tools/ai/features.py the same way
 * and run the parity test.
 *
 * One call per 10 s bed window, in time order. Features:
 *   0 jerk_mg        movement intensity in this window
 *   1 move_count     samples that moved in this window
 *   2 gyro_dps       mean rotation speed
 *   3 moves_1min     move_count summed over the last 60 s
 *   4 moves_5min     ... over the last 5 min
 *   5 min_since_move minutes since a window with >= 3 moving samples (capped at 60)
 *   6 tof_valid_frac fraction of ToF readings in range
 *   7 tof_mm         mean distance (0 if none in range)
 *   8 tof_std_mm     distance wobble (breathing / fidgeting / sitting up)
 *   9 lux_log        log10(1 + room light)
 *  10 wake_rel_min   minutes since tonight's wake time, clamped to [-180, 180]
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#define AIF_N 11
#define AIF_HIST 40
#define AIF_GAP_RESET_S 120.0
#define AIF_ACTIVE_MOVES 3.0f

typedef struct { double t; float moves; } AifHist;

typedef struct {
  AifHist h[AIF_HIST];
  int n, head;
  double lastT, lastActiveT;
  int have;
} AifState;

typedef struct {
  double t;             /* window end, unix seconds */
  float jerk_mg, move_count, gyro_cdps;
  float tof_valid, tof_n, tof_mm, tof_std_mm;
  float lux;            /* NaN if unknown */
  float wake_rel_min;   /* NaN if unknown */
} AifInput;

void aif_reset(AifState* s);
void aif_step(AifState* s, const AifInput* in, float out[AIF_N]);

#ifdef __cplusplus
}
#endif
