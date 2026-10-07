#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "dex.h"  // DEX_COUNT se usa en tamanos de array mas abajo

// 1 tick = 1 minuto de juego. Baja este valor para probar mas rapido
// (p. ej. 5000UL = las estadisticas caen 12x mas rapido).
#define PET_TICK_MS 60000UL
// Minutos de juego por nivel. Con 60, CHARMANDER evoluciona a las ~16 h
// de juego con cuidado perfecto. Baja a 1 para ver evoluciones al momento.
#define MINUTES_PER_LEVEL 60
#define EAT_ANIM_MS 2500UL
#define HEART_MS 1500UL
#define EVOLVE_ANIM_MS 5200UL              // animacion de evolucion (mas larga = mas epica)
#define CEREMONY_MS 10000UL                // duracion de la despedida en pantalla
#define FAREWELL_AGE_MIN (3UL * 24 * 60)   // se despide a los 3 dias de juego (en forma final)
#define RUNAWAY_TICKS 60                   // se escapa tras 1 h con TODO a cero
#define OVERWEIGHT_FROM 67                 // > este peso: "thick"/"dick", energia cae mas rapido

// ceremonias de fin de ciclo
enum : uint8_t { CER_NONE = 0, CER_FAREWELL, CER_RUNAWAY, CER_RELEASE };

enum PetMood : uint8_t { MOOD_HAPPY, MOOD_SAD, MOOD_EATING, MOOD_SLEEPING };

// resultado de tocar el boton de dormir/luz: la UI decide sonido/mensaje
enum SleepAction : uint8_t {
  SLEEP_LIGHTS_OUT,   // luz apagada, a dormir
  SLEEP_LIGHTS_ON,    // luz encendida, despierto
  SLEEP_NAP_START,    // power-nap iniciado
  SLEEP_NOT_YET,      // power-nap pedido pero no toca aun (cansancio/en curso)
  SLEEP_NAP_COOLDOWN, // power-nap: el cooldown de 60 min aun no ha pasado
  SLEEP_BLOCKED,      // luz: no se puede despertar entre 22-6 (silencioso, sin aviso)
  SLEEP_CONFIRM_NEEDED,  // dormir manual entre 20-22h: la UI debe preguntar antes
};

// medallas del individuo (bitmask)
enum : uint16_t {
  MED_LV10 = 1 << 0, MED_LV25 = 1 << 1, MED_LV50 = 1 << 2,
  MED_BERRY = 1 << 3, MED_STREAK7 = 1 << 4, MED_BOND = 1 << 5,
  MED_FINAL = 1 << 6, MED_FIT = 1 << 7,
};
#define MED_COUNT 8

class Pet {
public:
  // Estadisticas 0..100
  uint8_t fullness = 80;  // comida
  uint8_t joy = 80;       // felicidad
  uint8_t energy = 80;    // energia
  uint8_t hygiene = 100;  // limpieza
  uint8_t poops = 0;      // cacas en pantalla (max 3)
  uint8_t weight = 0;     // 0-100: las chuches engordan, el minijuego quema
  // genes (90-110%, se tiran al eclosionar) y entrenamiento (0-100)
  uint8_t geneAtk = 100, geneDef = 100, geneSpe = 100;
  uint8_t trAtk = 0, trDef = 0, trSpe = 0;
  uint8_t lifeTrAtk = 0, lifeTrDef = 0, lifeTrSpe = 0;  // "Top Form": version
                                     // de por vida de trAtk/trDef/trSpe, NO
                                     // se reinicia en evolve (solo en hatch)
  bool berryKnown = false;  // ya descubrio su baya favorita
  uint8_t berryLovedCount = 0;  // veces que se le dio su baya favorita (medalla: 30)
  bool shiny = false;       // variante de color rara (se sortea en el huevo)
  uint32_t ageMinutes = 0;
  int16_t speciesId = -1;      // numero de Pokedex (1-151), -1 = huevo
  int16_t prevSpeciesId = -1;  // para la animacion de evolucion
  uint8_t careMistakes = 0;   // descuidos: cada uno retrasa la evolucion 1 nivel
  uint32_t weightGreenSinceEpoch = 0;  // "Top Form": 0 = no esta en verde
                                         // ahora mismo; si no, desde cuando
                                         // lleva ininterrumpido en verde
  // "Toller Tag" v2: un ciclo va de un despertar al siguiente despertar.
  // Se concede solo si NINGUNO de los cuatro valores cayo a <=10 en TODO
  // el ciclo (de dia o de noche -- de noche no hay descuidos formales, asi
  // que se vigila el valor directamente). perfectDayClean tambien decide
  // el tachado visual: tachado en cuanto se pone en falso, sea de dia o de
  // noche
  uint32_t perfectDayCycleStart = 0;  // 0 = aun no hubo ningun despertar
                                        // (un huevo recien nacido no cuenta
                                        // el tiempo hasta el primer despertar)
  bool perfectDayClean = true;        // el ciclo ACTUAL sigue sin ningun
                                        // valor <=10 hasta ahora
  bool perfectDayWasSleeping = false;  // para detectar la transicion
                                         // dormido->despierto (un despertar)
  uint32_t finalFormSinceEpoch = 0;  // "Endform": desde cuando la especie
                                       // ACTUAL es una forma final (24h
                                       // rodantes); 0 = no es forma final
                                       // ahora mismo (o aun no se ha fijado)
  uint32_t hatchEpoch = 0;         // cuando salio del huevo esta cria (nunca
                                     // se reinicia salvo con el proximo huevo);
                                     // para la gracia de 24h antes del primer
                                     // streak/record
  uint8_t weekday = 0;             // 0=domingo..6=sabado; ajustable a mano,
                                    // avanza solo al cruzar la medianoche
  uint32_t weekdayLastDay = 0;     // ultimo "today()" en que se actualizo
  bool sleeping = false;
  bool lightsOut = false;  // luz apagada a mano (independiente de 'sleeping': se
                            // puede dormir a la fuerza a las 22h sin luz apagada)
  bool tripActive = false;      // en sincronia con tripOpen (TamaPoke.ino);
                                  // no se persiste, un ausflug no sobrevive
                                  // a un apagon de todos modos
  bool tripActiveFlag = false;  // SI se persiste, solo para poder detectar
                                  // en el siguiente arranque que el aparato
                                  // se apago de golpe (boton) durante un
                                  // ausflug -- entonces se muestra un aviso
  // "Treffen" (Bluetooth): absichtlich NICHT persistiert -- ein Abschalten
  // waehrend der 3 Minuten soll den Zugewinn verlieren (siehe Vorgabe)
  bool visiting = false;          // Besuchszeit laeuft gerade (blockiert
                                    // Handlungen wie eine Zeremonie)
  bool visitIsHost = false;       // true = das andere Pokemon besucht UNS
                                    // (bleibt im Bild); false = UNSER
                                    // Pokemon ist unterwegs (verlaesst das Bild)
  uint32_t visitEndEpoch = 0;     // 0 = kein Besuch aktiv
  int16_t visitPartnerDex = 0;
  uint8_t visitPartnerFullness = 0, visitPartnerJoy = 0, visitPartnerEnergy = 0, visitPartnerHygiene = 0;
  bool visitPartnerWasNew = false;  // fuer die Begegnungs-Anzeige ("NEU")
  bool visitFighting = false;       // Streit statt Spielen (20%, ausser bei Beeren-Geschenk)
  int8_t visitBerryDelta = 0;       // +1 = Beere geschenkt bekommen, -1 = verschenkt, 0 = keine

  uint32_t tripSoloEndEpoch = 0;  // "Pokemon-Ausflug" (mandar al bicho solo):
                                    // 0 = inactivo; si no, cuando termina el
                                    // temporizador. A DIFERENCIA del ausflug
                                    // normal, esto SI se persiste: debe
                                    // seguir corriendo tras un apagon,
                                    // siempre que no se haya cruzado la
                                    // franja de sueno forzado (22-6h)
  uint8_t tripSoloCount = 0;      // veces enviado solo HOY (maximo 5)
  uint32_t tripSoloDay = 0;       // dia (today()) al que corresponde tripSoloCount
  bool everSoloEvent1 = false;    // alternativa para "Rendezvous"
  bool everSoloEvent5 = false;    // alternativa para "Vagabund"
  bool stepsThisInterval = false;  // se puso a true si hubo pasos desde la
                                     // ultima vez que se decidio si baja la
                                     // alegria; TamaPoke.ino lo activa
  bool stepsForWeight = false;  // igual que stepsThisInterval, pero con su
                                  // propia ventana fija de 3 min (para el peso)
  bool stepsForEnergy = false;  // ventana fija de 7 min (para la energia)
  bool stepsForHunger = false;  // ventana fija de 5 min (para el hambre)
  uint32_t lastSeenEpoch = 0;   // ultima hora RTC vista (para progresion offline)
  uint8_t ceremony = CER_NONE;  // despedida/escapada/liberacion en curso
  uint8_t lastEnd = CER_NONE;   // como acabo la anterior (afecta al huevo)
  uint8_t dexReg[49] = { 0 };       // pokedex de criados (bitmap, ceil(386/8))
  uint8_t dexMedals[DEX_COUNT + 1] = { 0 };   // por especie (indice 1-386): medallas ganadas
                                     // alguna vez con ella, se queda aunque el bicho
                                     // actual se vaya (persistente, no del individuo)
  uint16_t dexEncounters[DEX_COUNT + 1] = { 0 };  // "Begegnungen": nada lo incrementa aun
  uint16_t dexBattleTotal[DEX_COUNT + 1] = { 0 };  // combates totales (como rival Y como
                                           // propio bicho, ambos cuentan)
  uint16_t dexBattleWins[DEX_COUNT + 1] = { 0 };    // victorias de esta especie en esos combates
  uint16_t dexRaised[DEX_COUNT + 1] = { 0 };      // veces criada por el jugador (hatch/evolve)
  uint32_t totalSteps = 0;   // pasos acumulados en Ausflug/Trip (toda la vida
                              // del guardado, no del individuo); base para
                              // futuros encuentros durante el paseo
  uint32_t stepsToday = 0;   // pasos del dia natural en curso
  uint32_t stepsDay = 0;     // dia (epoch/86400) al que corresponde stepsToday
  uint16_t magicBerries = 0; // bayas magicas en posesion (premio de encuentros)
  bool bigTripDone = false;   // para "Vagabund": >=3000 pasos en UN ausflug
  uint32_t stepsSinceHatch = 0;  // para "Vagabund": pasos totales desde el
                                   // ultimo huevo (a diferencia de totalSteps,
                                   // que es de toda la partida)
  // etapa del pokedex (1-10, u 11 para la etapa final) -- calculo numerico
  // puro, en sincronia con pokedexTier() del .ino (misma formula)
  uint8_t pokedexTierNum() const {
    uint16_t reg = registeredCount();
    if (reg >= DEX_COUNT) return 11;
    int pct = reg * 100 / DEX_COUNT;
    int stage = (pct == 0) ? 1 : ((pct - 1) / 10) + 1;
    if (stage > 10) stage = 10;
    return (uint8_t)stage;
  }
  // avisa (banner tipo medalla) si se acaba de subir de etapa
  void checkPokedexTierUp() {
    uint8_t t = pokedexTierNum();
    if (t > lastPokedexTier) {
      lastPokedexTier = t;
      pushAchievement(3, t);
      save();  // igual que las otras tres notificaciones: persiste ya, no espera al siguiente save() de otro lado
    }
  }
  // marca una especie como "vista" en el dex, SIN tocar el registro shiny
  // (a diferencia de registerSpecies, que usa el shiny del bicho actual --
  // no vale para un encuentro salvaje ajeno durante un ausflug)
  void registerSeen(int16_t dex) {
    if (dex < 1 || dex > DEX_COUNT) return;
    dexReg[(dex - 1) >> 3] |= (1 << ((dex - 1) & 7));
    checkPokedexTierUp();
  }
  // ajuste manual del dia de la semana: fija tambien la referencia a "hoy"
  // para que el avance automatico no lo vuelva a subir de mas al momento
  void setWeekday(uint8_t wd) {
    weekday = wd % 7;
    weekdayLastDay = lastSeenEpoch ? lastSeenEpoch / 86400 : 0;
    save();
  }
  // fija un favorito (0..2) y guarda; no valida rango del dex, eso lo hace
  // quien llama
  void setFavorite(uint8_t slot, int16_t dex) {
    if (slot > 2) return;
    favorites[slot] = dex;
    save();
  }
  // vinculo por caminar en un ausflug (+1 cada 1000 pasos); usa el mismo
  // tope por hora que el resto de fuentes de vinculo
  void addTripBond() { addBond(1); }
  // resta vinculo directamente (sin pasar por el tope por hora, que solo
  // aplica a ganancias); nunca baja de 0
  void loseBond(uint8_t amt) { bond = (bond > amt) ? bond - amt : 0; }
  void requestSave() { save(); }  // wrapper publico: save() es privado
  bool everGot2BerriesOneTrip = false;  // para "Rendezvous": 2 bayas en UN ausflug
  uint8_t dexShinyReg[49] = { 0 };  // criados en version shiny (bitmap, ceil(386/8))
  int16_t favorites[3] = { 1, 4, 7 };  // 3 favoritos para la portada del
                                          // pokedex y para el sorteo de huevo
  uint8_t lastPokedexTier = 1;  // ultima etapa avisada (para no repetir el banner)
  // racha de cuidado diario (del jugador: persiste entre crianzas)
  uint16_t streak = 0, bestStreak = 0;
  uint8_t petStreak = 0;  // igual que streak, pero solo del bicho actual (para
                           // la medalla "7 Days Streak"); streak/bestStreak
                           // siguen sin atarse a ningun individuo, como antes
  uint32_t lastCareDay = 0;
  uint32_t lastReleaseDay = 0;  // ultimo dia en que se "solto" un pokemon
                                  // (limite: solo una vez por dia natural)
  // vinculo (del bicho: sube lento con cuidado, se resetea al nacer otro)
  uint8_t bond = 0;
  char nick[24] = "";    // apodo (vacio = nombre de especie); 24 bytes para
                           // que quepan hasta 11 caracteres visibles aunque
                           // TODOS sean umlauts (2 bytes cada uno)
  // medallas: del individuo + contador acumulado entre todas las crianzas
  uint16_t medals = 0, totalMedals = 0;
  // cola de celebraciones (medallas + hitos), se consume una a una en la
  // pantalla principal: kind 0=medalla (value=bit), 1=hito de pasos
  // (value=100000,200000...), 2=record de racha (value=dias)
  static const uint8_t ACHV_QUEUE_SIZE = 8;
  uint8_t achvKind[ACHV_QUEUE_SIZE] = { 0 };
  uint32_t achvValue[ACHV_QUEUE_SIZE] = { 0 };
  uint8_t achvCount = 0;
  void pushAchievement(uint8_t kind, uint32_t value) {
    if (kind == 2) {  // record de racha: actualiza la ya en cola en vez de amontonar
      for (uint8_t i = 0; i < achvCount; i++) {
        if (achvKind[i] == 2) { achvValue[i] = value; return; }
      }
    }
    if (achvCount >= ACHV_QUEUE_SIZE) return;  // cola llena: se pierde (muy raro)
    achvKind[achvCount] = kind;
    achvValue[achvCount] = value;
    achvCount++;
  }
  void advanceAchievement() {  // la pantalla principal la llama al agotarse el tiempo
    if (achvCount == 0) return;
    for (uint8_t i = 1; i < achvCount; i++) {
      achvKind[i - 1] = achvKind[i];
      achvValue[i - 1] = achvValue[i];
    }
    achvCount--;
    save();  // persiste ya: si se apaga justo despues, no debe reaparecer lo ya visto
  }
  uint16_t gameHi = 0;      // record del minijuego de la pelota (sin limite
                              // de tiempo, tiene mas sentido un record ahi
                              // que en el entrenamiento, que si lo tiene)
  uint16_t pokemonDefeated = 0;  // contador general de combates ganados
  uint8_t winStreak = 0;         // victorias seguidas en combate (se reinicia
                                   // con cualquier derrota); para "Champion"
                                   // basta con winStreak>=10, ya que medals
                                   // conserva el bit para siempre una vez puesto

  void begin();                 // carga estado de NVS (o crea el primer huevo)
  void update(uint32_t nowMs);  // llamar en cada loop()

  // Acciones (botones tactiles)
  bool feed();              // baya roja (compatibilidad)
  bool feedBerry(uint8_t color);  // 0 roja, 1 azul, 2 verde; false si esta lleno o bloqueado
  // Cooldown aktiv? Schuetzt zusaetzlich gegen eine verfaelschte RTC-Zeit
  // (z.B. I2C-Lesefehler): kein Cooldown im Spiel dauert laenger als 30
  // Minuten, also gilt alles ueber 2h "Zukunft" als defekter Zeitstempel
  // und wird als abgelaufen behandelt statt dauerhaft zu blockieren
  bool cooldownActive(uint32_t deadline) const {
    if (!deadline || !lastSeenEpoch || lastSeenEpoch >= deadline) return false;
    return (deadline - lastSeenEpoch) <= 7200UL;
  }
  bool berryOnCooldown() const {  // true = bloqueado por el cooldown de 5 min
    if (fullness < 25) return false;  // hambre real: salta el cooldown
    return cooldownActive(berryCooldownUntilEpoch);
  }
  bool feedCandy();
  bool lovesBerry(uint8_t color) const {
    return !isEgg() && (speciesId % 3) == color;  // gusto oculto por especie
  }
  uint8_t playResult(uint8_t score);  // recompensa del minijuego (entrena VEL); devuelve el aumento real aplicado
  uint8_t trainStrength(uint16_t hits);  // saco de entrenamiento (entrena FUE)
  uint8_t battleResult(bool won, int16_t oppDex, uint8_t blockedHits);  // recompensa/castigo tras un
                                                  // combate; victoria cuenta
                                                  // para la medalla "Champion"
  void registerBattle() { if (battleCount < 255) battleCount++; }  // para "Top Form"
  void addSteps(uint32_t n);  // ausflug/trip: acumula total y del dia, guarda
  // baya magica: rellena por completo el valor elegido (0 hambre, 1 alegria,
  // 2 energia, 3 higiene). Devuelve false si no hay ninguna o no se puede
  bool eatMagicBerry(uint8_t which) {
    if (magicBerries == 0 || ceremony != CER_NONE || isEgg()) return false;
    magicBerries--;
    if (which == 0) fullness = 100;
    else if (which == 1) joy = 100;
    else if (which == 2) energy = 100;
    else if (which == 3) hygiene = 100;
    save();
    return true;
  }

  // stats de combate: base real de gen 1 x genes + nivel + entrenamiento
  uint16_t atkStat() const;
  uint16_t defStat() const;
  uint16_t speStat() const;
  void play();
  uint8_t hourNow() const { return hourAt(lastSeenEpoch); }  // hora real 0-23 (13 si no hay reloj)
  bool napping() const { return napUntil != 0; }
  uint32_t napSecondsLeft() const {  // para el contador en pantalla, redondeado hacia arriba
    if (!napUntil) return 0;
    uint32_t n = millis();
    uint32_t left = napUntil > n ? napUntil - n : 0;
    return (left + 999) / 1000;
  }
  SleepAction sleepTap();  // toca la zona de dormir: decide luz/nap segun hora+estado
  void confirmSleep() {    // nach "Ja" auf die 20-22h-Bestaetigungsfrage
    sleeping = true;
    lightsOut = true;
    checkPerfectDayCycle(lastSeenEpoch);
    save();
  }
  bool clean();  // false si esta en cooldown (30 min entre banos)
  bool bathOnCooldown() const {
    return cooldownActive(bathCooldownUntilEpoch);
  }
  bool caress();  // tocar al bicho; false si esta en cooldown (o dormido/huevo/ceremonia)
  void eggTap();  // tocar el huevo: 3 toques y eclosiona
  void newEgg();   // empezar de cero con un inicial aleatorio
  bool release();  // soltar (pulsacion larga + confirmar); false si ya se solto hoy
  void syncClock(uint32_t nowEpoch);  // aplica el tiempo transcurrido apagado
  void setClock(uint32_t nowEpoch);   // fija la hora sin aplicar progresion
  void guardClockJump(uint32_t deltaMinAbs, uint32_t oldEpoch);  // llamar SOLO tras un ajuste manual desde los
                            // ajustes (nunca desde comandos de depuracion):
                            // evita fabricar dias/rachas/peso-verde de la nada.
                            // deltaMinAbs = magnitud del ajuste en minutos
                            // (valor absoluto), decide si es una correccion
                            // pequena (se respeta) o un salto grande (se reinicia)
  void startFarewell();  // tambien usable desde la consola serie (BYE)
  void startRunaway();   // tambien usable desde la consola serie (RUN)

  bool isEgg() const { return speciesId < 0; }
  uint8_t eggCracks() const { return eggTaps; }
  bool eating() const { return millis() < eatUntil; }
  bool showHeart() const { return millis() < heartUntil; }
  bool evolving() const { return millis() < evolveUntil; }
  float evolveT() const {     // progreso de la animacion de evolucion 0..1
    uint32_t n = millis();
    uint32_t left = evolveUntil > n ? evolveUntil - n : 0;
    return 1.0f - (float)left / (float)EVOLVE_ANIM_MS;
  }
  bool canEvolveNow() const;  // condiciones de evolucion cumplidas (lista)
  void evolve();              // dispara la transformacion (la llama un toque del usuario)
  bool canFarewellNow() const;  // forma final + 7 dias: lista para despedirse (boton)
  bool canRunawayNow() const;   // abandono total 1h: lista para escaparse (boton triste)
  bool blocked() const { return ceremony != CER_NONE || visiting; }  // wie
                                    // eine Zeremonie: keine Handlungen ausser
                                    // zur Uhr wischen
  // el usuario decide en un dialogo; "mantener/quedaros" pospone y re-ofrece luego
  bool wantEvolveButton() const { return canEvolveNow() && !evoDeclinedOnce && !canRunawayNow(); }
  bool wantFarewellButton() const { return canFarewellNow() && ageMinutes >= farDeclinedAge && !canRunawayNow(); }
  void declineEvolve() { evoDeclinedLv = level(); evoDeclinedOnce = true; }  // no se reofrece mas solo
  void declineFarewell() { farDeclinedAge = ageMinutes + 1440; } // re-ofrece dentro de 1 dia
  uint32_t farDeclinedAgeValue() const { return farDeclinedAge; }  // nur lesen (Diagnose-Befehl FAREWELL)
  // primera partida: el jugador elige inicial (Bulbasaur/Charmander/Squirtle)
  bool awaitingStarter() const { return starterPick; }
  void chooseStarter(int16_t dex) { eggTarget = dex; starterPick = false; save(); }
  void factoryReset() { prefs.clear(); }  // borra la NVS (test: comando serie WIPE)
  void dbgRunawayReady() { fullness = joy = energy = hygiene = 0; neglectTicks = RUNAWAY_TICKS; }  // test
  uint8_t level() const;  // curva no lineal, ver implementacion en pet.cpp
  // minutos de edad necesarios para alcanzar targetLv (misma curva que
  // level()); usado por el comando de prueba serie "LVL n"
  static uint32_t minutesForLevel(uint16_t targetLv);
  // minutos ya transcurridos en el nivel actual, y duracion total de ese
  // nivel (para la barra de progreso) -- misma curva que level()
  void levelProgress(uint32_t &into, uint32_t &total) const;
  bool isRegistered(int16_t dex) const {
    return dex >= 1 && dex <= DEX_COUNT && (dexReg[(dex - 1) >> 3] & (1 << ((dex - 1) & 7)));
  }
  bool isShinyRegistered(int16_t dex) const {
    return dex >= 1 && dex <= DEX_COUNT && (dexShinyReg[(dex - 1) >> 3] & (1 << ((dex - 1) & 7)));
  }
  uint16_t registeredCount() const;
  bool lineHasUnregistered(int16_t base) const;
  uint8_t eggRarity() const;       // rareza del huevo actual (sin revelar especie)
  int16_t pickEggSpecies();        // publica para poder simular tiradas (EGGS)
  uint8_t lowestStat() const { return min(min(fullness, joy), min(energy, hygiene)); }
  PetMood mood() const;
  // progreso de la ceremonia de despedida/escapada, 0..1 (para animarla)
  float ceremonyT() const {
    if (ceremony == CER_NONE) return 0.0f;
    uint32_t n = millis();
    uint32_t left = ceremonyUntil > n ? ceremonyUntil - n : 0;
    return 1.0f - (float)left / (float)CEREMONY_MS;
  }

  // racha / vinculo / medallas / nombre
  void rename(const char *name);
  bool hasMedal(uint16_t m) const { return medals & m; }
  int careBonus() const;  // mejora del huevo por racha + vinculo

  // guardado periodico diferido: tick() marca pendiente y el loop lo vuelca
  // cuando la pantalla esta atenuada/apagada (la escritura a flash congela
  // ~1s ambos cores: asi no se ve ni corta el tactil)
  bool savePending() const { return pendingSave; }
  void flushSave();

private:
  Preferences prefs;
  uint32_t lastTick = 0;
  uint32_t eatUntil = 0;
  uint32_t heartUntil = 0;
  uint32_t evolveUntil = 0;
  uint32_t caressCooldownUntilEpoch = 0;  // cooldown de caricias (15 min), en
                                           // hora real y persistido: apagar y
                                           // encender ya no sirve para saltarselo
  uint32_t berryCooldownUntilEpoch = 0;   // cooldown entre bayas (5 min);
                                           // NO aplica a las chuches, y se
                                           // salta si fullness<25 (hambre real)
  int16_t eggTarget = 1;       // dex oculto que saldra del huevo
  bool eggShiny = false;       // sorpresa sorteada al crear el huevo
  uint8_t eggTaps = 0;
  uint8_t mistakeCooldown = 0;
  uint8_t weightCapCooldown = 0;  // como mistakeCooldown, pero para la
                                    // penalizacion de vinculo por peso=100
  uint8_t levelAtLastMistake = 0;  // ya no se usa para medallas, se queda por
                                     // compatibilidad con guardados viejos
  uint32_t lastMistakeEpoch = 0;   // para "Perfektionist": 48h sin descuidos
  uint8_t trainCount = 0;    // veces que se completo el saco (para "Top Form")
  uint8_t battleCount = 0;   // veces que se combatio (para "Top Form")
  uint8_t gameCount = 0;     // veces que se jugo el minijuego (para "Top Form")
  uint8_t ticksSinceSave = 0;
  bool pendingSave = false;     // guardado periodico pendiente de volcar
  uint8_t evoDeclinedLv = 0;    // ya no controla el reofrecimiento (ver
                                  // evoDeclinedOnce), se queda por compatibilidad
  bool evoDeclinedOnce = false;  // tras rechazar UNA vez, el boton automatico
                                   // no vuelve a aparecer -- solo se puede
                                   // evolucionar ya tocando el aviso verde
                                   // en la cuarta pagina de estado
  uint32_t farDeclinedAge = 0;  // "quedaros juntos": no ofrecer despedida hasta esta edad
  bool starterPick = false;     // primera partida: esperando que el jugador elija inicial
  uint8_t neglectTicks = 0;
  uint16_t goodTicks = 0;  // racha bien cuidado: forja la DEF
  uint32_t ceremonyUntil = 0;
  uint8_t bondThisHour = 0;    // tope por hora de subida de vinculo
  uint32_t bondHourRef = 0;    // hora (epoch/3600) a la que corresponde bondThisHour
  uint32_t napUntil = 0;           // millis() en el que termina el power-nap (0 = no hay)
  uint32_t napCooldownUntilEpoch = 0;  // hora RTC hasta la que no se puede hacer otro nap (0 = sin cooldown);
                                        // en epoca real (persistida) para que apagar y encender no sirva de truco
  uint32_t bathCooldownUntilEpoch = 0;  // igual, pero para el bano (30 min)
  uint8_t poopSlotsDone = 0;   // bitmask: que franjas de caca (0=8-11h,1=11-14h,2=14-17h,3=17-20h) ya paso hoy
  uint32_t poopDay = 0;        // dia (today()) al que corresponde poopSlotsDone; se reinicia al cambiar de dia

  // ventanas horarias del ciclo dia/noche (mismo criterio que sceneHour() del .ino)
  uint8_t hourAt(uint32_t epoch) const { return epoch ? (uint8_t)((epoch / 3600) % 24) : 13; }
  static bool inLightsWindow(uint8_t h) { return h >= 20 || h < 8; }       // Licht aus/an posible (era 18:00)
  static bool inFreeToggleWindow(uint8_t h) {                             // dormir/despertar libremente
    return (h >= 6 && h < 8) || (h >= 20 && h < 22);
  }

  uint32_t today() const { return lastSeenEpoch ? lastSeenEpoch / 86400 : 0; }
  void reconcileSleepState();  // corrige wach/durmiendo tras un salto de hora
  void registerCare();   // primer cuidado del dia: racha + vinculo
  void addBond(uint8_t amt);
  void checkMedals();
  // separado de checkMedals(): necesita el epoch EXACTO del momento
  // evaluado. Dentro del bucle de recuperacion offline de syncClock(),
  // lastSeenEpoch YA vale el epoch final (no el simulado paso a paso);
  // usarlo ahi daria lugar a un dia mal identificado y premios erroneos
  void checkPerfectDayCycle(uint32_t epoch);  // "Toller Tag" v2: vigila el
                                                // valor mas bajo en CADA
                                                // minuto y detecta despertares
  void checkFinalForm12h(uint32_t epoch);  // "Endform": mismo patron que
                                             // checkPerfectDay, autonoma
                                             // (notificacion+dex+save
                                             // incluidos), para que funcione
                                             // bien dentro del bucle offline
  void advanceWeekday();  // avanza al cruzar la medianoche (llamado desde tick/syncClock)
  void propagateMedalsToChain();  // TODAS las medallas: retroactivo a TODA la
                                    // cadena de formas anteriores
  void maybePoop(uint32_t epoch);  // 3 cacas/dia repartidas al azar entre 8-20h
  void tick();
  void hatch();
  void registerSpecies(int16_t dex);
  void save();
  void load();
  static uint8_t clamp100(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }
};
