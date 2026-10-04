#pragma once
#include <stdint.h>

// Efectos de sonido del juego (cola, no bloqueante). El orden coincide con la
// tabla SFX de audio.cpp.
enum Sfx : uint8_t {
  SFX_EAT = 0,  // comer
  SFX_PLAY,     // punto del minijuego / golpe
  SFX_HEART,    // le gusta / mimo
  SFX_HATCH,    // eclosion
  SFX_EVOLVE,   // evolucion
  SFX_MEDAL,    // medalla / hito
  SFX_DENY,     // accion no permitida
  SFX_BYE,      // despedida
  SFX_LEVEL,    // sube de nivel
  SFX_HIT_STRONG,  // combate: golpe fuerte
  SFX_HIT_WEAK,    // combate: golpe flojo
  SFX_MISS,        // combate: golpe fallado
  SFX_BATTLE_WIN,  // combate: victoria
  SFX_BATTLE_LOSE, // combate: derrota
  SFX_CLEAN,       // baño/limpieza
  SFX_TRAIN,       // golpe en el saco de entrenamiento
  SFX_COUNT
};

void audioBegin();          // init ES8311 + I2S + amplificador + tarea de audio
void sfxPlay(uint8_t id);   // encola un efecto (no bloquea el loop)
void sfxFlush();            // vacia la cola de golpe (descarta lo pendiente)
void audioSetEnabled(bool on);
bool audioEnabled();
