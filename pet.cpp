#include "dex.h"
#include "pet.h"
#include "audio.h"

void Pet::begin() {
  prefs.begin("tamapoke", false);
  if (!prefs.getBool("init", false)) {
    prefs.putBool("init", true);
    newEgg();
  } else {
    load();
  }
  lastTick = millis();
}

// Nivel 1->2: 60 min (MINUTES_PER_LEVEL). Cada salto siguiente dura un 2%
// mas que el anterior, redondeado hacia arriba (60, 62, 64, 66, 68...).
// Antes esto era "1 + ageMinutes/60" devuelto directo como uint8_t: al
// llegar a nivel 256 desbordaba y volvia a 0 (~10.6 dias de edad). Con la
// nueva curva el nivel tardaria milenios en acercarse a eso, pero el bucle
// se para en 255 igualmente por seguridad.
uint8_t Pet::level() const {
  uint32_t need = 0, dur = MINUTES_PER_LEVEL;
  uint16_t lv = 1;
  while (lv < 255) {
    need += dur;
    if (ageMinutes < need) break;
    lv++;
    dur = (dur * 102 + 99) / 100;  // *1.02 redondeado hacia arriba
  }
  return (uint8_t)lv;
}

void Pet::levelProgress(uint32_t &into, uint32_t &total) const {
  uint32_t need = 0, dur = MINUTES_PER_LEVEL;
  uint16_t lv = 1;
  while (lv < 255) {
    need += dur;
    if (ageMinutes < need) break;
    lv++;
    dur = (dur * 102 + 99) / 100;
  }
  total = dur;
  into = ageMinutes - (need - dur);
}

uint32_t Pet::minutesForLevel(uint16_t targetLv) {
  uint32_t need = 0, dur = MINUTES_PER_LEVEL;
  uint16_t lv = 1;
  while (lv < targetLv && lv < 255) {
    need += dur;
    lv++;
    dur = (dur * 102 + 99) / 100;
  }
  return need;
}

void Pet::newEgg() {
  ceremony = CER_NONE;
  neglectTicks = 0;
  weight = 0;
  speciesId = -1;
  prevSpeciesId = -1;
  eggTarget = pickEggSpecies();  // especie oculta segun rareza y pokedex
  starterPick = (registeredCount() == 0);  // primera partida: el jugador elige inicial
  // sorteo shiny: 1/48 base, mejor con despedida y con racha/vinculo altos
  int shinyBase = (lastEnd == CER_FAREWELL ? 24 : 48) - careBonus();
  if (shinyBase < 8) shinyBase = 8;
  eggShiny = (random(shinyBase) == 0);
  eggTaps = 0;
  fullness = 80;
  joy = 80;
  energy = 80;
  hygiene = 100;
  poops = 0;
  ageMinutes = 0;
  farDeclinedAge = 0;  // Zusammenbleiben-Sperre gehoert zum alten Individuum
  careMistakes = 0;
  mistakeCooldown = 0;
  sleeping = false;
  save();
}

void Pet::setClock(uint32_t nowEpoch) {
  lastSeenEpoch = nowEpoch;
  reconcileSleepState();  // la hora pudo saltar de golpe (ajuste manual): corrige,
                            // y ya dispara checkPerfectDayCycle() internamente
  if (nowEpoch) save();  // persiste ya: un corte de luz no pierde la referencia
}

// llamar SOLO desde el ajuste manual del reloj en los ajustes (applyClock en
// el .ino), nunca desde setClock() en si: setClock() tambien la usan
// comandos de depuracion por serie (incl. CAREDAY, que simula un salto de
// dia A PROPOSITO para probar la racha) y no deben quedar bloqueados por
// esto. Sin simulacion del tiempo intermedio (a diferencia de syncClock,
// el reencuentro tras estar apagado), adelantar el reloj a mano podria
// "fabricar" un dia limpio, una racha, o 12h de peso en verde de la nada.
// Resincroniza los contadores basados en cambio de dia al dia actual, sin
// conceder NI denegar nada por el salto en si
void Pet::guardClockJump(uint32_t deltaMinAbs, uint32_t oldEpoch) {
  if (!lastSeenEpoch) return;
  // "Toller Tag" wird absichtlich NICHT mehr hier beruehrt: der neue
  // Wach-zu-Wach-Mechanismus haengt an echten Schlaf/Wach-Uebergaengen,
  // nicht an verstrichener Zeit -- eine reine Zeitkorrektur soll weder
  // den laufenden Zyklus noch das Durchstreichen veraendern
  //
  // Top Form / Endform: nur bei einer Korrektur UEBER 2 Stunden (120 Min)
  // wird die laufende Serie zurueckgesetzt -- das deckt kleine Korrekturen
  // und eine manuelle Sommer-/Winterzeit-Umstellung (1h) vollstaendig ab,
  // schuetzt aber weiterhin vor einem groben Vorwaerts-Sprung, der eine
  // Medaille kuenstlich "erschleichen" wuerde
  if (deltaMinAbs > 120) {
    if (finalFormSinceEpoch) finalFormSinceEpoch = lastSeenEpoch;  // idem para "Endform"
    if (weightGreenSinceEpoch) weightGreenSinceEpoch = lastSeenEpoch;  // la racha de peso se reinicia desde ahora
  }
  // Die vier kurzen Cooldowns (5-60 Min) werden bei jeder manuellen
  // Korrektur exakt umgerechnet: die zum Zeitpunkt der Korrektur noch
  // verbleibende Dauer (altes Ziel minus alte Uhrzeit) bleibt erhalten,
  // nur eben ab der neuen Uhrzeit weitergezaehlt -- statt sie komplett
  // zu loeschen. War ein Cooldown zum Korrekturzeitpunkt schon abgelaufen
  // (oldEpoch >= Ziel), bleibt er unangetastet (ist ja ohnehin inaktiv)
  if (oldEpoch) {
    if (berryCooldownUntilEpoch > oldEpoch) berryCooldownUntilEpoch = lastSeenEpoch + (berryCooldownUntilEpoch - oldEpoch);
    if (bathCooldownUntilEpoch > oldEpoch) bathCooldownUntilEpoch = lastSeenEpoch + (bathCooldownUntilEpoch - oldEpoch);
    if (caressCooldownUntilEpoch > oldEpoch) caressCooldownUntilEpoch = lastSeenEpoch + (caressCooldownUntilEpoch - oldEpoch);
    if (napCooldownUntilEpoch > oldEpoch) napCooldownUntilEpoch = lastSeenEpoch + (napCooldownUntilEpoch - oldEpoch);
  }
  lastCareDay = today();  // el salto no cuenta como dia de racha ganado ni roto
  save();
}

// corrige wach/durmiendo segun la hora actual, para cuando el salto exacto de
// las 22h/8h no se llego a cruzar en un tick (reloj ajustado a mano, o el
// aparato no estaba encendido justo en ese minuto). A diferencia de tick(),
// usa >=/< en vez de == : no importa cuanto haya saltado la hora, siempre
// deja un estado correcto. En las ventanas libres (6-8h y 18-22h) no toca
// nada: ahi cualquiera de los dos estados es legitimo (eleccion del jugador)
void Pet::reconcileSleepState() {
  if (isEgg() || ceremony != CER_NONE || starterPick) return;
  uint8_t h = hourNow();
  if (h >= 8 && h < 18) {         // ventana de nap: nunca deberia estar dormido
    if (sleeping) { sleeping = false; lightsOut = false; }
  } else if (h >= 22 || h < 6) {  // ventana de sueno forzado
    if (!sleeping) sleeping = true;
  }
  // centralizado aqui (no en cada llamador): cualquier cambio de sleeping
  // que pase POR ESTA FUNCION dispara la deteccion de despertar. Asi no
  // hace falta acordarse de anadirlo en cada sitio nuevo que la use
  checkPerfectDayCycle(lastSeenEpoch);
}

void Pet::syncClock(uint32_t nowEpoch) {
  uint32_t seen = prefs.getUInt("seen", 0);
  lastSeenEpoch = nowEpoch;
  advanceWeekday();
  if (nowEpoch == 0) return;
  uint32_t mins = (seen && nowEpoch > seen) ? (nowEpoch - seen) / 60 : 0;
  if (mins < 2 || ceremony != CER_NONE || starterPick) {
    reconcileSleepState();  // por si el aparato justo se apago/encendio en el limite
    save();  // primera vez, sin tiempo que aplicar o aun eligiendo inicial
    return;
  }
  if (mins > 14UL * 24 * 60) mins = 14UL * 24 * 60;  // tope: 2 semanas

  for (uint32_t i = 0; i < mins; i++) {
    ageMinutes++;
    if (isEgg()) continue;  // ya no eclosiona sola: hace falta tocarla (eggTap)
    // napUntil nunca esta activo aqui: el nap (10s) no persiste, se pierde
    // sin mas si el aparato se apaga a mitad (igual que gameOpen/sackOpen)
    uint8_t h = hourAt(seen + (uint32_t)(i + 1) * 60);  // hora de este minuto simulado
    if ((h >= 22 || h < 6) && !sleeping) sleeping = true;
    if (h >= 22 || h < 6) {
      // ninguno de los dos modos de ausflug sobrevive al sueno forzado: se
      // abandonan con la misma penalizacion que "niemanden getroffen"
      if (tripSoloEndEpoch) {
        tripSoloEndEpoch = 0;
        joy = (joy > 30) ? joy - 30 : 0;
        if (bond > 5) bond -= 5; else bond = 0;
      }
      if (tripActive) {
        tripActive = false;
        tripActiveFlag = false;
        joy = (joy > 30) ? joy - 30 : 0;
        if (bond > 5) bond -= 5; else bond = 0;
      }
    }
    if (h >= 8 && h < 18 && sleeping) { sleeping = false; lightsOut = false; }

    if (sleeping) {  // descanso: baja lento, igual que en vivo (sin suelo)
      if (ageMinutes % 6 == 0) {
        if (lightsOut) energy = clamp100(energy + 1);
        else {
          energy = clamp100((int)energy - 1);  // sueno forzado sin apagar la luz: penaliza
          joy = clamp100((int)joy - 2);  // Licht nicht aus: zusaetzliche Strafe auch bei Freude
        }
      }
      if (weight > 0 && ageMinutes % 30 == 0) weight--;
      if (ageMinutes % 10 == 0) fullness = clamp100((int)fullness - 1);
      if (ageMinutes % 20 == 0) hygiene = clamp100((int)hygiene - 1 - 10 * poops);
      if (ageMinutes % 15 == 0) joy = clamp100((int)joy - 1);
      continue;
    }
    if (ageMinutes % 5 == 0) {
      int hLoss = (tripActive && stepsForHunger) ? 2 : 1;
      fullness = clamp100((int)fullness - hLoss);
      stepsForHunger = false;
    }
    if (ageMinutes % 15 == 0)
      energy = clamp100((int)energy - (weight >= OVERWEIGHT_FROM ? 2 : 1));
    if (tripActive && ageMinutes % 7 == 0) {
      if (stepsForEnergy) energy = clamp100((int)energy - 2);
      stepsForEnergy = false;
    }
    maybePoop(seen + (uint32_t)(i + 1) * 60);
    if (ageMinutes % 20 == 0) {
      int poopPenalty = tripActive ? 5 : 10;
      hygiene = clamp100((int)hygiene - 1 - poopPenalty * poops);
    }
    if (weight > 0 && ageMinutes % 30 == 0) weight--;
    if (tripActive && weight > 0 && ageMinutes % 3 == 0) {
      if (stepsForWeight) weight = (weight > 2) ? weight - 2 : 0;
      stepsForWeight = false;
    }

    if (lowestStat() >= 40) {
      if (++goodTicks >= 720) {
        goodTicks = 0;
        if (trDef < 100) trDef++;
      }
    } else {
      goodTicks = 0;
    }

    bool otherZero = (fullness == 0 || energy == 0 || hygiene == 0);
    if (tripActive) {
      int joyPeriod = otherZero ? 3 : 5;
      if (ageMinutes % joyPeriod == 0) {
        if (!stepsThisInterval) joy = clamp100((int)joy - 1);
        stepsThisInterval = false;
      }
    } else {
      if (ageMinutes % 4 == 0) joy = clamp100((int)joy - (otherZero ? 2 : 1));
    }

    checkPerfectDayCycle(seen + (uint32_t)(i + 1) * 60);  // dia simulado de ESTE minuto, no el final
    checkFinalForm12h(seen + (uint32_t)(i + 1) * 60);

    // mismo descuido que en vivo: ahora SI puede pasar en tu ausencia
    if (mistakeCooldown > 0) mistakeCooldown--;
    if (lowestStat() <= 10 && mistakeCooldown == 0) {
      careMistakes++;
      mistakeCooldown = 60;
      levelAtLastMistake = level();
      lastMistakeEpoch = seen + (uint32_t)(i + 1) * 60;
      if (bond > 10) bond -= 10; else if (bond > 1) bond = 1;
      // a partir del 6o descuido total, cada descuido nuevo tiene una
      // chance creciente de disparar la escapada (10% en el 6o, +10% por
      // cada uno mas, tope en 50% desde el 10o)
      if (careMistakes >= 6) {
        int runChance = (careMistakes - 5) * 10;
        if (runChance > 50) runChance = 50;
        if ((int)random(100) < runChance) neglectTicks = RUNAWAY_TICKS;
      }
    }
    // peso al tope (100): penalizacion de vinculo, mismo enfriamiento que un descuido
    if (weightCapCooldown > 0) weightCapCooldown--;
    if (weight >= 100 && weightCapCooldown == 0) {
      weightCapCooldown = 60;
      if (bond > 20) bond -= 20; else bond = 0;
    }

    // pestillo de un solo sentido: ver comentario en tick()
    int zeroCount = (fullness == 0) + (joy == 0) + (energy == 0) + (hygiene == 0);
    if (zeroCount >= 3) neglectTicks = RUNAWAY_TICKS;
  }
  checkMedals();
  // la hora real (lastSeenEpoch) puede haber avanzado mas de lo simulado (tope
  // de 2 semanas): asegura que el estado final encaje con la hora de verdad
  reconcileSleepState();
  // la evolucion NO se aplica offline: queda lista y la dispara el usuario
  // tocando al bicho cuando vuelve (para que vea la transformacion)
  Serial.printf("offline: %u min aplicados (nv.%u)\n", mins, level());
  save();
}

void Pet::update(uint32_t nowMs) {
  // fin de ceremonia: la criatura se va y queda un huevo nuevo
  if (ceremony != CER_NONE && millis() > ceremonyUntil) {
    newEgg();
    return;
  }
  // power-nap: se resuelve aqui (cada frame) y no en tick() (cada minuto),
  // para que la cuenta atras de 10s en pantalla sea precisa
  if (napUntil && nowMs >= napUntil) {
    energy = clamp100(energy + 50);
    napUntil = 0;
    // cooldown en hora real (no millis()): asi apagar y encender el aparato
    // no sirve para saltarselo
    napCooldownUntilEpoch = lastSeenEpoch ? lastSeenEpoch + 3600UL : 0;
    checkMedals();
    pendingSave = true;
  }
  while (nowMs - lastTick >= PET_TICK_MS) {
    lastTick += PET_TICK_MS;
    tick();
  }
}

// 3 cacas al dia como maximo, cada una en una de tres franjas de 4h dentro de
// 8-20h (8-12/12-16/16-20). Probabilidad 1/minutos-restantes-en-la-franja: en
// promedio cae en un momento aleatorio, y como muy tarde en el ultimo minuto
// de la franja (garantizado). Vale igual en vivo y en la simulacion offline.
void Pet::maybePoop(uint32_t epoch) {
  if (!epoch) return;
  uint32_t day = epoch / 86400;
  if (day != poopDay) { poopDay = day; poopSlotsDone = 0; }
  uint32_t minuteOfDay = (epoch / 60) % 1440;
  if (minuteOfDay < 480 || minuteOfDay >= 1200) return;  // fuera de 8:00-20:00
  uint8_t slot = (uint8_t)((minuteOfDay - 480) / 180);   // 0,1,2,3 (franjas de 3h)
  if (poopSlotsDone & (1 << slot)) return;
  uint32_t slotEnd = 480 + (uint32_t)(slot + 1) * 180;
  uint32_t remaining = slotEnd - minuteOfDay;  // >=1
  if (poops < 3 && random(remaining) == 0) {
    poops++;
    poopSlotsDone |= (1 << slot);
  }
}

void Pet::tick() {
  if (ceremony != CER_NONE) return;  // el tiempo se detiene en la despedida
  if (starterPick) return;  // la partida no empieza hasta elegir inicial: si el
                            // tiempo corriera aqui, el huevo eclosionaria solo a
                            // los 3 min con la especie sorteada y se perderia la
                            // eleccion del jugador
  uint8_t lvBefore = level();
  ageMinutes++;
  advanceWeekday();

  if (isEgg()) return;  // ya no eclosiona sola: hace falta tocarla (eggTap)

  // power-nap en curso (10s): nada mas se mueve (comida/joy/higiene/energia
  // intactos). Se resuelve en update(), no aqui, para maxima precision.
  // No persiste: un corte de luz durante el nap se pierde sin mas, ya que
  // dura tan poco (como gameOpen/sackOpen)
  if (napUntil) return;

  uint8_t h = hourNow();
  // ciclo dia/noche automatico: dentro de la franja 22h-6h se duerme si no lo
  // esta ya (el usuario puede haber apagado la luz antes); entre 8h y 18h se
  // despierta siempre (ventana de nap, igual que en reconcileSleepState).
  // Tanto 6h-8h como 18h-22h son zona libre: el jugador puede acostarlo
  // pronto (Licht aus) o despertarlo pronto a mano, sin que el automatismo
  // lo deshaga enseguida. Rango en vez de hora exacta: si el bicho nace (u
  // otro evento arranca el tick) ya pasadas las 22h, por ejemplo a las 23h,
  // el disparador no se pierde por no coincidir con la hora exacta
  if ((h >= 22 || h < 6) && !sleeping) sleeping = true;
  if (h >= 22 || h < 6) {
    // ninguno de los dos modos de ausflug sobrevive al sueno forzado: se
    // abandonan con la misma penalizacion que "niemanden getroffen"
    if (tripSoloEndEpoch) {
      tripSoloEndEpoch = 0;
      joy = (joy > 30) ? joy - 30 : 0;
      if (bond > 5) bond -= 5; else bond = 0;
    }
    if (tripActive) {
      tripActive = false;
      tripActiveFlag = false;
      joy = (joy > 30) ? joy - 30 : 0;
      if (bond > 5) bond -= 5; else bond = 0;
    }
  }
  if (h >= 8 && h < 18 && sleeping) { sleeping = false; lightsOut = false; }

  // el sueño es descanso: las necesidades bajan MUCHO mas lento que despierto
  // y con suelo (amanece pidiendo algo de mimo, no a cero, sin descuidos ni
  // escapadas). El peso aun se quema; goodTicks (racha DEF) queda en pausa.
  if (sleeping) {
    // con la luz apagada la energia sube; si se quedo dormido a la fuerza
    // (22h) sin apagar la luz, baja al mismo ritmo como penalizacion
    if (ageMinutes % 6 == 0) {
      if (lightsOut) energy = clamp100(energy + 1);
      else {
        energy = clamp100((int)energy - 1);
        joy = clamp100((int)joy - 2);  // Licht nicht aus: zusaetzliche Strafe auch bei Freude
      }
    }
    if (weight > 0 && ageMinutes % 30 == 0) weight--;
    if (ageMinutes % 10 == 0) fullness = clamp100((int)fullness - 1);
    if (ageMinutes % 20 == 0) hygiene = clamp100((int)hygiene - 1 - 10 * poops);
    if (ageMinutes % 15 == 0) joy = clamp100((int)joy - 1);
    checkMedals();  // aun puede cruzar un nivel por edad mientras duerme
    if (++ticksSinceSave >= 5) pendingSave = true;
    return;
  }

  if (level() > lvBefore) sfxPlay(SFX_LEVEL);  // subio de nivel (despierto)

  // hambre: -1/5min normal; en ausflug -2/5min si hubo pasos en esa ventana
  // (mismo ritmo base que fuera de ausflug, asi que una sola comprobacion basta)
  if (ageMinutes % 5 == 0) {
    int hLoss = (tripActive && stepsForHunger) ? 2 : 1;
    fullness = clamp100((int)fullness - hLoss);
    stepsForHunger = false;
  }
  // energia: ritmo normal de fondo (-1/15min, -2 si hay sobrepeso) siempre
  // activo; en ausflug se suma un extra de -2/7min si hubo pasos en esa
  // ventana (el solape es de como mucho 1-2 puntos por ciclo, aceptable)
  if (ageMinutes % 15 == 0)
    energy = clamp100((int)energy - (weight >= OVERWEIGHT_FROM ? 2 : 1));
  if (tripActive && ageMinutes % 7 == 0) {
    if (stepsForEnergy) energy = clamp100((int)energy - 2);
    stepsForEnergy = false;
  }
  maybePoop(lastSeenEpoch);
  if (ageMinutes % 20 == 0) {
    int poopPenalty = tripActive ? 5 : 10;  // en ausflug, la mitad de grave
    hygiene = clamp100((int)hygiene - 1 - poopPenalty * poops);
  }
  // peso: ritmo normal de fondo (-1/30min) siempre activo; en ausflug se
  // suma un extra de -2/3min si hubo pasos en esa ventana
  if (weight > 0 && ageMinutes % 30 == 0) weight--;
  if (tripActive && weight > 0 && ageMinutes % 3 == 0) {
    if (stepsForWeight) weight = (weight > 2) ? weight - 2 : 0;
    stepsForWeight = false;
  }

  // la disciplina forja la defensa: 12 h seguidas bien cuidado = +1 DEF
  if (lowestStat() >= 40) {
    if (++goodTicks >= 720) {
      goodTicks = 0;
      if (trDef < 100) trDef++;
    }
  } else {
    goodTicks = 0;
  }

  // la alegria: fuera de ausflug, -1/4min normal, -2/4min si alguna otra
  // necesidad esta en cero. En ausflug: -1 cada 5min (3min si hay un cero),
  // pero SOLO si no hubo pasos en ese intervalo
  bool otherZero = (fullness == 0 || energy == 0 || hygiene == 0);
  if (tripActive) {
    int joyPeriod = otherZero ? 3 : 5;
    if (ageMinutes % joyPeriod == 0) {
      if (!stepsThisInterval) joy = clamp100((int)joy - 1);
      stepsThisInterval = false;  // reinicia la ventana de deteccion de pasos
    }
  } else {
    if (ageMinutes % 4 == 0) joy = clamp100((int)joy - (otherZero ? 2 : 1));
  }

  // Descuido: dejar una estadistica por los suelos cuenta como error de
  // cuidado (con enfriamiento para no contar el mismo descuido cada minuto)
  if (mistakeCooldown > 0) mistakeCooldown--;
  if (lowestStat() <= 10 && mistakeCooldown == 0) {
    careMistakes++;
    mistakeCooldown = 60;
    levelAtLastMistake = level();
    lastMistakeEpoch = lastSeenEpoch;
    if (bond > 10) bond -= 10; else if (bond > 1) bond = 1;
    // a partir del 6o descuido total, cada descuido nuevo tiene una chance
    // creciente de disparar la escapada (10% en el 6o, +10% por cada uno
    // mas, tope en 50% desde el 10o)
    if (careMistakes >= 6) {
      int runChance = (careMistakes - 5) * 10;
      if (runChance > 50) runChance = 50;
      if ((int)random(100) < runChance) neglectTicks = RUNAWAY_TICKS;
    }
  }
  // peso al tope (100): penalizacion de vinculo, mismo enfriamiento que un descuido
  if (weightCapCooldown > 0) weightCapCooldown--;
  if (weight >= 100 && weightCapCooldown == 0) {
    weightCapCooldown = 60;
    if (bond > 20) bond -= 20; else bond = 0;
  }

  checkPerfectDayCycle(lastSeenEpoch);  // en vivo, lastSeenEpoch YA es el momento correcto
  checkFinalForm12h(lastSeenEpoch);
  checkMedals();  // la evolucion la dispara el usuario (canEvolveNow + tap), no el tick

  // abandono: en cuanto 3 de los 4 valores estan a la vez en cero, ya esta
  // lista para escaparse. Es un pestillo de un solo sentido: una vez
  // activado, NINGUN cuidado posterior lo revierte -- si se ignoro tanto
  // como para llegar aqui, ya no hay vuelta atras, solo queda presenciarlo
  int zeroCount = (fullness == 0) + (joy == 0) + (energy == 0) + (hygiene == 0);
  if (zeroCount >= 3) neglectTicks = RUNAWAY_TICKS;

  // ciclo completo (forma final + 7 dias): la despedida NO salta sola; queda
  // lista (canFarewellNow) y la dispara el usuario con el boton, para que la vea

  // autoguardado periodico: NO escribir a flash aqui (corre dentro del loop,
  // mientras se anima); solo marcar y dejar que el loop lo vuelque al atenuar
  if (++ticksSinceSave >= 5) pendingSave = true;
}

// vuelca el guardado periodico pendiente (lo llama el loop en un momento sin
// animacion para que el paron de la escritura a flash no se vea)
void Pet::flushSave() {
  if (pendingSave) save();
}

// puntos de ramificacion (una especie con mas de una evolucion posible).
// A nivel de archivo para que evolve() y lineHasUnregistered() compartan
// la misma tabla.
struct BranchRule { int16_t from; int16_t opts[5]; uint8_t n; };
static const BranchRule BRANCHES[] = {
  { 44,  { 45, 182, 0, 0, 0 }, 2 },   // Gloom -> Vileplume / Bellossom (gen2)
  { 61,  { 62, 186, 0, 0, 0 }, 2 },   // Poliwhirl -> Poliwrath / Politoed (gen2)
  { 79,  { 80, 199, 0, 0, 0 }, 2 },   // Slowpoke -> Slowbro / Slowking (gen2)
  { 133, { 134, 135, 136, 196, 197 }, 5 },  // Eevee: 3x gen1 + 2x gen2
  { 236, { 106, 107, 237, 0, 0 }, 3 },      // Tyrogue -> Hitmonlee/Hitmonchan/Hitmontop
  { 265, { 266, 268, 0, 0, 0 }, 2 },  // Wurmple -> Silcoon / Cascoon (gen3)
  { 290, { 291, 292, 0, 0, 0 }, 2 },  // Nincada -> Ninjask / Shedinja (gen3)
  { 366, { 367, 368, 0, 0, 0 }, 2 },  // Clamperl -> Huntail / Gorebyss (gen3)
};
static const BranchRule *findBranch(int16_t from) {
  for (const BranchRule &br : BRANCHES)
    if (br.from == from) return &br;
  return nullptr;
}

// quedan miembros sin registrar en la linea evolutiva de esta base?
bool Pet::lineHasUnregistered(int16_t base) const {
  int16_t cur = base;
  for (int guard = 0; cur >= 1 && cur <= DEX_COUNT && guard < 6; guard++) {
    if (!isRegistered(cur)) return true;
    const BranchRule *br = findBranch(cur);
    if (br) {
      for (int i = 0; i < br->n; i++)
        if (!isRegistered(br->opts[i])) return true;
      return false;
    }
    cur = DEX_TBL[cur].evolvesTo;
  }
  return false;
}

// resuelve una especie a su forma base, recorriendo la cadena hacia atras
// (caso especial: las tres evoluciones de Evoli resuelven a Evoli mismo)
// busca si "target" es una de las opciones de alguna rama; devuelve el
// "from" de esa rama, o 0 si no es ninguna
static int16_t findBranchSource(int16_t target) {
  for (const BranchRule &br : BRANCHES)
    for (int i = 0; i < br.n; i++)
      if (br.opts[i] == target) return br.from;
  return 0;
}

static int16_t baseFormOf(int16_t dex) {
  int16_t bs = findBranchSource(dex);
  if (bs) dex = bs;
  for (int guard = 0; guard < 6 && dex >= 1 && dex <= DEX_COUNT; guard++) {
    int16_t prev = 0;
    for (int16_t i = 1; i <= DEX_COUNT; i++) {
      if (DEX_TBL[i].evolvesTo == dex) { prev = i; break; }
    }
    if (!prev) return dex;  // ya es forma base
    dex = prev;
    int16_t bs2 = findBranchSource(dex);
    if (bs2) dex = bs2;  // si la forma anterior tambien es hoja de otra rama, seguir subiendo
  }
  return dex;
}

uint8_t Pet::eggRarity() const {
  return (eggTarget >= 1 && eggTarget <= DEX_COUNT) ? DEX_TBL[eggTarget].rarity : R_COMUN;
}

// elige la especie del huevo: tirada de rareza (mejorada por una despedida
// completa, castigada por una escapada) y sesgo hacia lineas incompletas
int16_t Pet::pickEggSpecies() {
  // primera partida: inicial clasico
  if (registeredCount() == 0) {
    return CLASSIC_DEX[random(NUM_CLASSIC_DEX)];
  }

  // 10% de posibilidades de que sea uno de los 3 favoritos (al azar entre
  // ellos); si el favorito es una evolucion, se resuelve a su forma base
  // para que sea la que efectivamente eclosiona
  if (random(100) < 10) {
    int16_t fav = favorites[random(3)];
    if (fav >= 1 && fav <= DEX_COUNT) return baseFormOf(fav);
  }

  uint8_t tier = R_COMUN;
  if (lastEnd != CER_RUNAWAY) {
    bool blessed = (lastEnd == CER_FAREWELL);
    int rare = (blessed ? 45 : 27) + careBonus();
    int leg = (registeredCount() >= 25) ? (blessed ? 10 : 3) + careBonus() / 3 : 0;
    int r = random(100);
    if (r < leg) tier = R_LEGENDARIO;
    else if (r < leg + rare) tier = R_RARO;
  }

  // candidatos del tier con linea incompleta; si no hay, baja de tier;
  // si la pokedex del tier esta completa, vale cualquiera del tier
  for (int pass = 0; pass < 2; pass++) {
    for (int t = tier; t >= R_COMUN; t--) {
      int16_t cand[130];
      int n = 0;
      for (int16_t d = 1; d <= DEX_COUNT && n < 130; d++) {
        if (DEX_TBL[d].rarity != t) continue;
        if (pass == 0 && !lineHasUnregistered(d)) continue;
        cand[n++] = d;
      }
      if (n > 0) return cand[random(n)];
    }
  }
  return CLASSIC_DEX[random(NUM_CLASSIC_DEX)];  // inalcanzable, por si acaso
}

void Pet::registerSpecies(int16_t dex) {
  if (dex < 1 || dex > DEX_COUNT) return;
  dexReg[(dex - 1) >> 3] |= (1 << ((dex - 1) & 7));
  if (shiny) dexShinyReg[(dex - 1) >> 3] |= (1 << ((dex - 1) & 7));
  checkPokedexTierUp();
}

// la racha y el vinculo mejoran el sorteo del huevo (0..~14)
int Pet::careBonus() const {
  int s = streak > 30 ? 30 : streak;
  return s / 3 + bond / 25;
}

// primer cuidado del dia: avanza la racha y afianza el vinculo
void Pet::registerCare() {
  if (ceremony != CER_NONE) return;
  uint32_t d = today();
  if (d == 0 || d == lastCareDay) return;  // sin reloj, o ya conto hoy
  // el salto 0->1 ya no tiene una gracia de 24h aparte: se trata igual que
  // cualquier otro salto, solo hace falta cruzar al siguiente dia natural
  // (lastCareDay se ancla al dia del nacimiento en hatch(), asi que el
  // primer cuidado el MISMO dia del nacimiento todavia no cuenta)
  if (lastCareDay == 0 || d == lastCareDay + 1) {
    streak++;
    if (petStreak < 255) petStreak++;
  } else {
    streak = 1;        // hubo un hueco de dias
    petStreak = 1;
  }
  lastCareDay = d;
  if (streak > bestStreak) {
    bestStreak = streak;
    pushAchievement(2, bestStreak);  // "Streak Record" -- cada vez que se supera el record
  }
  bond = clamp100(bond + 4);
  checkMedals();
  save();
}

void Pet::addBond(uint8_t amt) {
  uint32_t h = lastSeenEpoch ? lastSeenEpoch / 3600 : 0;
  if (h != bondHourRef) { bondHourRef = h; bondThisHour = 0; }  // nueva hora: se reinicia el tope
  if (bondThisHour >= 5) return;  // tope por hora: el vinculo no se farmea
  bond = clamp100(bond + amt);
  bondThisHour += amt;
}

// avanza el dia de la semana al cruzar la medianoche (o varios dias de
// golpe si el aparato estuvo apagado). Ajustable a mano por el usuario;
// esto solo se encarga del avance automatico
void Pet::advanceWeekday() {
  uint32_t d = today();
  if (d == 0) return;  // sin reloj todavia
  if (weekdayLastDay == 0 || d < weekdayLastDay) { weekdayLastDay = d; return; }
  if (d != weekdayLastDay) {
    uint32_t deltaDays = d - weekdayLastDay;
    if (deltaDays > 3650) deltaDays = 1;  // salvaguarda ante un salto absurdo
    weekday = (uint8_t)((weekday + deltaDays) % 7);
    weekdayLastDay = d;
  }
}

// "Toller Tag" v2: ein Zyklus geht von einem Aufwachen bis zum naechsten.
// Wird bei JEDEM simulierten Minutenschritt aufgerufen (autonome
// Vergabe+Benachrichtigung aus demselben Grund wie bei checkFinalForm12h:
// checkMedals() laeuft im Offline-Bucle nur EIN einziges Mal am Ende)
void Pet::checkPerfectDayCycle(uint32_t epoch) {
  if (!epoch || isEgg()) return;
  // ein Wert <=10 markiert den LAUFENDEN Zyklus als "nicht mehr sauber" --
  // das faerbt sofort das Durchstreichen ein, egal ob Tag oder Nacht,
  // unabhaengig vom (nur tagsueber zaehlenden) formalen Pflegefehler
  if (lowestStat() <= 10) perfectDayClean = false;

  bool wasSleeping = perfectDayWasSleeping;
  perfectDayWasSleeping = sleeping;
  if (wasSleeping && !sleeping) {  // gerade aufgewacht
    if (perfectDayCycleStart && perfectDayClean && !(medals & MED_STREAK7)) {
      medals |= MED_STREAK7;
      totalMedals++;
      pushAchievement(0, (uint32_t)MED_STREAK7);
      if (speciesId >= 1 && speciesId <= DEX_COUNT) {
        uint8_t beforeDex = dexMedals[speciesId];
        dexMedals[speciesId] |= MED_STREAK7;
        if (dexMedals[speciesId] != beforeDex) propagateMedalsToChain();
      }
      save();
    }
    perfectDayCycleStart = epoch;  // neuer Zyklus startet ab jetzt
    perfectDayClean = true;
    // steht GENAU beim Aufwachen schon ein Wert <=10 (nachts passiert),
    // startet der neue Zyklus gleich als "nicht sauber"
    if (lowestStat() <= 10) {
      perfectDayClean = false;
      // Nachtrag: nachts laeuft der FORMALE Pflegefehler nicht (siehe
      // "el sueno es descanso" weiter oben in tick()), deshalb wird hier
      // direkt beim Aufwachen genau EIN Pflegefehler nachgetragen, falls
      // noch ein Wert unter der Schwelle steht -- dieselben Auswirkungen
      // wie tagsueber (Bindung, Weglaufen-Eskalation). mistakeCooldown
      // danach gesetzt, damit nicht sofort noch ein zweiter Fehler durch
      // die normale Tages-Pruefung dazukommt
      careMistakes++;
      mistakeCooldown = 60;
      levelAtLastMistake = level();
      lastMistakeEpoch = epoch;
      if (bond > 10) bond -= 10; else if (bond > 1) bond = 1;
      if (careMistakes >= 6) {
        int runChance = (careMistakes - 5) * 10;
        if (runChance > 50) runChance = 50;
        if ((int)random(100) < runChance) neglectTicks = RUNAWAY_TICKS;
      }
    }
  }
}

// "Endform": mismo motivo que checkPerfectDay para ser autonoma -- dentro
// del bucle offline de syncClock() se llama en cada minuto simulado, pero
// checkMedals() solo se llama UNA VEZ al final. Si esta funcion se
// limitara a poner el bit, checkMedals() ya se encontraria el bit puesto
// al capturar su "antes" y no notificaria nada
void Pet::checkFinalForm12h(uint32_t epoch) {
  if (!epoch || isEgg()) return;
  bool isFinal = DEX_TBL[speciesId].evolvesTo == 0;
  if (!isFinal) { finalFormSinceEpoch = 0; return; }
  if (!finalFormSinceEpoch) { finalFormSinceEpoch = epoch; return; }
  if (epoch >= finalFormSinceEpoch && epoch - finalFormSinceEpoch >= 12UL * 3600 && !(medals & MED_FINAL)) {
    medals |= MED_FINAL;
    totalMedals++;
    pushAchievement(0, (uint32_t)MED_FINAL);
    if (speciesId >= 1 && speciesId <= DEX_COUNT) {
      uint8_t beforeDex = dexMedals[speciesId];
      dexMedals[speciesId] |= MED_FINAL;
      if (dexMedals[speciesId] != beforeDex) propagateMedalsToChain();
    }
    save();
  }
}

void Pet::checkMedals() {
  if (isEgg()) return;
  // seguimiento de "peso en verde ininterrumpido" para Top Form: se seguira
  // llamando esta funcion con la frecuencia suficiente (cada tick) para que
  // esto capture cualquier cambio de peso con buena precision
  if (weight <= 66) {
    if (weightGreenSinceEpoch == 0 && lastSeenEpoch) weightGreenSinceEpoch = lastSeenEpoch;
  } else {
    weightGreenSinceEpoch = 0;
  }
  uint16_t before = medals;
  // los nombres de las constantes se quedan igual (MED_LV10/25) pero lo que
  // representan ahora es otra cosa: "Walker" y "Rendezvous". Todas estas
  // condiciones (salvo Final Form) se acumulan de por vida, desde el
  // ultimo huevo -- ya NO se reinician al evolucionar
  if ((bigTripDone && stepsSinceHatch >= 10000) || everSoloEvent5) medals |= MED_LV10;  // "Vagabund"
  if (everGot2BerriesOneTrip || everSoloEvent1) medals |= MED_LV25;                   // "Rendezvous"
  if (winStreak >= 10) medals |= MED_LV50;  // "Champion": 10 victorias seguidas sin derrota
  if (berryLovedCount >= 30) medals |= MED_BERRY;
  if (bond >= 100) medals |= MED_BOND;
  // "Endform" (MED_FINAL) ya no se concede aqui: la maneja checkFinalForm12h()
  // de forma autonoma, con el requisito de 24h rodantes
  // Top Form / In Shape: 50 puntos de por vida en cada uno de los tres
  // entrenamientos (ataque/defensa/velocidad) y 24h SEGUIDAS con el peso
  // en la franja "delgado o normal" (<=66) -- las cuatro a la vez
  bool weightGreen24h = weightGreenSinceEpoch && lastSeenEpoch && lastSeenEpoch >= weightGreenSinceEpoch &&
                        (lastSeenEpoch - weightGreenSinceEpoch >= 24UL * 3600);
  bool allThree50 = lifeTrAtk >= 50 && lifeTrDef >= 50 && lifeTrSpe >= 50;
  bool sum150 = ((int)lifeTrAtk + lifeTrDef + lifeTrSpe) >= 150;
  if ((allThree50 || sum150) && weightGreen24h) medals |= MED_FIT;
  uint16_t gained = medals & ~before;
  if (gained) {
    for (int b = 0; b < MED_COUNT; b++) {
      if (gained & (1 << b)) { totalMedals++; pushAchievement(0, (uint32_t)(1 << b)); }
    }
  }
  // se sincroniza SIEMPRE (no solo si "gained" tiene algo nuevo): tras
  // evolucionar, la forma nueva hereda medallas ya ganadas antes, y hay que
  // propagarlas hacia atras aunque esta forma en concreto no gane nada
  // nuevo en este mismo tick. Ahora TODAS las medallas (no solo Final
  // Form) se conceden retroactivamente a toda la cadena de formas
  // anteriores desde el huevo: se ganan "de por vida", no por forma
  if (speciesId >= 1 && speciesId <= DEX_COUNT) {
    uint8_t beforeDex = dexMedals[speciesId];
    dexMedals[speciesId] |= (uint8_t)medals;
    bool dexChanged = (dexMedals[speciesId] != beforeDex);
    if (dexChanged) propagateMedalsToChain();
    if (gained || dexChanged) save();
  } else if (gained) {
    save();
  }
}

// todas las medallas actuales se conceden tambien a TODAS las formas
// anteriores de la cadena (no solo la inmediata) -- las medallas se ganan
// "de por vida" del individuo, no por forma. Tiene en cuenta los puntos de
// ramificacion (findBranchSource) para que cadenas como Evoli tambien
// propaguen correctamente hacia atras
void Pet::propagateMedalsToChain() {
  int16_t cur = speciesId;
  for (int guard = 0; guard < 10; guard++) {  // las cadenas nunca son tan largas
    int16_t prev = findBranchSource(cur);
    if (!prev) {
      for (int16_t i = 1; i <= DEX_COUNT; i++) {
        if (DEX_TBL[i].evolvesTo == cur) { prev = i; break; }
      }
    }
    if (!prev) break;
    dexMedals[prev] |= (uint8_t)medals;
    cur = prev;
  }
}

void Pet::rename(const char *name) {
  strncpy(nick, name, sizeof(nick) - 1);
  nick[sizeof(nick) - 1] = 0;
  save();
}

static uint16_t calcStat(uint8_t base, uint8_t gene, uint8_t lvl, uint8_t tr) {
  return (uint16_t)base * gene / 100 + lvl + tr;
}

uint16_t Pet::atkStat() const {
  return isEgg() ? 0 : calcStat(DEX_TBL[speciesId].bAtk, geneAtk, level(), trAtk);
}
uint16_t Pet::defStat() const {
  return isEgg() ? 0 : calcStat(DEX_TBL[speciesId].bDef, geneDef, level(), trDef);
}
uint16_t Pet::speStat() const {
  return isEgg() ? 0 : calcStat(DEX_TBL[speciesId].bSpe, geneSpe, level(), trSpe);
}

uint16_t Pet::registeredCount() const {
  uint16_t n = 0;
  for (int i = 1; i <= DEX_COUNT; i++)
    if (isRegistered(i)) n++;
  return n;
}

// forma final que ya cumplio su ciclo (7 dias): lista para despedirse. La
// despedida la dispara el usuario con el boton (no salta sola, para que la vea)
bool Pet::canFarewellNow() const {
  return !isEgg() && !sleeping && ceremony == CER_NONE &&
         DEX_TBL[speciesId].evolvesTo == 0 && ageMinutes >= FAREWELL_AGE_MIN &&
         finalFormSinceEpoch && lastSeenEpoch && lastSeenEpoch >= finalFormSinceEpoch &&
         (lastSeenEpoch - finalFormSinceEpoch) >= 24UL * 3600;
}

// abandono total durante 1h: lista para escaparse. La dispara el usuario con el
// boton (final triste); una vez activado NINGUN cuidado lo revierte, ver tick()
bool Pet::canRunawayNow() const {
  return !isEgg() && !sleeping && ceremony == CER_NONE && neglectTicks >= RUNAWAY_TICKS;
}

void Pet::startFarewell() {
  if (isEgg() || ceremony != CER_NONE) return;
  lastEnd = CER_FAREWELL;
  ceremony = CER_FAREWELL;
  ceremonyUntil = millis() + CEREMONY_MS;
  heartUntil = ceremonyUntil;  // corazones durante toda la despedida
  sfxPlay(SFX_BYE);
  save();
}

void Pet::startRunaway() {
  if (isEgg() || ceremony != CER_NONE) return;
  lastEnd = CER_RUNAWAY;
  ceremony = CER_RUNAWAY;
  ceremonyUntil = millis() + CEREMONY_MS;
  sfxPlay(SFX_BYE);
  save();
}

bool Pet::release() {
  if (isEgg() || ceremony != CER_NONE) return false;
  uint32_t d = today();
  if (d && d == lastReleaseDay) return false;  // ya se solto uno hoy
  lastReleaseDay = d;
  lastEnd = CER_RELEASE;
  ceremony = CER_RELEASE;
  ceremonyUntil = millis() + CEREMONY_MS;
  heartUntil = ceremonyUntil;
  sfxPlay(SFX_BYE);
  save();
  return true;
}

void Pet::hatch() {
  speciesId = eggTarget;
  shiny = eggShiny;
  ageMinutes = 0;  // el tiempo que paso como huevo (ahora potencialmente largo,
                    // sin eclosion automatica) no debe contar para el nivel
  farDeclinedAge = 0;  // Zusammenbleiben-Sperre gehoert zum alten Individuum
  // genes del individuo: 90-110% por stat (cada crianza es unica)
  geneAtk = 90 + random(21);
  geneDef = 90 + random(21);
  geneSpe = 90 + random(21);
  trAtk = trDef = trSpe = 0;
  lifeTrAtk = lifeTrDef = lifeTrSpe = 0;  // "Top Form": version de por vida
  berryKnown = false;
  berryLovedCount = 0;
  bond = 0;          // vinculo, medallas y nombre son del individuo
  bondThisHour = 0;
  bondHourRef = 0;
  medals = 0;
  petStreak = 0;      // racha "de este bicho" para la medalla 7 dias; streak
                       // general NO se toca, sigue sin atarse a un individuo
  levelAtLastMistake = 0;
  perfectDayCycleStart = 0;  // Zeit bis zum ersten Aufwachen zaehlt noch nicht
  perfectDayClean = true;
  perfectDayWasSleeping = sleeping;  // aktuellen Zustand uebernehmen, um ein
                                       // falsches "gerade aufgewacht" beim
                                       // allerersten Tick zu vermeiden
  finalFormSinceEpoch = 0;  // "Endform": individuo nuevo, se fija de verdad en el primer checkFinalForm12h()
  hatchEpoch = lastSeenEpoch;
  pokemonDefeated = 0;    // "Champion": individuo nuevo, sin combates aun
  winStreak = 0;
  bigTripDone = false;    // "Vagabund": individuo nuevo, sin ausflug grande aun
  stepsSinceHatch = 0;
  everGot2BerriesOneTrip = false;  // "Rendezvous": individuo nuevo
  tripSoloCount = 0;       // "Pokemon-Ausflug": Tageslimit fuer das neue Individuum zuruecksetzen
  tripSoloDay = 0;
  everSoloEvent1 = false;  // "Rendezvous" (alternativa): individuo nuevo
  everSoloEvent5 = false;  // "Vagabund" (alternativa): individuo nuevo
  weightGreenSinceEpoch = 0;  // racha de peso para "Top Form": empieza de cero
  // el streak se ancla siempre al dia del nacimiento: asi el primer cuidado
  // el MISMO dia no cuenta todavia, hace falta cruzar al dia siguiente
  // (tambien evita que el tiempo como huevo cuente como un hueco de dias)
  lastCareDay = today();
  trainCount = 0;
  battleCount = 0;
  gameCount = 0;
  nick[0] = 0;
  registerSpecies(speciesId);  // criado = registrado en la pokedex
  if (speciesId >= 1 && speciesId <= DEX_COUNT && dexRaised[speciesId] < 65535) dexRaised[speciesId]++;
  checkPerfectDayCycle(lastSeenEpoch);
  checkFinalForm12h(lastSeenEpoch);
  checkMedals();     // por si nace ya en forma final (legendario)
  sfxFlush();
  sfxPlay(SFX_HATCH);
  save();
}

// ¿se dan ya las condiciones para evolucionar? Cada descuido retrasa la
// evolucion 1 nivel, y ademas tiene que estar bien cuidado en ese momento
// (ninguna estadistica por debajo de 40). NO evoluciona sola: la dispara el
// usuario tocando al bicho (evolve()), para que vea la transformacion.
bool Pet::canEvolveNow() const {
  if (isEgg() || sleeping || ceremony != CER_NONE) return false;
  const DexEntry &d = DEX_TBL[speciesId];
  if (d.evolvesTo == 0) return false;
  return level() >= (uint8_t)(d.evolveLevel + careMistakes) && lowestStat() >= 40;
}

void Pet::evolve() {
  if (!canEvolveNow()) return;
  const DexEntry &d = DEX_TBL[speciesId];
  prevSpeciesId = speciesId;
  int16_t next = d.evolvesTo;
  const BranchRule *br = findBranch(speciesId);
  if (br) {
    // rama: prefiere la evolucion que aun falte en el pokedex
    int16_t opts[5];
    int n = 0;
    for (int i = 0; i < br->n; i++)
      if (!isRegistered(br->opts[i])) opts[n++] = br->opts[i];
    next = n > 0 ? opts[random(n)] : br->opts[random(br->n)];
  }
  speciesId = next;
  registerSpecies(speciesId);
  if (speciesId >= 1 && speciesId <= DEX_COUNT && dexRaised[speciesId] < 65535) dexRaised[speciesId]++;
  // las medallas se ganan "de por vida" del individuo (desde el huevo hasta
  // la despedida/huida), YA NO se reinician al evolucionar -- se propagan
  // ademas retroactivamente a esta nueva forma via checkMedals() en el
  // siguiente tick. El vinculo tampoco se reduce mas al evolucionar
  trainCount = 0;
  battleCount = 0;
  gameCount = 0;
  berryKnown = false;  // hay que redescubrir la baya favorita en la nueva forma
  evoDeclinedOnce = false;  // la nueva forma tiene su propia oportunidad de pregunta automatica
  evoDeclinedLv = 0;
  sfxPlay(SFX_EVOLVE);
  evolveUntil = millis() + EVOLVE_ANIM_MS;
  save();
}

bool Pet::feed() {
  return feedBerry(0);
}

bool Pet::feedBerry(uint8_t color) {
  if (ceremony != CER_NONE) return false;
  if (isEgg() || sleeping) return false;
  if (fullness > 90) return false;  // casi lleno: ya no le apetecen bayas, solo chuches
  if (berryOnCooldown()) return false;
  energy = clamp100(energy + 5);  // cualquier baya da un poco de energia
  weight = clamp100(weight + 5);  // cualquier baya engorda un poco
  if (lovesBerry(color)) {
    fullness = clamp100(fullness + 30);
    joy = clamp100(joy + 5);
    heartUntil = millis() + HEART_MS;  // "le encanta!"
    berryKnown = true;                 // descubierto: se muestra en la ficha
    if (berryLovedCount < 255) berryLovedCount++;  // para la medalla (30 veces)
    addBond(1);
  } else {
    fullness = clamp100(fullness + 20);
  }
  eatUntil = millis() + EAT_ANIM_MS;
  berryCooldownUntilEpoch = lastSeenEpoch ? lastSeenEpoch + 300UL : 0;  // 5 min
  registerCare();
  save();
  return true;
}

bool Pet::feedCandy() {
  if (ceremony != CER_NONE) return false;
  if (isEgg() || sleeping) return false;
  if (weight >= 100) return false;  // kugelrund: keine chuches mas
  fullness = clamp100(fullness + 10);
  joy = clamp100(joy + 10);
  energy = clamp100(energy + 10);
  weight = clamp100(weight + 12);  // las chuches pasan factura
  eatUntil = millis() + EAT_ANIM_MS;
  registerCare();
  save();
  return true;
}

uint8_t Pet::playResult(uint8_t score) {
  if (ceremony != CER_NONE || isEgg()) return 0;
  uint8_t before = trSpe;
  uint8_t v = trSpe + score / 3;  // jugar entrena la velocidad
  trSpe = v > 100 ? 100 : v;
  uint8_t gained = trSpe - before;  // aumento REAL aplicado (puede ser menor si topaba en 100)
  if (lifeTrSpe < 255 - gained) lifeTrSpe += gained; else lifeTrSpe = 255;  // "Top Form": de por vida
  joy = clamp100((int)joy + score);  // +1 por cada punto de score
  energy = (energy > 5) ? energy - 5 : 0;
  fullness = (fullness > 1) ? fullness - 1 : 0;
  { uint8_t wLoss = score / 5; weight = (weight > wLoss) ? weight - wLoss : 0; }
  if (score >= 10) { heartUntil = millis() + HEART_MS; addBond(1); }
  if (score > gameHi) gameHi = score;   // record del minijuego
  if (gameCount < 255) gameCount++;     // para "Top Form"
  registerCare();
  save();
  return gained;
}

// saco de entrenamiento: los golpes entrenan la fuerza. Devuelve la subida.
uint8_t Pet::trainStrength(uint16_t hits) {
  if (ceremony != CER_NONE || isEgg()) return 0;
  uint8_t gain = 0;
  if (hits >= 30) {                 // menos de 30 golpes: ningun punto
    gain = (hits - 30) / 5;         // se cuenta a partir del golpe 30, no desde el 1
    // sin tope por sesion: solo el limite general de 100 de mas abajo
  }
  uint8_t before = trAtk;
  uint8_t v = trAtk + gain;
  trAtk = v > 100 ? 100 : v;
  uint8_t applied = trAtk - before;  // aumento REAL aplicado (puede ser menor si topaba en 100)
  if (lifeTrAtk < 255 - applied) lifeTrAtk += applied; else lifeTrAtk = 255;  // "Top Form": de por vida
  energy = (energy > 5) ? energy - 5 : 0;   // cansa
  fullness = (fullness > 1) ? fullness - 1 : 0;
  weight = (weight > 2) ? weight - 2 : 0;
  joy = clamp100(joy + 5);
  if (hits >= 30) { heartUntil = millis() + HEART_MS; addBond(1); }
  if (trainCount < 255) trainCount++;  // para "Top Form"
  registerCare();
  save();
  return applied;
}

// resultado de un combate: victoria cuenta para la medalla "Champion" y da
// +30 alegria; derrota resta alegria en su lugar. Energia/hambre bajan igual
// en ambos casos (el combate cansa, gane o pierda)
// resultado de un combate: victoria cuenta para la medalla "Champion" y da
// +30 alegria; derrota resta alegria en su lugar. Energia/hambre bajan igual
// en ambos casos (el combate cansa, gane o pierda). oppDex sirve para llevar
// la cuenta de victorias/derrotas por especie (tasa de victorias del dex)
uint8_t Pet::battleResult(bool won, int16_t oppDex, uint8_t blockedHits) {
  if (ceremony != CER_NONE || isEgg()) return 0;
  if (won) {
    joy = clamp100((int)joy + 10);
    if (pokemonDefeated < 65535) pokemonDefeated++;  // contador general, ya sin uso para medallas
    if (winStreak < 255) winStreak++;
    heartUntil = millis() + HEART_MS;
    addBond(1);
  } else {
    joy = (joy > 10) ? joy - 10 : 0;
    winStreak = 0;  // cualquier derrota corta la racha
  }
  energy = (energy > 10) ? energy - 10 : 0;
  fullness = (fullness > 1) ? fullness - 1 : 0;
  weight = (weight > 2) ? weight - 2 : 0;
  uint8_t before = trDef;
  int defAdd = (won ? 2 : 1) + blockedHits;  // base por resultado + 1 por cada golpe abrido
  int v = (int)trDef + defAdd;
  trDef = (uint8_t)(v > 100 ? 100 : v);
  uint8_t gained = trDef - before;  // aumento REAL aplicado (puede topar en 100)
  if (lifeTrDef < 255 - gained) lifeTrDef += gained; else lifeTrDef = 255;  // "Top Form": de por vida
  // tasa de victorias por especie: cuenta tanto del lado del propio bicho
  // como del lado del rival (si son la misma especie, se anotan las dos)
  if (speciesId >= 1 && speciesId <= DEX_COUNT && won && dexBattleWins[speciesId] < 65535)
    dexBattleWins[speciesId]++;
  if (oppDex >= 1 && oppDex <= DEX_COUNT && !won && dexBattleWins[oppDex] < 65535)
    dexBattleWins[oppDex]++;
  registerCare();
  checkMedals();
  save();
  return gained;
}

void Pet::addSteps(uint32_t n) {
  if (!n) return;
  uint32_t before = totalSteps;
  totalSteps += n;
  stepsSinceHatch += n;  // "Vagabund": total desde el ultimo huevo
  uint32_t day = lastSeenEpoch / 86400;
  if (day != stepsDay) { stepsDay = day; stepsToday = 0; }
  stepsToday += n;
  // hitos de pasos: cada 100000, empezando en 100000 (se muestran despues
  // del ausflug en la pantalla principal, no durante el paseo)
  uint32_t prevMs = before / 100000, newMs = totalSteps / 100000;
  for (uint32_t m = prevMs + 1; m <= newMs && m > 0; m++) pushAchievement(1, m * 100000);
  save();
}

void Pet::play() {
  if (ceremony != CER_NONE) return;
  if (isEgg() || sleeping) return;
  joy = clamp100(joy + 25);
  energy = clamp100(energy - 10);
  fullness = clamp100(fullness - 5);
  heartUntil = millis() + HEART_MS;
  addBond(1);
  registerCare();
  save();
}

SleepAction Pet::sleepTap() {
  if (ceremony != CER_NONE || isEgg()) return SLEEP_BLOCKED;
  uint8_t h = hourNow();

  if (inLightsWindow(h)) {  // 20:00-07:59: Licht aus/an (era 18:00)
    if (!sleeping) {
      // ya se desperto esta manana (6:00-7:59): no se puede volver a dormir
      // del todo hasta la noche de verdad (20:00) -- solo el Power-Nap desde
      // las 8:00 sirve mientras tanto. EXCEPCION: si la energia ya cayo por
      // debajo de 25, se permite un Power-Nap tambien en esta franja
      if (h >= 6 && h < 8) {
        if (energy < 25 && !napping()) {
          if (cooldownActive(napCooldownUntilEpoch)) return SLEEP_NAP_COOLDOWN;
          napUntil = millis() + 10000UL;
          return SLEEP_NAP_START;
        }
        return SLEEP_NOT_YET;
      }
      return SLEEP_CONFIRM_NEEDED;  // freiwilliges Einschlafen (praktisch nur
                                      // 20-22h erreichbar): UI fragt erst nach
    }
    if (!lightsOut) {  // dormido a la fuerza (22h) sin luz apagada: se puede apagar ahora
      lightsOut = true;
      save();
      return SLEEP_LIGHTS_OUT;
    }
    // Aufwecken nur noch morgens (6-8h) manuell erlaubt. Abends (20-22h)
    // kann man zwar einschlafen lassen, aber NICHT mehr selbst wecken --
    // sonst liesse sich durch wiederholtes Schlafen-legen/Wecken am Abend
    // der "Toller Tag"-Zyklus mehrfach pro Stunde kuenstlich abschliessen
    // und neu starten (Medaille zu frueh, oder Durchstreichen zu frueh
    // aufgehoben)
    if (!inFreeToggleWindow(h) || (h >= 20 && h < 22)) return SLEEP_BLOCKED;
    sleeping = false;
    lightsOut = false;
    checkPerfectDayCycle(lastSeenEpoch);
    save();
    return SLEEP_LIGHTS_ON;
  }

  // 08:00-19:59: power-nap (antes terminaba a las 17:59; la franja muerta
  // 18-20h se elimino, ahora tambien es zona de nap)
  if (napping()) return SLEEP_NOT_YET;  // en curso: ya no se puede interrumpir
  if (cooldownActive(napCooldownUntilEpoch)) return SLEEP_NAP_COOLDOWN;
  if (energy > 80) return SLEEP_NOT_YET;
  napUntil = millis() + 10000UL;  // 10 segundos
  return SLEEP_NAP_START;
}

bool Pet::clean() {
  if (ceremony != CER_NONE) return false;
  if (poops == 0 && hygiene >= 25 && bathOnCooldown()) return false;
  poops = 0;
  hygiene = clamp100(hygiene + 20);
  bathCooldownUntilEpoch = lastSeenEpoch ? lastSeenEpoch + 1800UL : 0;  // 30 min
  heartUntil = millis() + HEART_MS;
  addBond(1);
  registerCare();
  save();
  return true;
}

bool Pet::caress() {
  if (ceremony != CER_NONE) return false;
  if (isEgg() || sleeping) return false;
  if (cooldownActive(caressCooldownUntilEpoch)) return false;  // 15 min entre caricias
  caressCooldownUntilEpoch = lastSeenEpoch ? lastSeenEpoch + 900UL : 0;
  joy = clamp100(joy + 10);
  heartUntil = millis() + HEART_MS;
  addBond(1);
  registerCare();
  save();
  return true;
}

void Pet::eggTap() {
  if (!isEgg()) return;
  registerCare();
  heartUntil = millis() + 300;  // nur so kurz wie der Tap-Ton selbst (260-290ms)
  if (++eggTaps >= 15) hatch();
  else save();
}

PetMood Pet::mood() const {
  if (sleeping || napping()) return MOOD_SLEEPING;
  if (eating()) return MOOD_EATING;
  if (lowestStat() < 25) return MOOD_SAD;
  return MOOD_HAPPY;
}

void Pet::save() {
  ticksSinceSave = 0;
  pendingSave = false;
  prefs.putUChar("full", fullness);
  prefs.putUChar("joy", joy);
  prefs.putUChar("ene", energy);
  prefs.putUChar("hyg", hygiene);
  prefs.putUChar("poop", poops);
  prefs.putUChar("wgt", weight);
  prefs.putUChar("gatk", geneAtk);
  prefs.putUChar("gdef", geneDef);
  prefs.putUChar("gspe", geneSpe);
  prefs.putUChar("tatk", trAtk);
  prefs.putUChar("tdef", trDef);
  prefs.putUChar("tspe", trSpe);
  prefs.putBool("bk", berryKnown);
  prefs.putUChar("blc", berryLovedCount);
  prefs.putBool("shy", shiny);
  prefs.putBool("eshy", eggShiny);
  prefs.putBool("stpk", starterPick);
  prefs.putBytes("dexsh", dexShinyReg, sizeof(dexShinyReg));
  prefs.putUInt("age", ageMinutes);
  prefs.putShort("dexn", speciesId);
  prefs.putShort("eggT2", eggTarget);
  prefs.putUChar("crack", eggTaps);
  prefs.putUChar("mist", careMistakes);
  prefs.putUChar("pstrk", petStreak);
  prefs.putUChar("lvmist", levelAtLastMistake);
  prefs.putUInt("mistep", lastMistakeEpoch);
  prefs.putUInt("wgreen", weightGreenSinceEpoch);
  prefs.putUInt("pdcyc", perfectDayCycleStart);
  prefs.putBool("pdclean", perfectDayClean);
  prefs.putBool("pdwassl", perfectDayWasSleeping);
  prefs.putUInt("finalep", finalFormSinceEpoch);
  prefs.putBool("tripflag", tripActiveFlag);
  prefs.putUInt("hatchep", hatchEpoch);
  prefs.putUChar("wkday", weekday);
  prefs.putUInt("wkdayd", weekdayLastDay);
  prefs.putUChar("achvcnt", achvCount);
  prefs.putBytes("achvk", achvKind, sizeof(achvKind));
  prefs.putBytes("achvv", achvValue, sizeof(achvValue));
  prefs.putBool("bigtrip", bigTripDone);
  prefs.putBool("rndzvs2", everGot2BerriesOneTrip);
  prefs.putBool("sleep", sleeping);
  prefs.putBool("lout", lightsOut);
  prefs.putUChar("lend", lastEnd);
  if (lastSeenEpoch) prefs.putUInt("seen", lastSeenEpoch);
  prefs.putUInt("napcd", napCooldownUntilEpoch);
  prefs.putUInt("bathcd", bathCooldownUntilEpoch);
  prefs.putUInt("carecd", caressCooldownUntilEpoch);
  prefs.putUInt("berrycd", berryCooldownUntilEpoch);
  prefs.putUInt("fardec", farDeclinedAge);
  prefs.putUChar("evodec", evoDeclinedLv);
  prefs.putBool("evodec1", evoDeclinedOnce);
  prefs.putUChar("pslot", poopSlotsDone);
  prefs.putUInt("pday", poopDay);
  prefs.putBytes("dexreg", dexReg, sizeof(dexReg));
  prefs.putBytes("dexmed", dexMedals, sizeof(dexMedals));
  prefs.putBytes("dexenc", dexEncounters, sizeof(dexEncounters));
  prefs.putBytes("dexbtl", dexBattleTotal, sizeof(dexBattleTotal));
  prefs.putBytes("dexbtw", dexBattleWins, sizeof(dexBattleWins));
  prefs.putBytes("dexrai", dexRaised, sizeof(dexRaised));
  prefs.putUInt("totsteps", totalSteps);
  prefs.putUInt("daysteps", stepsToday);
  prefs.putUInt("daystepd", stepsDay);
  prefs.putUShort("mberry", magicBerries);
  prefs.putUShort("strk", streak);
  prefs.putUShort("bstrk", bestStreak);
  prefs.putUInt("cday", lastCareDay);
  prefs.putUInt("relday", lastReleaseDay);
  prefs.putBytes("favs", favorites, sizeof(favorites));
  prefs.putUChar("pdtier", lastPokedexTier);
  prefs.putUChar("bond", bond);
  prefs.putUChar("lifeatk", lifeTrAtk);
  prefs.putUChar("lifedef", lifeTrDef);
  prefs.putUChar("lifespe", lifeTrSpe);
  prefs.putUInt("stepshatch", stepsSinceHatch);
  prefs.putUChar("winstreak", winStreak);
  prefs.putUInt("soloend", tripSoloEndEpoch);
  prefs.putUChar("solocnt", tripSoloCount);
  prefs.putUInt("soloday", tripSoloDay);
  prefs.putBool("soloev1", everSoloEvent1);
  prefs.putBool("soloev5", everSoloEvent5);
  prefs.putUChar("bondtoday", bondThisHour);
  prefs.putUChar("negl", neglectTicks);
  prefs.putUInt("bondhr", bondHourRef);
  prefs.putUShort("medal", medals);
  prefs.putUShort("tmedal", totalMedals);
  prefs.putUShort("ghi", gameHi);
  prefs.putUShort("pkdef", pokemonDefeated);
  prefs.putUChar("traincnt", trainCount);
  prefs.putUChar("battlecnt", battleCount);
  prefs.putUChar("gamecnt", gameCount);
  prefs.putString("nick", nick);
}

void Pet::load() {
  fullness = prefs.getUChar("full", 80);
  joy = prefs.getUChar("joy", 80);
  energy = prefs.getUChar("ene", 80);
  hygiene = prefs.getUChar("hyg", 100);
  poops = prefs.getUChar("poop", 0);
  weight = prefs.getUChar("wgt", 0);
  geneAtk = prefs.getUChar("gatk", 0);
  geneDef = prefs.getUChar("gdef", 0);
  geneSpe = prefs.getUChar("gspe", 0);
  if (geneAtk == 0) {  // mascota anterior a los genes: tirada unica ahora
    geneAtk = 90 + random(21);
    geneDef = 90 + random(21);
    geneSpe = 90 + random(21);
  }
  trAtk = prefs.getUChar("tatk", 0);
  trDef = prefs.getUChar("tdef", 0);
  trSpe = prefs.getUChar("tspe", 0);
  berryKnown = prefs.getBool("bk", false);
  berryLovedCount = prefs.getUChar("blc", 0);
  shiny = prefs.getBool("shy", false);
  eggShiny = prefs.getBool("eshy", false);
  starterPick = prefs.getBool("stpk", false);
  prefs.getBytes("dexsh", dexShinyReg, sizeof(dexShinyReg));
  ageMinutes = prefs.getUInt("age", 0);
  if (prefs.isKey("dexn")) {
    speciesId = prefs.getShort("dexn", -1);
    eggTarget = prefs.getShort("eggT2", 4);
  } else {
    // migracion desde la version con indices de flash (0-8)
    static const uint8_t OLD2DEX[9] = { 4, 5, 6, 1, 2, 3, 7, 8, 9 };
    int8_t old = prefs.getChar("spec", -1);
    speciesId = (old >= 0 && old < 9) ? OLD2DEX[old] : -1;
    int8_t oldT = prefs.getChar("eggT", 0);
    eggTarget = (oldT >= 0 && oldT < 9) ? OLD2DEX[oldT] : 4;
  }
  eggTaps = prefs.getUChar("crack", 0);
  careMistakes = prefs.getUChar("mist", 0);
  petStreak = prefs.getUChar("pstrk", 0);
  levelAtLastMistake = prefs.getUChar("lvmist", 0);
  lastMistakeEpoch = prefs.getUInt("mistep", 0);
  weightGreenSinceEpoch = prefs.getUInt("wgreen", 0);
  perfectDayCycleStart = prefs.getUInt("pdcyc", 0);
  perfectDayClean = prefs.getBool("pdclean", true);
  perfectDayWasSleeping = prefs.getBool("pdwassl", false);
  finalFormSinceEpoch = prefs.getUInt("finalep", 0);
  tripActiveFlag = prefs.getBool("tripflag", false);
  hatchEpoch = prefs.getUInt("hatchep", 0);
  weekday = prefs.getUChar("wkday", 0);
  weekdayLastDay = prefs.getUInt("wkdayd", 0);
  achvCount = prefs.getUChar("achvcnt", 0);
  prefs.getBytes("achvk", achvKind, sizeof(achvKind));
  prefs.getBytes("achvv", achvValue, sizeof(achvValue));
  bigTripDone = prefs.getBool("bigtrip", false);
  everGot2BerriesOneTrip = prefs.getBool("rndzvs2", false);
  sleeping = prefs.getBool("sleep", false);
  lightsOut = prefs.getBool("lout", false);
  lastEnd = prefs.getUChar("lend", CER_NONE);
  napCooldownUntilEpoch = prefs.getUInt("napcd", 0);
  bathCooldownUntilEpoch = prefs.getUInt("bathcd", 0);
  caressCooldownUntilEpoch = prefs.getUInt("carecd", 0);
  berryCooldownUntilEpoch = prefs.getUInt("berrycd", 0);
  farDeclinedAge = prefs.getUInt("fardec", 0);
  // Plausibilitaet: declineFarewell() setzt Alter+1440 und das Alter sinkt nie,
  // also kann der Wert nie groesser als Alter+1440 sein. Alles darueber ist ein
  // Rest eines frueheren Individuums (frueher wurde er beim Schluepfen nicht
  // zurueckgesetzt) und wuerde die Frage grundlos sperren
  if (farDeclinedAge > ageMinutes + 1440) farDeclinedAge = 0;
  evoDeclinedLv = prefs.getUChar("evodec", 0);
  evoDeclinedOnce = prefs.getBool("evodec1", false);
  poopSlotsDone = prefs.getUChar("pslot", 0);
  poopDay = prefs.getUInt("pday", 0);
  prefs.getBytes("dexreg", dexReg, sizeof(dexReg));
  prefs.getBytes("dexmed", dexMedals, sizeof(dexMedals));
  prefs.getBytes("dexenc", dexEncounters, sizeof(dexEncounters));
  prefs.getBytes("dexbtl", dexBattleTotal, sizeof(dexBattleTotal));
  prefs.getBytes("dexbtw", dexBattleWins, sizeof(dexBattleWins));
  prefs.getBytes("dexrai", dexRaised, sizeof(dexRaised));
  totalSteps = prefs.getUInt("totsteps", 0);
  stepsToday = prefs.getUInt("daysteps", 0);
  stepsDay = prefs.getUInt("daystepd", 0);
  magicBerries = prefs.getUShort("mberry", 0);
  streak = prefs.getUShort("strk", 0);
  bestStreak = prefs.getUShort("bstrk", 0);
  lastCareDay = prefs.getUInt("cday", 0);
  lastReleaseDay = prefs.getUInt("relday", 0);
  prefs.getBytes("favs", favorites, sizeof(favorites));
  lastPokedexTier = prefs.getUChar("pdtier", pokedexTierNum());
  bond = prefs.getUChar("bond", 0);
  lifeTrAtk = prefs.getUChar("lifeatk", 0);
  lifeTrDef = prefs.getUChar("lifedef", 0);
  lifeTrSpe = prefs.getUChar("lifespe", 0);
  stepsSinceHatch = prefs.getUInt("stepshatch", 0);
  winStreak = prefs.getUChar("winstreak", 0);
  tripSoloEndEpoch = prefs.getUInt("soloend", 0);
  tripSoloCount = prefs.getUChar("solocnt", 0);
  tripSoloDay = prefs.getUInt("soloday", 0);
  everSoloEvent1 = prefs.getBool("soloev1", false);
  everSoloEvent5 = prefs.getBool("soloev5", false);
  bondThisHour = prefs.getUChar("bondtoday", 0);
  neglectTicks = prefs.getUChar("negl", 0);
  bondHourRef = prefs.getUInt("bondhr", 0);
  medals = prefs.getUShort("medal", 0);
  totalMedals = prefs.getUShort("tmedal", 0);
  gameHi = prefs.getUShort("ghi", 0);
  pokemonDefeated = prefs.getUShort("pkdef", 0);
  trainCount = prefs.getUChar("traincnt", 0);
  battleCount = prefs.getUChar("battlecnt", 0);
  gameCount = prefs.getUChar("gamecnt", 0);
  prefs.getString("nick", nick, sizeof(nick));
  // siembra: la mascota actual cuenta como criada (guardados antiguos)
  if (speciesId >= 1) registerSpecies(speciesId);
}
