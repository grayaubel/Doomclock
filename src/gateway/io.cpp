// io.cpp — knob + button via interrupts (E2), buzzer patterns and the latch servo.
//
// Interrupt-driven input with deferred processing: the ISRs only read the pins and push a
// small event into gInputQ; TaskApp does debouncing, long-press timing and the actions.
// Buzzer and servo use LEDC directly (no extra library): servo on channel 4 (timer 2, 50 Hz),
// buzzer on channel 6 (timer 3, variable frequency) so they never fight over a timer.
#include "shared.h"
#include "soc/gpio_struct.h"

QueueHandle_t gInputQ;

// ---------------- ISRs ----------------
static inline int IRAM_ATTR fastRead(int pin) {
  return pin < 32 ? (GPIO.in >> pin) & 1 : (GPIO.in1.data >> (pin - 32)) & 1;
}

static const int8_t QDEC[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
static volatile uint8_t qPrev = 0;
static volatile int8_t qAcc = 0;

static void IRAM_ATTR isrKnob() {
  uint8_t s = (fastRead(PIN_ENC_CLK) << 1) | fastRead(PIN_ENC_DT);
  qAcc += QDEC[(qPrev << 2) | s];
  qPrev = s;
  if (qAcc >= ENC_STEPS_PER_DETENT || qAcc <= -ENC_STEPS_PER_DETENT) {
    InputEv e = {IN_KNOB, (int8_t)((qAcc > 0 ? 1 : -1) * ENC_DIR), (uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS)};
    qAcc = 0;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(gInputQ, &e, &woken);
    if (woken) portYIELD_FROM_ISR();
  }
}

static void IRAM_ATTR isrKnobBtn() {
  InputEv e = {(uint8_t)(fastRead(PIN_ENC_SW) ? IN_KNOB_UP : IN_KNOB_DOWN), 0,
               (uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS)};
  BaseType_t woken = pdFALSE;
  xQueueSendFromISR(gInputQ, &e, &woken);
  if (woken) portYIELD_FROM_ISR();
}

static void IRAM_ATTR isrButton() {
  bool pressed = fastRead(PIN_BUTTON) == (BUTTON_ACTIVE_LOW ? 0 : 1);
  InputEv e = {(uint8_t)(pressed ? IN_BTN_DOWN : IN_BTN_UP), 0,
               (uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS)};
  BaseType_t woken = pdFALSE;
  xQueueSendFromISR(gInputQ, &e, &woken);
  if (woken) portYIELD_FROM_ISR();
}

// ---------------- buzzer ----------------
#define BZ_CH 6
struct Step { uint16_t hz, ms; };
static const Step P_ALARM[] = {{2200, 150}, {0, 90}, {2200, 150}, {0, 90}, {2600, 220}, {0, 600}};
static const Step P_NAG[]   = {{3200, 60}, {0, 70}, {3200, 60}, {0, 70}, {3200, 60}, {0, 2700}};
static const Step P_PROBE[] = {{1200, 90}, {0, 120}, {1600, 90}};
static const Step P_CLICK[] = {{2600, 12}};
static const Step P_OK[]    = {{1800, 60}, {0, 40}, {2400, 90}};
static const Step P_ERR[]   = {{600, 250}, {0, 80}, {600, 250}};
static const Step* bzSteps = nullptr;
static uint8_t bzLen = 0, bzIdx = 0;
static bool bzLoop = false;
static uint32_t bzUntil = 0;

static void bzApply() {
  ledcWriteTone(BZ_CH, bzSteps[bzIdx].hz);
  bzUntil = millis() + bzSteps[bzIdx].ms;
}

void buzzer(BuzzPattern p) {
  bzLoop = (p == BZ_ALARM || p == BZ_NAG);
  switch (p) {
    case BZ_ALARM: bzSteps = P_ALARM; bzLen = 6; break;
    case BZ_NAG:   bzSteps = P_NAG;   bzLen = 6; break;
    case BZ_PROBE: bzSteps = P_PROBE; bzLen = 3; break;
    case BZ_CLICK: bzSteps = P_CLICK; bzLen = 1; break;
    case BZ_OK:    bzSteps = P_OK;    bzLen = 3; break;
    case BZ_ERR:   bzSteps = P_ERR;   bzLen = 3; break;
    default:       bzSteps = nullptr; bzLen = 0; ledcWriteTone(BZ_CH, 0); return;
  }
  bzIdx = 0;
  bzApply();
}

void buzzerTick() {
  if (!bzSteps || (int32_t)(millis() - bzUntil) < 0) return;
  if (++bzIdx >= bzLen) {
    if (!bzLoop) { bzSteps = nullptr; ledcWriteTone(BZ_CH, 0); return; }
    bzIdx = 0;
  }
  bzApply();
}

// ---------------- servo ----------------
#define SV_CH 4
enum SvPhase : uint8_t { SV_IDLE, SV_PULL, SV_RETURN };
static SvPhase svPhase = SV_IDLE;
static uint32_t svUntil = 0;

static void servoUs(uint32_t us) { ledcWrite(SV_CH, us == 0 ? 0 : (us * 65535UL) / 20000UL); }

void servoRelease() {
  servoUs(SERVO_PULL_US);
  svPhase = SV_PULL;
  svUntil = millis() + SERVO_PULL_MS;
}

void servoTick() {
  if (svPhase == SV_IDLE || (int32_t)(millis() - svUntil) < 0) return;
  if (svPhase == SV_PULL) { servoUs(SERVO_REST_US); svPhase = SV_RETURN; svUntil = millis() + SERVO_RETURN_MS; }
  else { servoUs(0); svPhase = SV_IDLE; }                 // stop pulses: no torque held (timeout, rule 4.3)
}

bool buzzerIdle() { return bzSteps == nullptr; }

bool servoBusy() { return svPhase != SV_IDLE; }

// ---------------- setup ----------------
void ioBegin() {
  gInputQ = xQueueCreate(32, sizeof(InputEv));
  pinMode(PIN_ENC_CLK, INPUT_PULLUP);
  pinMode(PIN_ENC_DT, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  qPrev = (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), isrKnob, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT), isrKnob, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_SW), isrKnobBtn, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), isrButton, CHANGE);

  ledcSetup(BZ_CH, 2000, 10);
  ledcAttachPin(PIN_BUZZER, BZ_CH);
  ledcWriteTone(BZ_CH, 0);

  ledcSetup(SV_CH, 50, 16);
  ledcAttachPin(PIN_SERVO, SV_CH);
  servoUs(0);                                            // no pulses at boot: the servo stays limp
}
