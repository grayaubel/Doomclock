// ai.cpp — on-device sleep/wake classifier (E7): features -> random forest -> smoothed class.
// Runs on the gateway for every bed window. Classes: 0 ASLEEP, 1 AWAKE_IN_BED, 2 UP.
#include "shared.h"
#include "ai_features.h"
#include "ai_model.h"

AiState gAi;
static AifState feat;
static WakeClass hist[3] = {WC_UNKNOWN, WC_UNKNOWN, WC_UNKNOWN};
static uint8_t histN = 0;

static void forestPredict(const float* f, float prob[3]) {
  prob[0] = prob[1] = prob[2] = 0;
  for (int t = 0; t < AI_N_TREES; t++) {
    int n = AI_ROOT[t];
    while (AI_FEAT[n] >= 0) n = (f[AI_FEAT[n]] <= AI_THR[n]) ? AI_LEFT[n] : AI_RIGHT[n];   // sklearn: left if <=
    for (int c = 0; c < 3; c++) prob[c] += AI_PROB[n][c];
  }
  for (int c = 0; c < 3; c++) prob[c] /= AI_N_TREES;
}

// Placeholder used only while ai_model.h is untrained. A fixed rule, NOT the AI component:
// it exists so the clock is usable while the group collects and labels data.
static void placeholderPredict(const float* f, float prob[3]) {
  prob[0] = prob[1] = prob[2] = 0;
  if (f[6] < 0.3f) prob[WC_UP] = 1;                              // body not in front of the ToF
  else if (f[5] >= 5.0f && f[9] < 1.0f) prob[WC_ASLEEP] = 1;      // dark and still for 5 min
  else prob[WC_AWAKE_IN_BED] = 1;
}

void aiPushWindow(const BedPacket& p, time_t windowEnd, float lux, float wakeRelMin) {
  AifInput in = {(double)windowEnd, (float)p.jerk_mean_mg, (float)p.move_count, (float)p.gyro_mean_cdps,
                 (float)p.tof_valid, (float)p.tof_n, (float)p.tof_mean_mm, (float)p.tof_std_mm, lux, wakeRelMin};
  float f[AIF_N], prob[3];
  aif_step(&feat, &in, f);
  if (AI_MODEL_TRAINED) forestPredict(f, prob); else placeholderPredict(f, prob);

  int best = 0;
  for (int c = 1; c < 3; c++) if (prob[c] > prob[best]) best = c;
  WakeClass raw = (WakeClass)best;

  // smoothing: majority of the last 3 windows (30 s), ties go to the newest
  hist[2] = hist[1]; hist[1] = hist[0]; hist[0] = raw;
  if (histN < 3) histN++;
  WakeClass dec = raw;
  if (histN == 3) {
    if (hist[1] == hist[2] && hist[1] != hist[0]) dec = hist[1];
  }

  lockState();
  gAi.raw = raw;
  gAi.cls = dec;
  gAi.conf = prob[best];
  gAi.trained = AI_MODEL_TRAINED;
  gAi.atMs = millis();
  unlockState();
}

void aiMarkStale() {
  lockState();
  gAi.cls = gAi.raw = WC_UNKNOWN;
  gAi.conf = 0;
  unlockState();
  histN = 0;
}
