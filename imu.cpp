#include "imu.h"
#include "pin_config.h"

#include <math.h>
#include <Wire.h>
#include <SensorQMI8658.hpp>

// Comparacion segura de deadlines con millis() (funciona tambien al dar la
// vuelta el contador tras ~49 dias).
static inline bool deadlineActive(uint32_t now, uint32_t deadline) {
  return deadline != 0 && (int32_t)(now - deadline) < 0;
}

static SensorQMI8658 qmi;
static bool ok = false;
static bool enabled = false;
static uint32_t lastPoll = 0;
static uint32_t bootIgnoreUntil = 0;
static uint16_t stepBank = 0;
static float walkGravity = 0;
static bool walkPeak = false;
static uint32_t walkLastPeak = 0;
static uint8_t walkCadence = 0;

bool imuOk() { return ok; }

static void resetWalkState() {
  stepBank = 0;
  walkGravity = 0;
  walkPeak = false;
  walkLastPeak = 0;
  walkCadence = 0;
  lastPoll = 0;
  bootIgnoreUntil = millis() + 400;
}

static void bankSteps(uint16_t count) {
  uint32_t next = (uint32_t)stepBank + count;
  stepBank = next > 60000UL ? 60000 : (uint16_t)next;
}

// Deteccion de pasos a partir de |aceleracion|: sigue una linea base movil
// (walkGravity) y busca picos por encima de ella con ritmo de paso, para no
// contar golpes/vibraciones sueltas como pasos.
static void detectSoftwareStep(uint32_t nowMs, float mag) {
  if (walkGravity <= 0.01f) {
    walkGravity = mag;
    return;
  }

  float signal = mag - walkGravity;
  float motion = fabsf(signal);
  // En reposo la linea base sigue rapido la desviacion del sensor. En
  // movimiento se mantiene lenta, para que el impulso del paso no se filtre.
  float alpha = motion < 0.08f ? 0.12f : 0.025f;
  walkGravity += (mag - walkGravity) * alpha;
  if (deadlineActive(nowMs, bootIgnoreUntil)) return;

  // Tras cada impulso, el movimiento debe calmarse antes de aceptar el
  // siguiente. Evita contar el golpe y el rebote del mismo paso por separado.
  if (walkPeak) {
    if (signal <= 0.025f) walkPeak = false;
    return;
  }
  // Solo cuenta el impulso positivo (el golpe). El siguiente vaiven negativo
  // es parte del mismo paso y no debe generar un segundo impulso.
  if (signal < 0.085f) return;
  walkPeak = true;

  uint32_t gap = walkLastPeak ? nowMs - walkLastPeak : 0;
  if (walkLastPeak && gap < 280UL) return;
  if (!walkLastPeak || gap > 1400UL) {
    walkLastPeak = nowMs;
    walkCadence = 1;
    return;
  }

  walkLastPeak = nowMs;
  if (walkCadence == 1) {
    // Dos impulsos con ritmo confirman que es caminar; entonces se liberan
    // juntos los dos primeros pasos. Golpes sueltos no cuentan nada.
    bankSteps(2);
    walkCadence = 2;
  } else {
    bankSteps(1);
    if (walkCadence < 255) walkCadence++;
  }
}

bool imuBegin() {
  ok = false;
  enabled = false;
  resetWalkState();

  if (!qmi.begin(Wire, QMI8658_I2C_ADDR, IIC_SDA, IIC_SCL)) {
    Serial.println("QMI8658 no detectado");
    return false;
  }
  // 2G / 62.5Hz: rango probado en hardware real para la deteccion de pasos.
  qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_2G,
                          SensorQMI8658::ACC_ODR_62_5Hz,
                          SensorQMI8658::LPF_MODE_0);
  // No se activa el acelerometro aqui: se enciende solo mientras dura un
  // Trip (imuEnable), para no gastar energia el resto del tiempo.
  ok = true;
  Serial.printf("QMI8658 ok id=0x%X\n", qmi.getChipID());
  return true;
}

void imuEnable() {
  if (!ok || enabled) return;
  qmi.enableAccelerometer();
  delay(40);
  resetWalkState();
  enabled = true;
}

void imuDisable() {
  if (!ok || !enabled) return;
  qmi.disableAccelerometer();
  enabled = false;
}

void imuPoll(uint32_t nowMs, uint16_t intervalMs) {
  if (!ok || !enabled) return;
  if (intervalMs < 25) intervalMs = 25;
  if (lastPoll && (uint32_t)(nowMs - lastPoll) < intervalMs) return;
  lastPoll = nowMs ? nowMs : 1;

  float x = 0, y = 0, z = 0;
  if (qmi.getAccelerometer(x, y, z)) {
    float mag = sqrtf(x * x + y * y + z * z);
    detectSoftwareStep(nowMs, mag);
  }
}

uint16_t imuTakeSteps() {
  uint16_t n = stepBank;
  stepBank = 0;
  return n;
}
