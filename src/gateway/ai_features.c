/* ai_features.c — see ai_features.h. Mirrored by tools/ai/features.py (parity-tested). */
#include "ai_features.h"
#include <math.h>
#include <string.h>

void aif_reset(AifState* s) { memset(s, 0, sizeof(*s)); }

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void aif_step(AifState* s, const AifInput* in, float out[AIF_N]) {
  /* a gap (node off / out of range) or time going backwards starts a fresh history */
  if (!s->have || in->t - s->lastT > AIF_GAP_RESET_S || in->t < s->lastT) {
    aif_reset(s);
    s->have = 1;
    s->lastActiveT = in->t;
  }
  s->h[s->head].t = in->t;
  s->h[s->head].moves = in->move_count;
  s->head = (s->head + 1) % AIF_HIST;
  if (s->n < AIF_HIST) s->n++;
  if (in->move_count >= AIF_ACTIVE_MOVES) s->lastActiveT = in->t;
  s->lastT = in->t;

  float m1 = 0, m5 = 0;
  for (int i = 0; i < s->n; i++) {
    const AifHist* e = &s->h[i];
    if (e->t > in->t - 60.0) m1 += e->moves;
    if (e->t > in->t - 300.0) m5 += e->moves;
  }

  out[0] = in->jerk_mg;
  out[1] = in->move_count;
  out[2] = in->gyro_cdps / 100.0f;
  out[3] = m1;
  out[4] = m5;
  out[5] = clampf((float)((in->t - s->lastActiveT) / 60.0), 0.0f, 60.0f);
  out[6] = in->tof_n > 0 ? in->tof_valid / in->tof_n : 0.0f;
  out[7] = in->tof_valid > 0 ? in->tof_mm : 0.0f;
  out[8] = in->tof_std_mm;
  out[9] = (isnan(in->lux) || in->lux < 0) ? 0.0f : log10f(1.0f + in->lux);
  out[10] = isnan(in->wake_rel_min) ? 0.0f : clampf(in->wake_rel_min, -180.0f, 180.0f);
}
