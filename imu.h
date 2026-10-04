#pragma once

#include <Arduino.h>

// QMI8658 (6 ejes) en el mismo bus I2C que el tactil/RTC/PMU. Sin pin de
// interrupcion en este pinout: solo se sondea (poll). Deteccion de pasos por
// software a partir de la magnitud de la aceleracion (el pedometro interno
// del chip no es fiable en todas las placas).
bool imuBegin();                              // detecta el chip (no lo activa aun)
bool imuOk();                                 // true si se encontro
void imuEnable();                             // activa el acelerometro (inicio de un Trip)
void imuDisable();                            // lo apaga (fin del Trip, ahorra energia)
void imuPoll(uint32_t nowMs, uint16_t intervalMs);  // llamar seguido desde loop()
uint16_t imuTakeSteps();                      // pasos nuevos desde la ultima llamada (los consume)
