// TamaPoke - tamagotchi pixel art inspirado en la gen 1
// para Waveshare ESP32-S3-Touch-AMOLED-1.75
//
// Librerias (Library Manager o repo de Waveshare):
//   - "GFX Library for Arduino" (moononournation), con soporte CO5300 QSPI
//   - "SensorLib" (Lewis He), driver tactil CST9217
//
// Placa: ESP32S3 Dev Module | Flash 16MB | PSRAM: OPI PSRAM | USB CDC On Boot: Enabled
//
// Los sprites y la tabla de especies se generan con tools/sprites.py (emit).

#include <Arduino.h>
#include <Wire.h>
#include "Arduino_GFX_Library.h"
#include "TouchDrvCSTXXX.hpp"
#include "pin_config.h"
#include "species.h"
#include "dex.h"
#include "pet.h"
#include "sdmon.h"
#include "bgmons.h"
#include "rtcbat.h"
#include "i18n.h"
#include "audio.h"
#include "imu.h"
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Version del firmware. Subir este numero en cada release (y manifest.json para
// el instalador web). Se muestra en la pantalla de ajustes y por serie al arrancar.
#define FW_VERSION "6.66"

// Pokedex-Regionen. Jede Region beginnt auf einer neuen Seite (16 Arten pro
// Seite, die letzte Seite einer Region kann halb leer sein): Kanto 10 Seiten,
// Johto 7, Hoenn 9, Sinnoh 7. Neue Regionen werden NUR hier hinten angehaengt
// (Name = Ueberschrift der Pokedex-Seite, Antippen = naechste Region).
struct GalRegion { int16_t first, last; const char *name; };
static const GalRegion GAL_REGION[] = {
  { 1, 151, "KANTO" },
  { 152, 251, "JOHTO" },
  { 252, 386, "HOENN" },
  { 387, 493, "SINNOH" },
};
#define GAL_REGIONS ((int)(sizeof(GAL_REGION) / sizeof(GAL_REGION[0])))
// Pokemon-Suche-Symbol (seit 6.28 auf dem Suchbildschirm, nicht mehr im Pokedex).
// Alles wird an der MITTE der SICHTBAREN Grafik ausgerichtet, nicht an der Mitte
// des Sprite-Rasters (die Grafik sitzt im Raster leicht links).
//  RADAR_ART_X2/Y2 = doppelte Mitte der sichtbaren Grafik in Rasterzellen
//  (Spalten 0..39, Zeilen 1..41 -> 40 bzw. 43; sichtbare Hoehe 41*3 = 123 px)
//  Suchbildschirm: Symbol (123 px) senkrecht mittig auf SEARCH_CY; der Textblock
//  (2 Zeilen, 55 px hoch) steht darueber, seit 6.29 eine Zeile (34 px) hoeher.
#define RADAR_N 43
#define RADAR_SC 3
#define RADAR_ART_X2 40
#define RADAR_ART_Y2 43
#define SEARCH_TXT_Y 100   // Oberkante des Textblocks (seit 6.29 eine Zeile = 34 px hoeher als in 6.28: 134)
#define SEARCH_LINE 34     // Zeilenabstand (Schriftgroesse 3)
#define SEARCH_TXT_H 55    // Hoehe des (immer zweizeilig gerechneten) Textblocks
#define SEARCH_CY 270      // Mitte der sichtbaren Symbolgrafik
#define SEARCH_CY_B 250    // Optik B: Symbol 20 px hoeher (Landschaft darunter)

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *panel = new Arduino_CO5300(
  bus, LCD_RESET, 0 /*rotation*/, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
// Framebuffer completo en PSRAM: dibujamos todo y hacemos flush() (sin parpadeo)
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel);

TouchDrvCST92xx touch;
Pet pet;

// sprite animado de la SD para la especie actual (si existe el archivo)
SdMon mon;          // sprite B/N (respaldo y minijuego si no hay PMD)
PmdMon pmd;         // sprite PMD multi-accion (pantalla principal)
PmdMon evoPmd;      // forma anterior, solo durante el parpadeo de evolucion
int16_t monFor = -2;
bool monShinyFor = false;

// comportamiento del bicho en pantalla
struct {
  uint8_t mode = 0;     // 0 idle, 1 paseo, 2 gesto one-shot
  uint8_t act = PMD_IDLE;
  uint32_t t0 = 0;      // inicio de la animacion en curso
  uint32_t until = 0;   // fin del estado actual
  float x = 233, targetX = 233;
} beh;
#define PET_GROUND 304  // linea de suelo de la mascota
PmdMon galleryPmd;  // sprite grande de la vista detalle de la galeria (PMD/TPK2, legal)
PmdMon favPmd[3];   // los tres favoritos en la portada del pokedex
PmdMon battlePmd;   // sprite del oponente durante un combate
PmdMon tripPmd;      // sprite del pokemon encontrado en un ausflug

// galeria pokedex
bool galleryOpen = false;
bool galleryDirty = false;
bool galleryGoldOnPage = false;  // Optik B: aktuelle Rasterseite hat Goldzellen -> animiert
uint32_t galleryFxLast = 0;
int galleryPage = 0;        // 10 paginas de 16
int16_t galleryDetail = 0;  // dex en vista detalle, 0 = rejilla
bool galleryInfoShown = false;  // en la vista detalle: info en vez de imagen+medallas
uint8_t galleryMedalTapId = 255;  // 255 = ninguna; si no, insignia tocada (indice)
uint32_t galleryMedalTapUntil = 0;

bool screenOff = false;       // pulsacion corta del boton PWR
bool cardOpen = false;        // ficha del bicho (deslizar vertical)
bool kbOpen = false;          // teclado para renombrar al bicho
char nameBuf[24] = "";
uint8_t nameLen = 0;
bool kbLastWasUmlaut = false;  // letzte Eingabe war eine Automatik-Umlaut-Umwandlung (fuer Backspace-Rueckgaengig)
uint8_t cardPage = 0;         // 0 perfil, 1 stats, 2 medallas, 3 progreso, 4 ausflug
bool clockOpen = false;       // pantalla de ajuste de hora (deslizar abajo)
bool clockSavedOpen = false;  // aviso "gespeichert" tras pulsar OK, dura lo
                                // mismo que el corazon antes de volver al
                                // juego
uint32_t clockSavedUntil = 0;
// ===================== Pokemon-Suche (Bluetooth-Treffen) =====================
#define MEET_SERVICE_UUID "6f3a1e2a-6b7c-4f1a-9d3e-0b8a2c5f7a10"
#define MEET_OFFER_UUID   "6f3a1e2b-6b7c-4f1a-9d3e-0b8a2c5f7a10"  // Server-Daten, vom Client gelesen
#define MEET_REQUEST_UUID "6f3a1e2c-6b7c-4f1a-9d3e-0b8a2c5f7a10"  // Client schreibt hier seine Daten hin
#define MEET_ACK_UUID     "6f3a1e2d-6b7c-4f1a-9d3e-0b8a2c5f7a10"  // Bestaetigung: vom Client nach dem
                                                                    // Schreiben gelesen, um echt zu
                                                                    // verifizieren, dass die Verbindung
                                                                    // bis zum Schluss durchgehend stand

#pragma pack(push, 1)
struct MeetPacket {
  int16_t dex;
  uint8_t fullness, joy, energy, hygiene;
  uint8_t magicBerries;
  uint32_t roleRand;
};
#pragma pack(pop)

enum PokeSearchPhase : uint8_t { PS_SEARCHING, PS_SUCCESS, PS_NOBODY, PS_CONN_ERROR, PS_UNKNOWN };
bool pokeSearchOpen = false;
bool tripResultIsMeet = false;   // true = diese tripResultOpen-Anzeige ist die
                                    // Begegnungs-Anzeige eines Treffens (nicht
                                    // eines echten Ausflugs): laeuft automatisch
                                    // nach 5s ab, keine Touch-Interaktion
uint32_t tripResultMeetUntil = 0;
bool meetSearchLive = false;  // wird SOFORT false in meetBleStop(), anders als pokeSearchOpen
                                // (das noch 2,5s fuer die Erfolgs-/Niemand-da-Anzeige offen bleibt)
PokeSearchPhase pokeSearchPhase = PS_SEARCHING;
uint32_t pokeSearchUntil = 0;      // 10s-Timeout der Suche
uint32_t pokeSearchMsgUntil = 0;   // Anzeigedauer der Erfolg/Niemand-Meldung
MeetPacket meetMyPacket, meetPartnerPacket;
volatile bool meetServerGotClientData = false;  // Server: onWrite hat Client-Daten empfangen
volatile bool meetClientDone = false;            // Client: Lesen+Schreiben abgeschlossen
volatile bool meetClientConnecting = false;
volatile bool meetSawPeer = false;  // ein passendes Geraet wurde gesehen, auch
                                      // wenn der Datenaustausch dann scheiterte
BLEServer *meetServer = nullptr;
BLECharacteristic *meetOfferChar = nullptr;
BLECharacteristic *meetRequestChar = nullptr;
BLECharacteristic *meetAckChar = nullptr;
BLEScan *meetScan = nullptr;
BLEClient *meetClient = nullptr;
BLEAdvertisedDevice *meetTargetDevice = nullptr;
bool meetBleActive = false;  // true waehrend BLE fuer die Suche eingeschaltet ist

// Besuch-Rendering (Ein-/Auslauf-Animation)
bool visitWasActive = false;    // fuer Uebergangs-Erkennung (Start/Ende der Besuchszeit)
uint32_t visitAnimStart = 0;    // millis() des Beginns der aktuellen Ein-/Auslauf-Animation
bool visitAnimIsExit = false;   // true = Auslauf-Animation (Besuchsende), false = Einlauf (Besuchsbeginn)
uint32_t visitHostWaitUntil = 0;  // Gastgeber: wartet bis zu diesem millis(), bevor der Besuch einlaeuft
PmdMon visitPartnerPmd;         // Sprite-Daten des Besuchs-Pokemon

uint8_t gBrightNormal = 90;   // einstellbare Normal-Helligkeit im Akkubetrieb
                                // (80/85/90/95/100), per Wisch in der Uhr-
                                // Vorschau verstellbar; USB-Helligkeit bleibt fix
bool clockPeekOpen = false;   // vista previa antes de los ajustes: fondo +
                                // pokemon en movimiento + hora, sin barras ni
                                // nombre. Mantener pulsado abre los ajustes
bool weekdaySetOpen = false;  // ajuste del dia de la semana (hold en la mitad
                                // de abajo de la vista previa)
uint8_t weekdayEdit = 0;      // valor en edicion, copiado de pet.weekday al abrir

bool favSlotChoiceOpen = false;  // eleccion de en que hueco (1/2/3) guardar
int16_t favSlotChooseDex = 0;    // que especie se esta favoriteando
bool favConfirmShown = false;    // "Favorito confirmado" tras elegir hueco
uint32_t favConfirmUntil = 0;
bool pokedexOverviewOpen = false;  // portada del pokedex (stufe + 3 favoritos)

// banner de medalla/hito: cuando empezo a mostrarse el actual (achvKind[0])
// y hasta cuando se queda antes de pasar solo al siguiente
uint32_t achvShowUntil = 0;
uint32_t achvShowStart = 0;  // para forzar 2s minimo antes de poder tocarla

// borrado total: mantener pulsado "Loeschen" en el reloj abre esta pantalla
bool clockDeleteOpen = false;
uint32_t clockDeleteUntil = 0;    // limite de 10s para completar la secuencia
uint8_t clockDeleteProgress = 0;  // 0..6: cuantos numeros lleva confirmados
int8_t clockDeleteDir = 0;        // +1 ascendente, -1 descendente (fijado al primer toque)
bool clockDeleteMarked[6] = { false, false, false, false, false, false };
bool clockDeleteSuccess = false;  // mostrando "datos borrados" antes de reiniciar
uint32_t clockDeleteSuccessUntil = 0;
int clockH = 12, clockM = 0;  // hora en edicion
int32_t clockDeltaMin = 0;    // cuanto se ha desplazado desde que se abrio el
                               // menu (en minutos); se aplica sobre la hora
                               // real ACTUAL al pulsar OK, no sobre una foto
                               // vieja -- si no, se perdian los minutos que
                               // pasan mientras el menu esta abierto

// ausflug/trip: modo contador de pasos, base para futuros encuentros
bool tripOpen = false;
bool tripBerryLost = false;  // se ofrecio una baya durante el ausflug pero
                               // las bolsas ya estaban llenas (max 9)
uint8_t tripBerriesGained = 0;  // bayas conseguidas EN ESTE ausflug (para "Rendezvous": 2)
bool tripEndConfirm = false;  // "End Trip" wurde einmal angetippt: zweiter Tap noetig
uint32_t tripEndConfirmUntil = 0;  // nach dieser Zeit faellt die Bestaetigung zurueck
uint32_t tripSteps = 0;
uint32_t tripNextCheckpoint = 1000;  // proximo multiplo de 1000 pasos a revisar
#define TRIP_MAX_ENC 12
#define MAGIC_BERRY_MAX 5
int16_t tripEncSpecies[TRIP_MAX_ENC];
bool tripEncBerry[TRIP_MAX_ENC];
bool tripEncNew[TRIP_MAX_ENC];
uint8_t tripEncCount = 0;
bool tripResultOpen = false;   // pantalla de resultados al terminar el ausflug
uint8_t tripResultIndex = 0;   // cual encuentro se esta mostrando (0 = ninguno)
uint32_t tripResultUntil = 0;  // solo para el caso "niemanden getroffen": se cierra sola
bool tripBerryChoiceOpen = false;  // eligiendo que rellena la baya magica
bool berryResultOpen = false;      // mostrando el aviso tras comer la baya
uint8_t berryResultWhich = 0;
uint32_t berryResultUntil = 0;

bool tripChoiceOpen = false;   // pantalla "junto / solo" antes de un ausflug
bool tripWarningOpen = false;  // aviso tras detectar un apagon completo
                                 // durante un ausflug de pasos; se cierra
                                 // solo con un toque
bool tripSoloSadOpen = false;  // aviso "triste" al recuperar antes de tiempo
uint32_t tripSoloSadUntil = 0;
bool tripSoloResultOpen = false;   // resultado tras recuperar el pokemon solo
uint8_t tripSoloResultEvent = 0;   // 0-9
int16_t tripSoloResultSpecies = 0;  // especie encontrada (eventos 1-4), 0 si ninguna
bool tripSoloResultNew = false;
bool tripSoloResultBerryLost = false;  // baya perdida por bolsas llenas (evento 1)
uint8_t tripSoloResultStage = 0;   // 0="encontraste a X"+NEU, 1=texto del evento, 2=baya perdida
bool tripSoloConfirm = false;   // "Pokemon zurueckrufen" tocado una vez: hace falta un segundo toque
uint32_t tripSoloConfirmUntil = 0;

// escena de bano: espuma sobre el bicho y limpieza al reventar
uint32_t bathUntil = 0;
bool bathPending = false;
struct { int16_t x, y; uint8_t r, ph; } bubbles[14];
uint32_t feedMenuUntil = 0;   // selector de comida abierto hasta este millis
// Optik (seit 6.0, experimentell): 0 = A (klassisch), 1 = B (neu). Nur Zeichnen,
// die Spielfunktion haengt nie davon ab. Gespeichert unter "look".
uint8_t gLook = 0;
#define LOOK_B (gLook == 1)
uint32_t eggHeartAt[3] = { 0, 0, 0 };  // Optik B: Zeitpunkte der letzten Ei-Taps (je ein steigendes Herz)
uint8_t eggHeartIdx = 0;
uint8_t eatItem = 0;          // zuletzt gefuettert: 0 Essen, 1/2 Beere, 3 Suessigkeit (fuer Optik B)
uint32_t sleepMsgUntil = 0;   // aviso breve del boton dormir/luz (que paso)
StrId sleepMsgId = S_HAPPY;   // que texto mostrar mientras sleepMsgUntil este activo

// minijuego "toques": mantener la pokeball en el aire
bool gameOpen = false;
uint32_t gameOverUntil = 0;
float ballX, ballY, ballVX, ballVY, gamePetX;
uint8_t gameScore;
bool gameLaunched = false;  // la bola sigue posada en la pala hasta el primer toque
bool gameGoingUp = false;   // fase actual: subiendo recta o cayendo en picado
float gameTargetX = 233;    // adonde se dirige en la bajada (curva suave hacia aqui)
float gameAscentX = 233;    // desde donde arranco la subida actual (para que la
                             // curva no salte si no fue desde el centro)
float hitX, hitY;             // ultimo golpe (anillo de impacto)
uint32_t hitTime = 0;

// saco de entrenamiento (entrena la fuerza)
bool sackOpen = false;
uint32_t sackUntil = 0, sackOverUntil = 0;
uint16_t sackHits = 0;
#define TAP_MIN_GAP 70  // Mindestabstand zwischen zwei angenommenen Taps (ms): Ei, Knoepfe, abweisende Toene
uint32_t tapLastAt = 0;  // Zeitpunkt des letzten angenommenen Taps (Hauptbildschirm usw.): gegen Doppel-Taps durch Prellen beim Abheben
uint32_t sackLastHit = 0;  // Zeitpunkt des letzten gezaehlten Schlags (Training: Mindestabstand)
// Messung fuers Training (Serial Monitor, 115200): Zeit pro Bild und groesste Luecke zwischen zwei Bildern
uint32_t sackStFrames = 0, sackStRenderSum = 0, sackStRenderMax = 0, sackStGapMax = 0, sackStLastStart = 0, sackStIrq0 = 0, sackStPolls = 0, sackStPollGapMax = 0, sackStLastPoll = 0;
float sackShake = 0;
uint8_t sackGain = 0;
uint8_t gameSpdGain = 0;
bool gameNewHi = false;  // record del minijuego de la pelota (antes del saco)

// combate (battle): 5 rondas automaticas contra un rival generado al azar
bool battleOpen = false;
bool battleOver = false;
bool battleWon = false;
uint8_t battleDefGain = 0;
int16_t battleOppDex = 0;
uint8_t battleOppLevel = 1;
uint16_t battleOppAtk = 0, battleOppDef = 0, battleOppSpe = 0;
uint16_t battleOppHp = 0, battleOppMaxHp = 0;
uint16_t battlePlayerHp = 0, battlePlayerMaxHp = 0;
uint8_t battleRound = 0;   // 0..4 (5 rondas en total)
uint8_t battleAttacksInRound = 0;  // 0 o 1; a los 2 (uno de cada lado) se cierra la ronda
uint8_t battleTurn = 0;    // 0 = ataca nuestro bicho, 1 = ataca el rival
uint8_t battleLastHit = 0; // resultado del ultimo golpe: 0 fallo, 1 flojo, 2 fuerte
uint8_t battleBlockedHits = 0;  // veces que el rival fallo contra nosotros
                                  // en este combate (para el nuevo bono de DEF)
uint8_t battlePhase = 0;   // 0 pausa, 1 proyectil volando, 2 impacto/reaccion, 3 fin de turno, 4 resultado
uint32_t battlePhaseUntil = 0;
float battleBallX = 0, battleBallY = 0;
#ifndef BATTLE_SHOT_START
#define BATTLE_SHOT_START 30   // Startversatz des Geschosses in Richtung Gegner (px)
#endif

// las 9 especies con sprite propio en flash (respaldo sin SD): dex -> indice
int flashIdxForDex(int16_t dex) {
  static const int8_t IDX[10] = { -1, 3, 4, 5, 0, 1, 2, 6, 7, 8 };
  return (dex >= 1 && dex <= 9) ? IDX[dex] : -1;
}

#define CX 233  // centro de la pantalla redonda
#define CY 233
#define PET_CY 202  // centro vertical del sprite

static const uint16_t INK_K = 0x18C4;  // spriteColor('k')

// botones de icono siguiendo el arco inferior de la pantalla redonda
// (los exteriores van mas altos para no salirse del circulo)
struct Btn {
  int16_t cx, cy;
  const char *const *icon;
};
Btn buttons[4] = {
  { 140, 390, SPR_ICON_FOOD },   // comer
  { 202, 404, SPR_ICON_PLAY },   // jugar
  { 264, 404, SPR_ICON_LIGHT },  // luz
  { 326, 390, SPR_ICON_CLEAN },  // bano
};
#define BTN_HALF 26  // boton de 52x52
#define BTN_HIT 36   // radio tactil (un poco mas generoso)
// Optik B: groessere Kacheln (60x60), Symbole x3, gleiche Reihenfolge/Funktion
Btn buttonsB[4] = {
  { 134, 393, SPR_ICONB_FOOD },   // comer
  { 200, 405, SPR_ICONB_PLAY },   // jugar (Pokeball)
  { 266, 405, SPR_ICONB_LIGHT },  // luz
  { 332, 393, SPR_ICONB_CLEAN },  // bano
};
#define BTN_HALF_B 30
#define BTN_HIT_B 40
int btnCx(int i) { return LOOK_B ? buttonsB[i].cx : buttons[i].cx; }
int btnCy(int i) { return LOOK_B ? buttonsB[i].cy : buttons[i].cy; }
int btnHit() { return LOOK_B ? BTN_HIT_B : BTN_HIT; }
int btnHalf() { return LOOK_B ? BTN_HALF_B : BTN_HALF; }

// grietas del huevo (pixeles 'k' sobre el sprite)
static const uint8_t CRACK1[][2] = { {15,8},{16,9},{15,10} };
static const uint8_t CRACK2[][2] = { {11,13},{12,14},{11,15},{20,12},{19,13},{20,14} };
// estrellas del modo noche
static const uint16_t STARS[][2] = { {120,140},{330,120},{370,210},{95,230},{280,90},{160,95} };

bool wasPressed = false;
// eleccion de inicial (primera partida): Bulbasaur / Charmander / Squirtle, 3 filas
static const int16_t STARTER_DEX[3] = { 1, 4, 7 };
#define STARTER_ROW_Y 110
#define STARTER_ROW_H 70
#define STARTER_ROW_GAP 8
// boton-CTA de evolucion (centrado, mitad de pantalla)
#define EVO_BTN_W 256
#define EVO_BTN_H 64
#define EVO_BTN_X (CX - EVO_BTN_W / 2)
#define EVO_BTN_Y 172
// boton-CTA de despedida (mas ancho: lleva el nombre + frase)
#define FAR_BTN_W 408
#define FAR_BTN_H 58
#define FAR_BTN_X (CX - FAR_BTN_W / 2)
#define FAR_BTN_Y 176
// el CST9217 avisa por el pin INT cuando hay datos tactiles; lo usamos para no
// leer el bus I2C mientras el chip esta dormido (esa lectura se colgaba ~1s)
volatile bool gTouchIrq = false;
volatile uint32_t gTouchIrqCount = 0;  // nur fuer die Training-Messung (Serial Monitor)
void IRAM_ATTR touchIsr() { gTouchIrq = true; gTouchIrqCount = gTouchIrqCount + 1; }
uint32_t lastRender = 0;
// proteccion del AMOLED: atenuado por inactividad
uint32_t lastInteract = 0;
bool screenDark = false;     // true tras 90s sin tocar: pantalla a 0, sin renderizar
bool swallowGesture = false; // el toque que despierta no acciona nada
uint32_t holdStart = 0;     // pulsacion larga sobre el bicho
uint32_t confirmUntil = 0;  // dialogo "soltar?" activo hasta este millis
uint32_t sleepConfirmUntil = 0;  // "Pokemon schlafen schicken?" (20-22h), 10s Timeout
bool releaseDenied = false;  // si true, el mismo recuadro blanco muestra el
                               // aviso de limite diario en vez de SI/NO
uint8_t choiceKind = 0;     // dialogo de decision: 0 ninguno, 1 evolucion, 2 despedida
uint32_t choiceUntil = 0;   // se cierra solo a este millis
int16_t tX0, tY0, tXl, tYl; // gesto en curso (inicio y ultima posicion)
uint32_t tStart = 0;
bool holdFired = false;

// ---------- Pokemon-Suche: Bluetooth-Implementierung ----------
// Angelehnt an das offizielle Espressif-Beispiel "BLE Client and Server
// Coexistence" (arduino-esp32, libraries/BLE/examples/Client_Server):
// beide Geraete laufen gleichzeitig als Server (bewerben den eigenen
// Dienst) UND als Client (suchen nach demselben Dienst bei anderen).
// Wer zuerst den anderen findet, verbindet sich aktiv als Client; das
// gefundene Geraet bleibt einfach Server. So braucht keines der beiden
// Geraete im Voraus eine feste Rolle.

class MeetServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override {}
  void onDisconnect(BLEServer *s) override {
    // NUR neu starten, wenn die eigene Suche noch WIRKLICH aktiv ist.
    // meetSearchLive wird sofort in meetBleStop() false, anders als
    // pokeSearchOpen (das fuer die Erfolgs-/Niemand-da-Anzeige noch 2,5s
    // laenger offen bleibt) -- dieser Callback feuert asynchron und kann
    // sonst in genau diesem Zeitfenster noch faelschlich neu starten
    if (meetSearchLive) BLEDevice::startAdvertising();
  }
};

class MeetRequestCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String v = c->getValue();
    if (v.length() == sizeof(MeetPacket)) {
      memcpy(&meetPartnerPacket, v.c_str(), sizeof(MeetPacket));
      meetServerGotClientData = true;
    }
  }
};

class MeetAdvCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (meetClientConnecting || meetClientDone) return;
    if (dev.haveServiceUUID() && dev.isAdvertisingService(BLEUUID(MEET_SERVICE_UUID))) {
      meetSawPeer = true;
      // Deterministischer Tiebreaker: beide Geraete entdecken sich normalerweise
      // GLEICHZEITIG und wuerden sonst beide gleichzeitig als Client verbinden
      // wollen (Wettlauf, nur eine Seite kam bisher zuverlaessig durch). Nur
      // die Seite mit der "groesseren" MAC-Adresse verbindet sich aktiv; die
      // andere bleibt Server und wartet auf die eingehende Verbindung
      String myAddr = BLEDevice::getAddress().toString().c_str();
      String otherAddr = dev.getAddress().toString().c_str();
      if (myAddr <= otherAddr) {
        // wir warten, die andere Seite verbindet sich zu uns -- eigenen Scan
        // trotzdem stoppen, damit der Funk ungestoert auf die eingehende
        // Verbindung reagieren kann, statt weiter im Scan-Modus zu bleiben
        BLEDevice::getScan()->stop();
        return;
      }
      BLEDevice::getScan()->stop();
      meetTargetDevice = new BLEAdvertisedDevice(dev);
      meetClientConnecting = true;
    }
  }
};

// startet Server (mit eigenem Angebot vorbefuellt) + Advertising
static void meetSetupServer() {
  meetServer = BLEDevice::createServer();
  meetServer->setCallbacks(new MeetServerCallbacks());
  BLEService *svc = meetServer->createService(MEET_SERVICE_UUID);
  meetOfferChar = svc->createCharacteristic(MEET_OFFER_UUID, BLECharacteristic::PROPERTY_READ);
  meetOfferChar->setValue((uint8_t *)&meetMyPacket, sizeof(MeetPacket));
  meetRequestChar = svc->createCharacteristic(MEET_REQUEST_UUID, BLECharacteristic::PROPERTY_WRITE);
  meetRequestChar->setCallbacks(new MeetRequestCallbacks());
  meetAckChar = svc->createCharacteristic(MEET_ACK_UUID, BLECharacteristic::PROPERTY_READ);
  uint8_t ackVal = 0xAA;
  meetAckChar->setValue(&ackVal, 1);
  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(MEET_SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
}

static void meetSetupClient() {
  meetScan = BLEDevice::getScan();
  meetScan->setAdvertisedDeviceCallbacks(new MeetAdvCallbacks());
  meetScan->setActiveScan(true);
  meetScan->setInterval(100);
  meetScan->setWindow(99);
}

// wird von pokeSearchTick() aufgerufen, sobald meetClientConnecting gesetzt
// wurde: verbindet, liest das Angebot des Servers, schreibt das eigene
// Angebot zurueck, trennt sofort wieder
static bool meetConnectAndExchange() {
  if (meetClient == nullptr) {
    meetClient = BLEDevice::createClient();
  } else if (meetClient->isConnected()) {
    // ein vorheriges disconnect() lief noch im Hintergrund nach (laut
    // mehreren Espressif-Issues ist das Trennen asynchron) -- vor einem
    // neuen Verbindungsversuch erst sauber trennen und kurz abwarten
    meetClient->disconnect();
    delay(150);
  }
  bool ok = meetClient->connect(meetTargetDevice);
  if (ok) {
    BLERemoteService *rs = meetClient->getService(BLEUUID(MEET_SERVICE_UUID));
    if (rs == nullptr) { ok = false; }
    else {
      BLERemoteCharacteristic *offerRc = rs->getCharacteristic(BLEUUID(MEET_OFFER_UUID));
      BLERemoteCharacteristic *reqRc = rs->getCharacteristic(BLEUUID(MEET_REQUEST_UUID));
      BLERemoteCharacteristic *ackRc = rs->getCharacteristic(BLEUUID(MEET_ACK_UUID));
      if (offerRc == nullptr || reqRc == nullptr || ackRc == nullptr) { ok = false; }
      else {
        String offerVal = offerRc->readValue();
        if (offerVal.length() != sizeof(MeetPacket)) { ok = false; }
        else {
          memcpy(&meetPartnerPacket, offerVal.c_str(), sizeof(MeetPacket));
          reqRc->writeValue((uint8_t *)&meetMyPacket, sizeof(MeetPacket), true);
          // Bestaetigung: erst wenn dieses Rueck-Lesen ebenfalls klappt, stand
          // die Verbindung wirklich bis zum Schluss -- erst DANN gilt der
          // Austausch als wirklich erfolgreich (statt sich nur auf das
          // Gelingen von writeValue() selbst zu verlassen)
          String ackVal = ackRc->readValue();
          if (ackVal.length() != 1 || (uint8_t)ackVal[0] != 0xAA) ok = false;
        }
      }
    }
    meetClient->disconnect();
    delay(150);  // Abklingzeit, bevor dieser Client fuer einen weiteren Versuch wiederverwendet wird
  }
  delete meetTargetDevice;  // Speicherleck vermeiden: wurde in onResult() frisch angelegt
  meetTargetDevice = nullptr;
  return ok;
}

// BLEScan::start(dauer, false) blockiert den aufrufenden Thread bis zum
// Ablauf der Dauer (offiziell dokumentiertes Verhalten der Bibliothek).
// Damit der Hauptbildschirm waehrend der Suche weiter gezeichnet wird,
// laeuft der Suchlauf in einem eigenen, kleinen FreeRTOS-Task
static volatile bool meetScanTaskRunning = false;
static void meetScanTaskFn(void *param) {
  if (meetScan) meetScan->start(10, false);
  meetScanTaskRunning = false;
  vTaskDelete(nullptr);
}
static void meetStartScanTask() {
  if (meetScanTaskRunning) return;
  meetScanTaskRunning = true;
  xTaskCreate(meetScanTaskFn, "meetScan", 4096, nullptr, 1, nullptr);
}

// einschalten fuer die Dauer der Suche. WICHTIG: BLEDevice::deinit()
// gefolgt von einem erneuten BLEDevice::init() ist in der ESP32-Bibliothek
// selbst nachweislich instabil (zahlreiche dokumentierte Abstuerze, siehe
// z.B. espressif/arduino-esp32 Issues #5971, #7990, #3335 -- ein bekanntes,
// bis heute ungeloestes Problem der Bibliothek, kein Fehler in unserem
// Code). Deshalb wird der Bluetooth-Stack nur EINMALIG initialisiert und
// danach nie wieder deinitialisiert; zwischen zwei Suchen werden nur
// Advertising und Scan gestoppt (siehe meetBleStop()), nicht der ganze
// Stack. Das kostet etwas mehr Ruhestrom als ein vollstaendiges
// Abschalten, ist aber die zuverlaessige Variante
static void meetBleStart() {
  meetMyPacket.dex = pet.isEgg() ? 0 : pet.speciesId;
  meetMyPacket.fullness = pet.fullness;
  meetMyPacket.joy = pet.joy;
  meetMyPacket.energy = pet.energy;
  meetMyPacket.hygiene = pet.hygiene;
  meetMyPacket.magicBerries = pet.magicBerries;
  meetMyPacket.roleRand = esp_random();
  meetServerGotClientData = false;
  meetSawPeer = false;
  meetClientDone = false;
  meetClientConnecting = false;
  meetTargetDevice = nullptr;
  if (!meetBleActive) {
    BLEDevice::init("TamaPoke");
    meetSetupServer();
    meetSetupClient();
    meetBleActive = true;
  } else {
    meetOfferChar->setValue((uint8_t *)&meetMyPacket, sizeof(MeetPacket));
    BLEDevice::startAdvertising();
  }
  meetSearchLive = true;
  meetStartScanTask();
}

// nur Advertising+Scan stoppen -- der Bluetooth-Stack selbst bleibt
// initialisiert (siehe Begruendung oben bei meetBleStart())
static void meetBleStop() {
  meetSearchLive = false;  // SOFORT, bevor irgendetwas anderes passiert -- schliesst
                             // das Zeitfenster, in dem ein verzoegert feuernder
                             // Trennungs-Callback das Advertising faelschlich neu starten koennte
  if (!meetBleActive) return;
  if (meetScan) meetScan->stop();
  if (meetClient && meetClient->isConnected()) meetClient->disconnect();
  BLEDevice::stopAdvertising();
}

// startet den Besuch, nachdem der Datenaustausch erfolgreich war
static void startVisitFromMeet() {
  int16_t sp = meetPartnerPacket.dex;
  if (sp < 1 || sp > DEX_COUNT) return;  // ungueltiges Paket: sicherheitshalber abbrechen
  bool wasNew = !pet.isRegistered(sp);
  if (wasNew) pet.registerSeen(sp);
  if (pet.dexEncounters[sp] < 65535) pet.dexEncounters[sp]++;

  pet.visitPartnerDex = sp;
  pet.visitPartnerFullness = meetPartnerPacket.fullness;
  pet.visitPartnerJoy = meetPartnerPacket.joy;
  pet.visitPartnerEnergy = meetPartnerPacket.energy;
  pet.visitPartnerHygiene = meetPartnerPacket.hygiene;
  pet.visitPartnerWasNew = wasNew;
  pet.visitIsHost = (meetPartnerPacket.roleRand > meetMyPacket.roleRand);  // hoehere Zahl = Besucher: ist PARTNER hoeher, besucht er UNS -> wir sind Gastgeber
  pet.visiting = true;
  uint32_t visitBase = pet.lastSeenEpoch ? pet.lastSeenEpoch : rtcEpoch();
  // Besucher-Seite: Ausflug dauert 5s laenger, damit die Rueckkehr dort
  // nicht mehr vor dem Gastgeber-Ende passiert (siehe Zeitablauf-Vorgabe)
  pet.visitEndEpoch = visitBase + (pet.visitIsHost ? 180UL : 185UL);

  // Zauberbeeren-Austausch: hat eine Seite das Maximum UND die andere
  // weniger als 4, schenkt die volle Seite eine Beere
  pet.visitBerryDelta = 0;
  if (pet.magicBerries >= MAGIC_BERRY_MAX && meetPartnerPacket.magicBerries < 4) {
    pet.magicBerries--;
    pet.visitBerryDelta = -1;
  } else if (meetPartnerPacket.magicBerries >= MAGIC_BERRY_MAX && pet.magicBerries < 4) {
    if (pet.magicBerries < MAGIC_BERRY_MAX) pet.magicBerries++;
    pet.visitBerryDelta = 1;
  }

  // Streiten statt Spielen: 20% Chance, aus einer Kombination beider
  // ausgetauschten Zufallszahlen (symmetrisch, beide Seiten kommen auf
  // dasselbe Ergebnis) -- bei einem Beeren-Geschenk immer 0% (nur Spielen)
  bool berryGifted = (pet.visitBerryDelta != 0);
  uint32_t combined = meetMyPacket.roleRand ^ meetPartnerPacket.roleRand;
  pet.visitFighting = !berryGifted && ((combined % 100) < 20);
  if (pet.visitFighting) {
    // Streit: Halbierung passiert jetzt gleich zu Beginn, nicht erst am
    // Ende -- sonst koennte man die Strafe durch Ausschalten umgehen
    pet.fullness = pet.fullness / 2;
    pet.joy = pet.joy / 2;
    pet.energy = pet.energy / 2;
    pet.hygiene = pet.hygiene / 2;
  }

  pet.requestSave();
  visitWasActive = false;  // erzwingt die Einlauf-Animation beim naechsten Aufruf des Hauptbildschirms
  if (pet.visitIsHost) visitPartnerPmd.load(sp, false);

  // Begegnungs-Anzeige wie beim Ausflug
  tripEncSpecies[0] = sp;
  tripEncNew[0] = wasNew;
  tripEncBerry[0] = (pet.visitBerryDelta != 0);
  tripEncCount = 1;
  tripResultIndex = 0;
  tripBerryLost = false;
  tripPmd.load(sp, false);
  tripResultOpen = true;
  tripResultIsMeet = true;
  tripResultMeetUntil = millis() + 5000;
  sfxPlay(wasNew ? SFX_BATTLE_WIN : SFX_HEART);
}

// treibt Suche/Ergebnis-Anzeige voran; wird aus loop() aufgerufen
void pokeSearchTick() {
  if (!pokeSearchOpen) return;
  if (pokeSearchPhase == PS_SEARCHING) {
    if (meetClientConnecting && !meetClientDone) {
      if (meetConnectAndExchange()) {
        meetClientDone = true;
      } else {
        meetClientConnecting = false;
        meetTargetDevice = nullptr;
        meetStartScanTask();
      }
    }
    if (meetServerGotClientData || meetClientDone) {
      meetBleStop();
      int16_t psp = meetPartnerPacket.dex;
      if (psp < 1 || psp > DEX_COUNT) {
        // Partner ist ein Pokemon aus einer Generation, die diese Firmware
        // (noch) nicht kennt: Meldung + Ablehnungston, kein Besuch
        pokeSearchPhase = PS_UNKNOWN;
        pokeSearchMsgUntil = millis() + 2500;
        sfxPlay(SFX_DENY);
        return;
      }
      pokeSearchPhase = PS_SUCCESS;
      pokeSearchMsgUntil = millis() + 2500;
      return;
    }
    if (millis() >= pokeSearchUntil) {
      meetBleStop();
      pokeSearchPhase = meetSawPeer ? PS_CONN_ERROR : PS_NOBODY;
      pokeSearchMsgUntil = millis() + 2500;
      sfxPlay(SFX_DENY);
      return;
    }
  } else if (pokeSearchPhase == PS_NOBODY || pokeSearchPhase == PS_CONN_ERROR || pokeSearchPhase == PS_UNKNOWN) {
    if (millis() >= pokeSearchMsgUntil) pokeSearchOpen = false;  // zurueck zum Hauptbildschirm
  } else if (pokeSearchPhase == PS_SUCCESS) {
    if (millis() >= pokeSearchMsgUntil) {
      pokeSearchOpen = false;
      galleryOpen = false;
      startVisitFromMeet();
    }
  }
}

void drawGameScene();  // prototipo (definida mas abajo)
void drawTripSceneB(uint8_t biome);  // Prototyp: Landschaft fuer Ausflug-/Such-/Trainingsseiten (Optik B)
int sceneHour();  // prototipo (definida mas abajo)

// Radar-Symbol mit je drei Wellenboegen links/rechts (duenne 1-Pixel-Segmente).
// rcy = senkrechte Mitte der SICHTBAREN Grafik
void drawRadarSymbol(int rcx, int rcy) {
  drawMap(SPR_RADAR, RADAR_N, rcx - (RADAR_ART_X2 * RADAR_SC) / 2,
          rcy - (RADAR_ART_Y2 * RADAR_SC) / 2, RADAR_SC, false);
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  for (int side = -1; side <= 1; side += 2) {
    for (int wv = 0; wv < 3; wv++) {
      int rad = 85 + wv * 18;
      float aMid = (side < 0) ? (float)PI : 0.0f;
      float aSpan = 0.95f;
      int steps = 8;
      int px = -1, py = -1;
      for (int st = 0; st <= steps; st++) {
        float a = aMid - aSpan / 2 + aSpan * st / steps;
        int qx = rcx + (int)(cosf(a) * rad), qy = rcy + (int)(sinf(a) * rad);
        if (px >= 0) gfx->drawLine(px, py, qx, qy, ink);
        px = qx; py = qy;
      }
    }
  }
}

// Suchbildschirm (seit 6.28): Text ueber dem Radar-Symbol (seit 6.29 eine Zeile hoeher). Das Symbol bleibt bei allen Meldungen an derselben Stelle;
// einzeilige Meldungen stehen mittig im (zweizeilig gerechneten) Textblock.
void renderPokeSearch() {
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  if (LOOK_B) drawTripSceneB(pet.isEgg() ? 0 : biomeFor(pet.speciesId));  // Optik B: Landschaft des Pokemon, 120 px tiefer
  else drawGameScene();
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  gfx->setTextColor(ink);
  gfx->setTextSize(3);
  const char *l1, *l2;
  if (pokeSearchPhase == PS_SEARCHING) { l1 = T(S_POKE_SEARCH1); l2 = T(S_POKE_SEARCH2); }
  else if (pokeSearchPhase == PS_SUCCESS) { l1 = T(S_POKE_SUCCESS1); l2 = T(S_POKE_SUCCESS2); }
  else if (pokeSearchPhase == PS_CONN_ERROR) { l1 = T(S_POKE_CONN_ERROR); l2 = nullptr; }
  else if (pokeSearchPhase == PS_UNKNOWN) { l1 = T(S_POKE_UNKNOWN1); l2 = T(S_POKE_UNKNOWN2); }
  else { l1 = T(S_POKE_NOBODY); l2 = nullptr; }
  if (l2) {
    gfx->setCursor(CX - (int)strlen(l1) * 9, SEARCH_TXT_Y);
    gfx->print(l1);
    gfx->setCursor(CX - (int)strlen(l2) * 9, SEARCH_TXT_Y + SEARCH_LINE);
    gfx->print(l2);
  } else {
    gfx->setCursor(CX - (int)strlen(l1) * 9, SEARCH_TXT_Y + (SEARCH_TXT_H - 21) / 2);
    gfx->print(l1);
  }
  drawRadarSymbol(CX, LOOK_B ? SEARCH_CY_B : SEARCH_CY);
  gfx->flush();
}

// prueft, ob die 3-minuetige Besuchszeit abgelaufen ist; wird aus loop() aufgerufen
// laesst ein Pokemon zwischen (baseX-rangeHalf) und (baseX+rangeHalf) hin und
// her wandern, mit kurzen Idle-Pausen an den Umkehrpunkten fuer Abwechslung.
// seed verschiebt die Phase, damit zwei Pokemon nicht synchron laufen
// Streit-Darstellung: wechselt je Zyklus zufaellig zwischen Vorstuermen
// (verfolgen, auf den Gegner zu) und Zurueckweichen (vom Gegner weg), mit
// kurzer Angriffs-/Treffer-Pause am Ende jeder Bewegung -- "ueberhaeufen
// sich mit Angriffen", aber nicht mehr gleichfoermig an fester Position.
// facingRight: true = der Gegner steht rechts (eigenes Pokemon links),
// false = der Gegner steht links (Besuchs-Pokemon rechts)
void drawFightingPet(PmdMon &m, int baseX, int groundY, uint32_t seed, bool facingRight) {
  if (!m.loaded) return;
  const uint32_t period = 2200, moveMs = 900;
  uint32_t t = millis() + seed;
  uint32_t cycleIdx = t / period;
  uint32_t phase = t % period;
  bool advancing = ((cycleIdx * 2654435761u + seed) % 2) == 0;  // je Zyklus neu, deterministisch
  int dir = facingRight ? 1 : -1;
  int moveAmt = (advancing ? dir : -dir) * 18;
  int x;
  uint8_t act;
  if (phase < moveMs) {
    float p = (float)phase / moveMs;
    x = baseX + (int)(moveAmt * p);
    bool walkRight = (moveAmt > 0);
    act = walkRight ? (m.has(PMD_WALKR) ? PMD_WALKR : PMD_IDLE)
                     : (m.has(PMD_WALKL) ? PMD_WALKL : PMD_IDLE);
  } else {
    x = baseX + moveAmt;
    if (phase < moveMs + 350 && m.has(PMD_HURT)) act = PMD_HURT;
    else act = m.has(PMD_ATTACK) ? PMD_ATTACK : PMD_IDLE;
  }
  drawPmdActM(m, act, x, groundY, millis(), true, false, 4, false);
}

void drawWanderingPet(PmdMon &m, int baseX, int rangeHalf, int groundY, uint32_t seed) {
  if (!m.loaded) return;
  const uint32_t period = 5000;
  uint32_t t = millis() + seed;
  uint32_t roundIdx = t / period;  // welche Hin-und-Her-Runde gerade laeuft
  uint32_t phase = t % period;
  float cyc = (float)phase / period;              // 0..1 ueber eine volle Hin-und-Her-Runde
  bool movingRight = cyc < 0.5f;
  float half = movingRight ? cyc * 2.0f : (1.0f - cyc) * 2.0f;  // 0..1 innerhalb der Haelfte
  int x = baseX - rangeHalf + (int)(half * 2 * rangeHalf);
  bool pausingStart = phase < 500;
  bool pausingMid = phase > period / 2 - 250 && phase < period / 2 + 250;
  bool pausingEnd = phase > period - 500;
  uint8_t act;
  if (pausingStart || pausingMid || pausingEnd) {
    // je Pause-Vorkommen (nicht jeder Frame) eine zufaellige Ruhe-Animation
    // waehlen, kein Schlafen dabei
    static const uint8_t pool[] = { PMD_IDLE, PMD_POSE, PMD_HOP, PMD_NOD, PMD_BREATH, PMD_SIT };
    uint32_t pauseId = roundIdx * 3 + (pausingStart ? 0 : pausingMid ? 1 : 2) + seed;
    act = PMD_IDLE;
    for (uint8_t k = 0; k < 6; k++) {
      uint8_t cand = pool[(pauseId + k) % 6];
      if (m.has(cand)) { act = cand; break; }
    }
  } else {
    act = movingRight ? (m.has(PMD_WALKR) ? PMD_WALKR : PMD_IDLE)
                       : (m.has(PMD_WALKL) ? PMD_WALKL : PMD_IDLE);
  }
  drawPmdActM(m, act, x, groundY, millis(), true, false, 4, false);
}

void visitTick() {
  if (!pet.visiting) return;
  uint32_t nowE = rtcEpoch();
  if (nowE && (!pet.lastSeenEpoch ||
               (nowE > pet.lastSeenEpoch ? nowE - pet.lastSeenEpoch : pet.lastSeenEpoch - nowE) <= 600UL)) {
    pet.lastSeenEpoch = nowE;  // waehrend des Besuchs zeitnah nachfuehren (nicht erst
                                 // auf die alle-30s-Hintergrund-Synchronisierung warten)
  }
  if (!pet.lastSeenEpoch || pet.lastSeenEpoch < pet.visitEndEpoch) return;
  if (!pet.visitFighting) {
    // Halbierung beim Streit ist bereits zu Beginn des Treffens passiert
    // (siehe startVisitFromMeet()); hier also nur noch das friedliche
    // Treffen: Uebernahme der jeweils hoeheren Werte
    if (pet.visitPartnerFullness > pet.fullness) pet.fullness = pet.visitPartnerFullness;
    if (pet.visitPartnerJoy > pet.joy) pet.joy = pet.visitPartnerJoy;
    if (pet.visitPartnerEnergy > pet.energy) pet.energy = pet.visitPartnerEnergy;
    if (pet.visitPartnerHygiene > pet.hygiene) pet.hygiene = pet.visitPartnerHygiene;
  }
  pet.visiting = false;
  pet.requestSave();
  sfxPlay(SFX_MEDAL);  // Fanfare
  if (screenDark) lastInteract = millis();  // weckt wie ein Tap, falls auf 0 abgedimmt
  if (screenOff) { screenOff = false; lastInteract = millis(); }  // auch bei komplett ausgeschaltetem Display: Abschieds-/Rueckkehr-Animation soll sichtbar sein
  visitAnimIsExit = true;
  visitAnimStart = millis();
}

void setup() {
  Serial.setRxBufferSize(8192);  // la transferencia a SD llega en bloques de 2 KB
  Serial.begin(115200);
  // CRITICO: sin esto, Serial.print BLOQUEA el juego cuando no hay un
  // monitor serie abierto en el host (el bufer TX del USB CDC se llena
  // y nadie lo vacia) -> con timeout 0 los mensajes se descartan
  Serial.setTxTimeoutMs(0);
  Serial.printf("TamaPoke fw v%s\n", FW_VERSION);
  loadLang();  // idioma guardado (ES por defecto)
  loadLook();  // Optik A/B
  {
    Preferences p;
    p.begin("tamapoke", true);
    uint8_t v = p.getUChar("brightn", 90);
    p.end();
    gBrightNormal = (v >= 75 && v <= 100 && (v - 75) % 5 == 0) ? v : 90;
  }
  Wire.begin(IIC_SDA, IIC_SCL);
  // CST9217 (tactil), AXP2101 (PMU) y PCF85063 (RTC) comparten este bus I2C.
  // Red de seguridad para PMU/RTC (SensorLib NO respeta este timeout en el
  // tactil; el cuelgue del tactil dormido se resuelve gateando por INT, ver
  // handleTouch).
  Wire.setTimeOut(50);

  // CRITICO: encender la alimentacion del panel (BLDO1=OLED VDD 3.3V) ANTES de
  // inicializar el display. Si el PMU se reseteo (drenaje total), este rail
  // queda OFF y la pantalla se ve negra aunque el resto de la placa funcione.
  pmuEnablePanel();
  delay(30);  // deja que el rail 3.3V se estabilice antes de hablarle por QSPI
              // (posible causa de que a veces haga falta pulsar PWR dos veces
              // tras un apagado total: el primer intento de gfx->begin()
              // corria antes de que el rail estuviera realmente estable)

  // QSPI a 80MHz (por defecto 40): el flush del framebuffer es el cuello de
  // botella del fps (~56ms a 40MHz). Si el panel mostrara basura, bajar a 40M.
  if (!gfx->begin(80000000)) Serial.println("gfx->begin() fallo");
  panel->setBrightness(180);

  touch.setPins(TP_RESET, TP_INT);
  bool touchOk = false;
  for (int i = 0; i < 3 && !touchOk; i++) {  // a veces falla al primer intento
    touchOk = touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL);
    if (!touchOk) delay(150);
  }
  if (!touchOk) Serial.println("CST9217 no detectado");
  // begin() deja el chip en modo comando (lee la identidad y no sale);
  // hace falta un reset por hardware para que vuelva a reportar toques
  touch.reset();
  touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
  touch.setMirrorXY(true, true);  // el panel esta montado girado 180 grados
  // INT activo-bajo: salta cuando hay datos. Gatea las lecturas I2C (ver loop)
  pinMode(TP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TP_INT), touchIsr, FALLING);

  pet.begin();
  bool tripWasInterrupted = pet.tripActiveFlag;  // vor syncClock() gecapturado:
                                                    // syncClock() podria borrarlo
                                                    // si se cruzo el sueno forzado
  sdBegin();
  thumbs.load();

  // reloj real: aplica el tiempo que estuvo apagado
  rtcBegin();
  batBegin();
  pwrSetup();
  uint32_t e = rtcEpoch();
  if (e == 0) {
    rtcSetEpoch(1767225600UL);  // RTC virgen: semilla (la hora absoluta da igual,
    e = rtcEpoch();             // solo importan las diferencias)
    Serial.println("RTC sin hora: sembrado, sin progresion offline esta vez");
  }
  pet.syncClock(e);

  if (tripWasInterrupted) {
    pet.tripActiveFlag = false;
    pet.requestSave();
    tripWarningOpen = true;
  }

  imuBegin();  // QMI8658 para el contador de pasos del Ausflug/Trip
  audioBegin();  // ES8311 + I2S + amplificador (suena un jingle de arranque)

  lastInteract = millis();
}

// carga/descarga el sprite de SD cuando cambia la especie
void ensureMon() {
  if (pet.speciesId == monFor && monShinyFor == pet.shiny && !sdDirty) return;
  sdDirty = false;
  monFor = pet.speciesId;
  monShinyFor = pet.shiny;
  mon.unload();
  pmd.unload();
  beh.x = beh.targetX = 233;
  beh.mode = 0;
  beh.until = 0;
  if (pet.speciesId >= 1 && pet.speciesId <= DEX_COUNT) {
    pmd.load(pet.speciesId, pet.shiny);          // principal: PMD
    if (!pmd.loaded) mon.load(pet.speciesId, pet.shiny);  // respaldo: B/N
  }
}

void loop() {
  uint32_t now = millis();
  pet.update(now);

  // avisa con un sonido cuando el bicho pasa a estar listo para evolucionar
  // (incluye el caso de cumplir al despertar). canEvolveNow es false durmiendo.
  static bool wasEvoReady = false;
  bool evoReady = pet.wantEvolveButton();
  if (evoReady && !wasEvoReady) sfxPlay(SFX_MEDAL);
  wasEvoReady = evoReady;
  // aviso sombrio cuando el bicho esta a punto de escaparse por abandono
  static bool wasRunReady = false;
  bool runReady = pet.canRunawayNow();
  if (runReady && !wasRunReady) sfxPlay(SFX_DENY);
  wasRunReady = runReady;

  pokeSearchTick();
  visitTick();
  handleTouch();
  handleSerial();
  ensureMon();

  // ausflug/trip: sondea el sensor de pasos cada vuelta del loop, no solo al
  // redibujar, para no perder pasos entre un render y el siguiente
  if (tripOpen) {
    imuPoll(now, 40);
    uint16_t got = imuTakeSteps();
    if (got) {
      tripSteps += got;
      pet.stepsThisInterval = true;
      pet.stepsForWeight = true;
      pet.stepsForEnergy = true;
      pet.stepsForHunger = true;
      tripCheckEncounters();
    }
    if (sceneHour() >= 22) finishTrip();  // hora de dormir forzada: fin automatico
  }

  // pulsacion corta del PWR: pantalla on/off
  static uint32_t lastPwr = 0;
  if (now - lastPwr > 250) {
    lastPwr = now;
    if (pwrShortPressed()) {
      if (screenDark || screenOff) {
        // esta oscura por CUALQUIER motivo (inactividad o apagado manual):
        // un solo toque la despierta. Antes, si estaba oscura solo por
        // inactividad (screenDark=true, screenOff aun false), el primer
        // toque ponia screenOff a true (empeorando las cosas) en vez de
        // despertarla -- hacia falta un segundo toque para corregirlo
        screenOff = false;
        lastInteract = now;
      } else {
        screenOff = true;  // esta encendida: un toque la apaga
      }
    }
  }

  updateBrightness(now);

  // vuelca el autoguardado periodico SOLO con la pantalla atenuada/apagada o
  // durmiendo: la escritura a NVS congela ~1s ambos cores (caché de flash off),
  // y aqui no hay animacion que se corte ni dedo esperando respuesta. Con 90s
  // de inactividad la pantalla ya atenua, asi que se vuelca enseguida; el uso
  // activo persiste igual por los guardados de cada accion (comer/jugar/...).
  if (pet.savePending() && (screenOff || screenDark || pet.sleeping)) {
    pet.flushSave();
  }

  // anota la hora real cada 30 s (se persiste en cada save del juego)
  static uint32_t lastClock = 0;
  if (now - lastClock > 30000) {
    lastClock = now;
    uint32_t e = rtcEpoch();
    // Plausibilitaetspruefung: hier sollten nur ca. 30s vergangen sein.
    // Ein durch einen I2C-Lesefehler verfaelschter (aber von Null
    // verschiedener) Zeitstempel koennte sonst ungeprueft uebernommen
    // werden. Grosszuegiger Spielraum (1h) fuer Verarbeitungsverzoegerungen;
    // ohne vorherige Referenz (0) wird der erste gueltige Wert akzeptiert
    if (e && (!pet.lastSeenEpoch ||
              (e > pet.lastSeenEpoch ? e - pet.lastSeenEpoch : pet.lastSeenEpoch - e) <= 3600UL)) {
      pet.lastSeenEpoch = e;
    }
  }

  // Speicher-Anzeige (seit 6.22): alle 15 s eine [MEM]-Zeile im Serial Monitor
  static uint32_t lastMem = 0;
  if (now - lastMem > 15000) {
    lastMem = now;
    memLog("tick");
  }

  // latido de salud cada 5 min (para el soak test; se descarta si no hay monitor)
  static uint32_t lastHealth = 0;
  if (now - lastHealth > 300000) {
    lastHealth = now;
    Serial.printf("HEALTH up=%lus heap=%u min=%u\n", (unsigned long)(now / 1000),
                  ESP.getFreeHeap(), ESP.getMinFreeHeap());
  }

  // (auch im Pokedex-Raster mit Goldzellen, dort laeuft die Funken-Animation)
  // 85 ms en juego/saco: margen seguro para que el redibujado no pise el envio
  // DMA del frame anterior (a 40-65 ms solapaba y causaba flashes negros; con
  // sprites grandes el dibujo tarda mas, asi que se deja colchon)
  // con la pantalla a oscuras (screenDark/screenOff, brillo 0) no se ve nada:
  // saltarse el redibujado+flush ahorra CPU y trafico QSPI en cada ciclo
  if (!screenDark && !screenOff &&
      now - lastRender >= (uint32_t)((gameOpen || sackOpen || tripOpen || (galleryOpen && (galleryGoldOnPage || (galleryDetail && !galleryInfoShown && pet.dexMedals[galleryDetail])))) ? 85 : 100)) {
    lastRender = now;
    render();
  }
}

// brillo segun inactividad (proteccion del AMOLED) + apagado total a los 10 min
void updateBrightness(uint32_t now) {
  // los eventos visibles despiertan la pantalla solos; un power-nap en curso
  // (10s) se mantiene despierto toda su duracion para que la cuenta atras se
  // vea siempre, aunque a 10s nunca llegaria a chocar con los 90s/10min
  if (pet.evolving() || pet.blocked() || pet.eating() || pet.showHeart() || pet.napping()) {
    lastInteract = now;
  }
  // ojo: handleTouch() puede fijar lastInteract con un millis() mas reciente
  // que el 'now' capturado al principio de este mismo loop() -> sin este
  // guard, "now - lastInteract" desbordaria (uint32) a un numero enorme y
  // dispararia el apagado de 10 min en el primerisimo toque
  uint32_t idle = (now >= lastInteract) ? (now - lastInteract) : 0;
  // proteccion AMOLED: reloj en pantalla + USB + tiempo sin tocar ya no
  // deja la pantalla siempre al maximo -- se atenua a 25 (como Licht aus)
  // en vez de apagar del todo, y el apagado total llega mas tarde (10 min
  // en vez de los 5 min habituales) para que siga sirviendo de reloj de
  // mesa sin arriesgar tanto el panel
  bool usbClockException = usbPresent() && clockPeekOpen;
  bool usbClockDim = usbClockException && idle > 90000;
  screenDark = idle > 90000 && !usbClockException;  // 90s sin tocar: pantalla a 0 (despierto o dormido)

  // Licht aus/Power-Nap solo atenuan en la pantalla principal; en cualquier
  // otra pantalla (pokedex, reloj, menus, minijuego...) vuelve al brillo
  // normal mientras se este ahi, y retoma la atenuacion al volver
  bool onMainScreen = !galleryOpen && !cardOpen && !kbOpen && !clockOpen && !clockSavedOpen &&
                       !weekdaySetOpen && !gameOpen && !sackOpen;  // seit 6.24/6.25: "gespeichert" und Wochentags-Einstellung nicht abdunkeln
  // usbPresent() (VBUS) puede parpadear brevemente con cargadores
  // inalambricos (bobina Qi con voltaje menos estable que un cable fijo);
  // batCharging() parece tener mas histeresis interna y se queda estable
  // durante esos parpadeos -- usando el OR de ambos, un parpadeo breve de
  // VBUS ya no hace parpadear el brillo
  uint8_t target = (usbPresent() || batCharging()) ? 100 : gBrightNormal;
  if (onMainScreen) {
    if (pet.lightsOut) target = 25;
    else if (pet.napping()) target = 50;
  }
  if (usbClockDim) target = 25;
  if (screenDark) target = 0;
  if (screenOff) target = 0;
  static uint8_t current = 255;
  if (target != current) {
    current = target;
    panel->setBrightness(target);
  }

  // 5 min sin tocar: apagado total por software (no solo la pantalla). Se
  // reintenta cada vez que se cumplen otros 5 min por si el primer intento
  // no llegara a cortar la alimentacion. Desactivado durante un Trip (para
  // no perder pasos). La vista previa de la hora con USB conectado usa un
  // plazo mas largo (10 min) en vez de quedar exenta del todo
  static bool shutdownTried = false;
  uint32_t shutdownAfter = usbClockException ? 600000UL : 300000UL;
  if (idle > shutdownAfter && !tripOpen) {
    if (!shutdownTried) {
      shutdownTried = true;
      pmuShutdown();
    }
  } else {
    shutdownTried = false;
  }
}

// ---------- consola serie (provision de SD + depuracion) ----------

// Speicherstand fuer den Serial Monitor (115200 Baud). PSRAM = der grosse
// externe Speicher (Bildpuffer, Sprites), Heap = der kleine interne.
// "min" ist der niedrigste Stand seit dem Start: das ist der Wert, der zaehlt.
void memLog(const char *tag) {
  uint32_t ps = ESP.getPsramSize();
  Serial.printf("[MEM] %-10s up=%lus | PSRAM frei %u KB (min %u KB, groesster Block %u KB) von %u KB | Heap frei %u KB (min %u KB)\n",
                tag, (unsigned long)(millis() / 1000),
                (unsigned)(ESP.getFreePsram() / 1024), (unsigned)(ESP.getMinFreePsram() / 1024),
                (unsigned)(ESP.getMaxAllocPsram() / 1024), (unsigned)(ps / 1024),
                (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMinFreeHeap() / 1024));
}

void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;
  if (sdSerialCommand(line)) return;

  if (line == "HATCH") {
    for (int i = 0; i < 15; i++) pet.eggTap();
    Serial.println("DONE");
  } else if (line.startsWith("SPEC ")) {
    int n = line.substring(5).toInt();
    if (n >= 1 && n <= DEX_COUNT) {
      pet.prevSpeciesId = pet.speciesId;
      pet.speciesId = n;
      Serial.printf("especie #%d %s\n", n, DEX_TBL[n].name);
    }
    Serial.println("DONE");
  } else if (line.startsWith("LVL ")) {
    pet.ageMinutes = Pet::minutesForLevel((uint16_t)line.substring(4).toInt());
    Serial.println("DONE");
  } else if (line.startsWith("TIME ")) {
    uint32_t e = (uint32_t)line.substring(5).toInt();
    rtcSetEpoch(e);
    pet.setClock(e);
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line.startsWith("RTCSET ")) {  // solo RTC (simular apagados en pruebas)
    rtcSetEpoch((uint32_t)line.substring(7).toInt());
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line == "TIME") {
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line == "GAL") {
    galleryOpen = !galleryOpen;
    galleryDetail = 0;
    galleryDirty = true;
    if (!galleryOpen) galleryPmd.unload();
    Serial.println("DONE");
  } else if (line == "EGGS") {
    // simula 20 tiradas de huevo (no cambia el estado del juego)
    for (int i = 0; i < 20; i++) {
      int16_t d = pet.pickEggSpecies();
      Serial.printf("%d:%s(r%u) ", d, DEX_TBL[d].name, DEX_TBL[d].rarity);
    }
    Serial.println();
    Serial.println("DONE");
  } else if (line == "SHINY") {  // alterna shiny del actual (pruebas)
    pet.shiny = !pet.shiny;
    Serial.printf("shiny=%d\n", pet.shiny);
    Serial.println("DONE");
  } else if (line.startsWith("NICK ")) {
    pet.rename(line.substring(5).c_str());
    Serial.printf("nick=%s\n", pet.nick);
    Serial.println("DONE");
  } else if (line == "CAREDAY") {  // simula un dia nuevo cuidado (pruebas)
    pet.setClock(pet.lastSeenEpoch + 86400);
    pet.caress();
    Serial.printf("streak=%u bond=%u medals=0x%X\n", pet.streak, pet.bond, pet.medals);
    Serial.println("DONE");
  } else if (line == "FAREWELL") {  // Diagnose: warum kommt die Zusammenbleiben-Frage (nicht)?
    bool isFinal = !pet.isEgg() && DEX_TBL[pet.speciesId].evolvesTo == 0;
    long secInFinal = (pet.finalFormSinceEpoch && pet.lastSeenEpoch)
                        ? (long)pet.lastSeenEpoch - (long)pet.finalFormSinceEpoch : -1;
    Serial.printf("Endform: %s (Art %d)\n", isFinal ? "ja" : "NEIN", (int)pet.speciesId);
    Serial.printf("Alter: %lu Min, noetig: %lu Min -> %s\n", (unsigned long)pet.ageMinutes,
                  (unsigned long)FAREWELL_AGE_MIN,
                  pet.ageMinutes >= FAREWELL_AGE_MIN ? "erfuellt" : "NOCH NICHT");
    Serial.printf("finalFormSinceEpoch=%lu lastSeenEpoch=%lu\n",
                  (unsigned long)pet.finalFormSinceEpoch, (unsigned long)pet.lastSeenEpoch);
    if (secInFinal < 0)
      Serial.println("Zeit in Endform: UNGUELTIG (Startzeit 0 oder in der Zukunft)");
    else
      Serial.printf("Zeit in Endform: %.1f h, noetig: 24 h -> %s\n", secInFinal / 3600.0f,
                    secInFinal >= 24L * 3600 ? "erfuellt" : "NOCH NICHT");
    Serial.printf("Wiederholungs-Sperre: farDeclinedAge=%lu, Alter=%lu -> %s\n",
                  (unsigned long)pet.farDeclinedAgeValue(), (unsigned long)pet.ageMinutes,
                  pet.ageMinutes >= pet.farDeclinedAgeValue() ? "frei" : "GESPERRT");
    Serial.printf("schlaeft=%d zeremonie=%d besuch=%d meldungen=%d weglaufenBereit=%d\n",
                  pet.sleeping ? 1 : 0, (int)pet.ceremony, pet.visiting ? 1 : 0,
                  (int)pet.achvCount, pet.canRunawayNow() ? 1 : 0);
    Serial.printf("canFarewellNow=%d wantFarewellButton=%d\n",
                  pet.canFarewellNow() ? 1 : 0, pet.wantFarewellButton() ? 1 : 0);
    Serial.println("DONE");
  } else if (line == "BYE") {
    pet.startFarewell();
    Serial.println("DONE");
  } else if (line == "RUN") {
    pet.startRunaway();
    Serial.println("DONE");
  } else if (line == "BEEP") {
    sfxPlay(SFX_HATCH);  // prueba de audio
    Serial.println("DONE");
  } else if (line == "SHUTDOWN") {
    Serial.println("DONE");  // responder ANTES: tras esto no hay vuelta atras
    delay(50);
    pmuShutdown();  // schaltet auch bei angeschlossenem USB komplett ab (bestaetigt)
  } else if (line == "ABANDON") {
    pet.dbgRunawayReady();  // fuerza el estado "lista para escaparse" (test del boton)
    Serial.println("DONE");
  } else if (line == "WIPE") {
    pet.factoryReset();     // borra NVS y reinicia -> partida nueva (eleccion de inicial)
    Serial.println("DONE");
    delay(100);
    ESP.restart();
  } else if (line == "REG") {
    Serial.printf("pokedex %u/%u:", pet.registeredCount(), (unsigned)DEX_COUNT);
    for (int i = 1; i <= DEX_COUNT; i++)
      if (pet.isRegistered(i)) Serial.printf(" %d", i);
    Serial.println();
    Serial.println("DONE");
  } else if (line == "FIXCAREDAY") {
    // corrige el guardado ya afectado por el bug de lastCareDay (v3.46 y
    // anteriores): se consumia el "hueco del dia" aunque tooYoung fuera
    // true, dejando el streak bloqueado hasta el dia siguiente sin esto
    pet.lastCareDay = 0;
    Serial.println("lastCareDay puesto a 0. Ahora una accion de cuidado deberia contar.");
    Serial.println("DONE");
  } else if (line == "STREAK") {
    uint32_t e = rtcEpoch();
    uint32_t d = pet.lastSeenEpoch ? pet.lastSeenEpoch / 86400 : 0;
    bool tooYoung = pet.hatchEpoch && pet.lastSeenEpoch && pet.lastSeenEpoch >= pet.hatchEpoch &&
                    (pet.lastSeenEpoch - pet.hatchEpoch < 86400UL);
    Serial.printf("rtcEpoch=%lu lastSeenEpoch=%lu today=%lu lastCareDay=%lu\n",
                  (unsigned long)e, (unsigned long)pet.lastSeenEpoch,
                  (unsigned long)d, (unsigned long)pet.lastCareDay);
    Serial.printf("hatchEpoch=%lu secondsSinceHatch=%ld tooYoung=%d\n",
                  (unsigned long)pet.hatchEpoch,
                  (long)(pet.lastSeenEpoch - pet.hatchEpoch), tooYoung ? 1 : 0);
    Serial.printf("streak=%u bestStreak=%u petStreak=%u ageMinutes=%lu\n",
                  pet.streak, pet.bestStreak, pet.petStreak, (unsigned long)pet.ageMinutes);
    Serial.printf("sleeping=%d lightsOut=%d hourNow=%d\n",
                  pet.sleeping, pet.lightsOut, pet.hourNow());
    Serial.println("DONE");
  } else if (line == "MEM") {
    memLog("MEM");
    Serial.println("DONE");
  } else if (line == "HEALTH") {
    Serial.printf("up=%lus heap=%u min=%u sd=%d mon=%d\n",
                  (unsigned long)(millis() / 1000), ESP.getFreeHeap(),
                  ESP.getMinFreeHeap(), sdReady, pmd.loaded || mon.loaded);
    Serial.println("DONE");
  } else if (line == "STATS") {
    Serial.printf("spec=%d nv=%u com=%u fel=%u ene=%u lim=%u desc=%u sd=%d mon=%d bat=%d usb=%d rtc=%u\n",
                  pet.speciesId, pet.level(), pet.fullness, pet.joy, pet.energy,
                  pet.hygiene, pet.careMistakes, sdReady, mon.loaded,
                  batPercent(), usbPresent(), rtcEpoch());
    Serial.printf("peso=%u fue=%u def=%u vel=%u genes=%u/%u/%u tr=%u/%u/%u baya=%d\n",
                  pet.weight, pet.atkStat(), pet.defStat(), pet.speStat(),
                  pet.geneAtk, pet.geneDef, pet.geneSpe,
                  pet.trAtk, pet.trDef, pet.trSpe, pet.berryKnown);
    Serial.printf("shiny=%d streak=%u/%u bond=%u medals=0x%X(%u) nick=%s\n",
                  pet.shiny, pet.streak, pet.bestStreak, pet.bond, pet.medals,
                  pet.totalMedals, pet.nick);
    Serial.println("DONE");
  }
}

// ---------- entrada tactil ----------

bool inPetZone(int16_t x, int16_t y) {
  return x > 110 && x < 356 && y > 95 && y < 310;
}

// el toque se resuelve al LEVANTAR el dedo para distinguir tap de deslizar
void openClock();  // prototipo (definida mas abajo, hace falta antes por el hold en la vista previa)
void openWeekdaySet();  // idem

// pagina que contiene esta especie, respetando los limites de generacion
// (inverso de galleryDexAt) -- para saltar automaticamente a la pagina del
// bicho actual al abrir el pokedex
static int galRegionPages(int r) {  // Seiten dieser Region
  return (GAL_REGION[r].last - GAL_REGION[r].first) / 16 + 1;
}
static int galRegionStartPage(int r) {  // erste (globale) Seite dieser Region
  int p = 0;
  for (int i = 0; i < r; i++) p += galRegionPages(i);
  return p;
}
static int galPagesTotal() { return galRegionStartPage(GAL_REGIONS - 1) + galRegionPages(GAL_REGIONS - 1); }
static int galRegionOfPage(int page, int &pageInRegion) {  // Region der (globalen) Seite
  int r = 0;
  while (r < GAL_REGIONS - 1 && page >= galRegionStartPage(r + 1)) r++;
  pageInRegion = page - galRegionStartPage(r);
  return r;
}
static int galleryPageForDex(int16_t dex) {
  for (int r = 0; r < GAL_REGIONS; r++)
    if (dex >= GAL_REGION[r].first && dex <= GAL_REGION[r].last)
      return galRegionStartPage(r) + (dex - GAL_REGION[r].first) / 16;
  return 0;
}

void handleTouch() {
  static uint32_t lastPoll = 0;
  if (millis() - lastPoll < 20) return;  // 50 Hz le sobra a un dedo 
  lastPoll = millis();
  if (sackOpen) {  // Messung (nur Serial-Ausgabe): Abstand zwischen zwei Touch-Abfragen
    if (sackStLastPoll) { uint32_t g = lastPoll - sackStLastPoll; if (g > sackStPollGapMax) sackStPollGapMax = g; }
    sackStLastPoll = lastPoll; sackStPolls++;
  }
  // solo tocamos el bus si el chip aviso por INT o si el dedo sigue abajo (hay
  // que detectar el levantamiento). Leer el CST9217 dormido se colgaba ~1s y
  // congelaba el loop entero; SensorLib no respeta el timeout de Wire.
  if (!gTouchIrq && !wasPressed) return;
  gTouchIrq = false;
  int16_t x, y;
  bool pressed = touch.getPoint(&x, &y, 1) > 0;

  // durante un Trip, con la pantalla apagada/oscura: ignorar el touch por
  // completo (ni siquiera despertar), para que no se active solo en el
  // bolsillo. Solo el boton PWR fisico puede volver a encenderla.
  if (tripOpen && (screenDark || screenOff)) {
    wasPressed = pressed;
    return;
  }

  if (pokeSearchOpen) {  // rein zeitgesteuert, keine Interaktion noetig
    wasPressed = pressed;
    return;
  }

  // minijuego (breakout): la pala (el bicho) sigue al dedo en todo momento,
  // no solo al soltar -- por eso se resuelve aqui, antes del reconocimiento
  // de gestos (tap/swipe), que solo mira la posicion al levantar el dedo
  if (gameOpen) {
    if (pressed) {
      lastInteract = millis();
      gameDrag(x, y, !wasPressed);
    }
    wasPressed = pressed;
    return;
  }

  // combate: lauft automatico, no hay nada que tocar durante el
  if (battleOpen) {
    wasPressed = pressed;
    return;
  }

  // ausflug/trip: solo el boton de abajo (cancelar) hace algo
  if (tripOpen) {
    if (tripEndConfirm && millis() > tripEndConfirmUntil) tripEndConfirm = false;  // se acabo el tiempo: vuelve al texto normal
    if (pressed && !wasPressed && y >= 330 && y <= 405 && x >= 83 && x <= 383) {
      lastInteract = millis();
      sfxPlay(SFX_HEART);
      if (tripEndConfirm) {
        tripEndConfirm = false;
        finishTrip();
      } else {
        tripEndConfirm = true;
        tripEndConfirmUntil = millis() + 3000;
      }
    }
    wasPressed = pressed;
    return;
  }

  // resultados del ausflug: tocar en cualquier sitio avanza ("weiter") --
  // salvo el caso "niemanden getroffen", que se cierra solo e ignora toques
  if (tripResultOpen) {
    if (tripResultIsMeet) {  // Treffen: laeuft automatisch per Countdown ab, keine Touch-Zone
      wasPressed = pressed;
      return;
    }
    if (tripEncCount == 0) {
      wasPressed = pressed;
      return;
    }
    if (pressed && !wasPressed) {
      lastInteract = millis();
      tripResultIndex++;
      if (tripResultIndex > tripEncCount || (tripResultIndex == tripEncCount && !tripBerryLost)) {
        tripResultOpen = false;
        tripPmd.unload();
      } else if (tripResultIndex < tripEncCount) {
        loadTripResultSprite();
      } else {
        tripPmd.unload();  // pagina extra: baya perdida, sin sprite
      }
    }
    wasPressed = pressed;
    return;
  }

  // eleccion "junto / solo": cualquier toque decide segun la mitad tocada.
  // Igual que en la pantalla principal: con la pantalla oscura/apagada, el
  // primer toque solo despierta, no cuenta como eleccion
  if (tripChoiceOpen) {
    if (pressed && !wasPressed) {
      bool wasDark = screenDark || screenOff;
      lastInteract = millis();
      screenOff = false;
      if (!wasDark) tripChoiceTap(x, y);
    }
    wasPressed = pressed;
    return;
  }

  // aviso de apagon durante un ausflug: cualquier toque lo cierra, salvo
  // que la pantalla estuviera oscura/apagada (entonces solo despierta)
  if (tripWarningOpen) {
    if (pressed && !wasPressed) {
      bool wasDark = screenDark || screenOff;
      lastInteract = millis();
      screenOff = false;
      if (!wasDark) tripWarningOpen = false;
    }
    wasPressed = pressed;
    return;
  }

  // resultado del "Pokemon-Ausflug": tocar en cualquier sitio avanza, salvo
  // que la pantalla estuviera oscura/apagada (entonces solo despierta)
  if (tripSoloResultOpen) {
    if (pressed && !wasPressed) {
      bool wasDark = screenDark || screenOff;
      lastInteract = millis();
      screenOff = false;
      if (!wasDark) tripSoloResultTap();
    }
    wasPressed = pressed;
    return;
  }

  // aviso "triste": se cierra solo, ignorar toques
  if (tripSoloSadOpen) {
    wasPressed = pressed;
    return;
  }

  // "Pokemon-Ausflug" en curso: solo el boton de abajo hace algo, salvo que
  // la pantalla estuviera oscura/apagada (entonces solo despierta)
  if (pet.tripSoloEndEpoch) {
    if (pressed && !wasPressed) {
      bool wasDark = screenDark || screenOff;
      lastInteract = millis();
      screenOff = false;
      if (!wasDark) tripSoloTap(x, y);
    }
    wasPressed = pressed;
    return;
  }

  // saco de entrenamiento: cada toque cuenta al instante (aporrear rapido)
  if (sackOpen) {
    if (pressed && !wasPressed) {
      lastInteract = millis();
      if (y < 72) sackOpen = false;  // tocar arriba = abandonar
      else sackTap();
    }
    wasPressed = pressed;
    return;
  }

  if (pressed && !wasPressed) {  // empieza el gesto
    tX0 = tXl = x;
    tY0 = tYl = y;
    tStart = millis();
    holdFired = false;
    swallowGesture = screenDark || screenOff;  // si estaba a oscuras, solo despierta
    screenOff = false;
    lastInteract = millis();
  } else if (pressed) {  // sigue apoyado
    tXl = x;
    tYl = y;
    // pulsacion larga sin moverse sobre el bicho -> dialogo de soltar
    if (!holdFired && !swallowGesture && !galleryOpen && !cardOpen && !kbOpen && !clockOpen && !clockPeekOpen && !weekdaySetOpen && !tripOpen && !tripResultOpen && !sackOpen && !battleOpen && !feedMenuUntil && millis() - tStart > 3000 &&
        abs(tXl - tX0) < 30 && abs(tYl - tY0) < 30 && inPetZone(tX0, tY0) &&
        !pet.isEgg() && !confirmUntil && !pet.blocked()) {
      confirmUntil = millis() + 10000;
      holdFired = true;
    }
    // pulsacion larga en la vista previa de la hora: mitad de arriba -> los
    // ajustes de hora de siempre; mitad de abajo -> el nuevo ajuste del
    // dia de la semana
    if (!holdFired && !swallowGesture && clockPeekOpen && millis() - tStart > 2000 &&
        abs(tXl - tX0) < 30 && abs(tYl - tY0) < 30) {
      if (tY0 < CY) {
        openClock();
      } else {
        openWeekdaySet();
      }
      holdFired = true;
    }
    // pulsacion larga sobre la imagen en el detalle del pokedex (especie ya
    // aufgedeckt) -> abre la eleccion de favorito
    if (!holdFired && !swallowGesture && galleryOpen && galleryDetail && !galleryInfoShown &&
        !favSlotChoiceOpen && millis() - tStart > 2000 &&
        abs(tXl - tX0) < 30 && abs(tYl - tY0) < 30) {
      int ddx0 = tX0 - CX, ddy0 = tY0 - 240;
      if (ddx0 * ddx0 + ddy0 * ddy0 <= 100 * 100) {
        favSlotChoiceOpen = true;
        favSlotChooseDex = galleryDetail;
        holdFired = true;
      }
    }
    // pulsacion larga sobre "Loeschen" en el reloj -> pantalla de confirmacion
    if (!holdFired && !swallowGesture && clockOpen && !clockDeleteOpen && millis() - tStart > 2000 &&
        abs(tXl - tX0) < 30 && abs(tYl - tY0) < 30 &&
        tX0 >= 133 && tX0 <= 333 && tY0 >= 252 && tY0 <= 358) {
      clockDeleteOpen = true;
      clockDeleteUntil = millis() + 10000;
      clockDeleteProgress = 0;
      clockDeleteDir = 0;
      for (int i = 0; i < 6; i++) clockDeleteMarked[i] = false;
      holdFired = true;
    }
  } else if (wasPressed) {  // levanta el dedo: resolver gesto
    lastInteract = millis();
    int dx = tXl - tX0, dy = tYl - tY0;
    uint32_t dt = millis() - tStart;
    if (!holdFired && !swallowGesture) {
      if (abs(dx) > 80 && abs(dy) < 70 && dt < 800) onSwipe(dx > 0 ? 1 : -1);
      else if (abs(dy) > 80 && abs(dx) < 70 && dt < 800) onSwipeV(dy > 0 ? 1 : -1);
      else if (dt < 1500 && abs(dx) < 40 && abs(dy) < 40) {
        uint32_t tg = millis() - tapLastAt;
        if (tg >= TAP_MIN_GAP) { tapLastAt = millis(); onTap(tX0, tY0); }
        else Serial.printf("[TAP] Doppel-Tap ignoriert (Abstand %lu ms, Dauer %lu ms)\n", (unsigned long)tg, (unsigned long)dt);
      }
    }
  }
  wasPressed = pressed;
}

// deslizar vertical: abre/cierra la ficha del bicho

void onSwipeV(int dir) {
  if (pet.awaitingStarter()) return;  // bloqueado durante la eleccion de inicial
  if (gameOpen || galleryOpen || kbOpen || sackOpen || pet.ceremony) return;
  if (clockOpen) { revertClockAudioLang(); clockOpen = false; clockDeleteOpen = false; return; }
  if (clockPeekOpen) { clockPeekOpen = false; return; }  // cualquier direccion vuelve al juego
  if (weekdaySetOpen) { weekdaySetOpen = false; return; }
  if (cardOpen) {
    if (tripBerryChoiceOpen || berryResultOpen) return;  // se cierra solo, ignorar el gesto
    cardOpen = false;  // cualquier direccion (arriba o abajo) cierra la ficha
    return;
  }
  if (dir > 0) {                    // deslizar abajo: vista previa de la hora (siempre permitido, auch waehrend eines Besuchs)
    if (!confirmUntil && !feedMenuUntil) clockPeekOpen = true;
  } else if (pet.visiting) {
    sfxPlay(SFX_DENY);  // Statusseite waehrend des Besuchs gesperrt (Uhr bleibt erlaubt, siehe oben)
  } else if (pet.achvCount > 0 || pet.canRunawayNow() || pet.wantFarewellButton() || pet.wantEvolveButton() || choiceKind == 1) {
    // notificacion activa, sperre de escapada, o "quiere decirte algo"
    // pendiente: no se puede huir a la ficha de estado
    sfxPlay(SFX_DENY);
  } else if (!pet.isEgg() && !confirmUntil && !feedMenuUntil) {
    cardOpen = true;                // deslizar arriba: ficha
    cardPage = 0;
  }
}

// deslizar: dir +1 = hacia la derecha
void onSwipe(int dir) {
  if (pet.awaitingStarter()) return;  // bloqueado durante la eleccion de inicial
  if (clockPeekOpen) {
    // Helligkeit im Akkubetrieb (75-100 in 5er-Schritten) per Wisch
    // verstellen: rechts = dunkler, links = heller. An den Enden: Ton "abweisend"
    // Im USB-Modus oder bei ausgeschaltetem Licht haette die Einstellung
    // gerade keine sichtbare Auswirkung -- dann nur der abweisende Ton
    if (usbPresent() || batCharging() || pet.lightsOut) {
      sfxPlay(SFX_DENY);
      return;
    }
    if ((dir > 0 && gBrightNormal <= 75) || (dir < 0 && gBrightNormal >= 100)) {
      sfxPlay(SFX_DENY);
      return;
    }
    gBrightNormal = (uint8_t)(gBrightNormal + (dir > 0 ? -5 : 5));
    Preferences p;
    p.begin("tamapoke", false);
    p.putUChar("brightn", gBrightNormal);
    p.end();
    sfxPlay(SFX_HEART);  // Bestaetigungston
    return;
  }
  if (gameOpen || kbOpen || clockOpen || weekdaySetOpen) return;
  if (!galleryOpen && !cardOpen && pet.achvCount > 0) {
    // notificacion activa: tampoco se puede huir al pokedex -- pero SOLO si
    // se esta en la pantalla principal (donde se ve el banner); si ya se
    // esta en el pokedex o en la ficha de estado, no debe quedar atrapado
    sfxPlay(SFX_DENY);
    return;
  }
  if (!galleryOpen && !cardOpen && pet.canRunawayNow()) {
    // sperre de escapada activa: no se puede huir al pokedex tampoco
    sfxPlay(SFX_DENY);
    return;
  }
  if (!galleryOpen && !cardOpen && (pet.wantFarewellButton() || pet.wantEvolveButton() || choiceKind == 1)) {
    // igual que la sperre de escapada: tampoco se puede huir al pokedex
    sfxPlay(SFX_DENY);
    return;
  }
  if (cardOpen) {  // dentro de la ficha: cambiar entre las 4 paginas (circular)
    if (tripBerryChoiceOpen || berryResultOpen) return;  // no cambiar de pagina ahora
    int p = (int)cardPage + (dir > 0 ? -1 : 1);  // izquierda avanza
    cardPage = (p < 0) ? 4 : (p > 4) ? 0 : p;
    return;
  }
  if (!galleryOpen) {
    if (!pet.blocked() && !confirmUntil) {
      galleryOpen = true;
      if (dir > 0) {  // deslizar a la derecha: portada con los favoritos
        pokedexOverviewOpen = true;
        for (int i = 0; i < 3; i++) favPmd[i].load((uint16_t)pet.favorites[i], false);
      } else {  // deslizar a la izquierda: rejilla de siempre, directo
        pokedexOverviewOpen = false;
      }
      galleryPage = pet.isEgg() ? 0 : galleryPageForDex(pet.speciesId);
      galleryDetail = 0;
      galleryDirty = true;
    } else {
      sfxPlay(SFX_DENY);
    }
    return;
  }
  if (pokedexOverviewOpen) {  // deslizar en la portada: vuelve al juego
    galleryOpen = false;
    pokedexOverviewOpen = false;
    for (int i = 0; i < 3; i++) favPmd[i].unload();
    return;
  }
  if (galleryDetail) {  // en detalle: volver a la rejilla
    galleryDetail = 0;
    galleryPmd.unload();
    galleryDirty = true;
    return;
  }
  int np = galleryPage - dir;  // deslizar a la izquierda avanza pagina
  if (np < 0) np = galPagesTotal() - 1;  // circular: antes de la primera = ultima
  if (np > galPagesTotal() - 1) np = 0;  // circular: despues de la ultima = primera
  if (np != galleryPage) {
    galleryPage = np;
    galleryDirty = true;
  }
}

void onTap(int16_t x, int16_t y) {
  // Serial.printf("TOUCH %d %d\n", x, y);  // diagnostico (silenciado: satura el log)
  if (clockPeekOpen) return;  // solo el hold (arriba/abajo) hace algo aqui; un tap normal no
  if (weekdaySetOpen) { weekdaySetTap(x, y); return; }
  if (clockDeleteOpen) {
    if (clockDeleteSuccess) return;  // ya se ve el resultado, ignorar toques
    int hit = -1;
    for (int i = 0; i < 6; i++) {
      float a = -(float)PI / 2 + i * (2.0f * (float)PI / 6);
      int bx = CX + (int)(cosf(a) * 150) - 25, by = CY + (int)(sinf(a) * 150) - 25;
      if (x >= bx && x < bx + 50 && y >= by && y < by + 50) { hit = i + 1; break; }
    }
    if (hit < 0) return;  // fuera de los numeros: se ignora, no cuenta como fallo
    if (clockDeleteProgress == 0) {
      if (hit == 1) clockDeleteDir = 1;
      else if (hit == 6) clockDeleteDir = -1;
      else { revertClockAudioLang(); clockDeleteOpen = false; clockOpen = false; return; }  // numero invalido de inicio
    } else {
      int expected = (clockDeleteDir == 1) ? (1 + clockDeleteProgress) : (6 - clockDeleteProgress);
      if (hit != expected) { revertClockAudioLang(); clockDeleteOpen = false; clockOpen = false; return; }  // orden incorrecto
    }
    clockDeleteMarked[hit - 1] = true;
    clockDeleteProgress++;
    sfxPlay(SFX_HEART);
    if (clockDeleteProgress >= 6) {
      clockDeleteSuccess = true;
      clockDeleteSuccessUntil = millis() + 2500;
    }
    return;
  }
  if (pet.awaitingStarter()) {  // primera partida: elegir inicial
    for (int i = 0; i < 3; i++) {
      int ry = STARTER_ROW_Y + i * (STARTER_ROW_H + STARTER_ROW_GAP);
      if (x >= 70 && x <= 396 && y >= ry && y <= ry + STARTER_ROW_H) {
        pet.chooseStarter(STARTER_DEX[i]);
        break;
      }
    }
    return;
  }
  if (favSlotChoiceOpen) { favSlotTap(x, y); return; }
  if (favConfirmShown) { favConfirmShown = false; return; }  // cualquier toque cierra antes de tiempo
  if (galleryOpen && pokedexOverviewOpen) { pokedexOverviewTap(x, y); return; }
  if (galleryOpen) {
    galleryTap(x, y);
    return;
  }
  if (kbOpen) {
    keyboardTap(x, y);
    return;
  }
  if (clockOpen) {
    clockTap(x, y);
    return;
  }
  if (pet.ceremony) return;  // durante la despedida/escapada no hay botones ni tono (anima su propia escena)
  // banner de medalla/hito: se puede tocar para pasar al siguiente, pero
  // solo tras verse al menos 2s (para que de tiempo a leerlo antes de que
  // un toque accidental lo salte)
  if (pet.achvCount > 0 && !cardOpen && !choiceKind && !confirmUntil && !feedMenuUntil &&
      x >= 73 && x <= 393 && y >= 150 && y <= 246) {
    if (millis() - achvShowStart >= 2000) {
      pet.advanceAchievement();
      achvShowUntil = 0;
    }
    return;
  }
  if (!galleryOpen && !cardOpen && pet.achvCount > 0) {
    // notificacion activa (medalla/hito/racha/stufe), tocando fuera de su
    // zona: se ignora con un tono negativo, igual que la sperre de
    // escapada -- nada mas funciona hasta cerrarla (salvo deslizar a la
    // vista previa de la hora, ver onSwipeV). Pero SOLO en la pantalla
    // principal, donde el banner es visible -- si ya se esta en el
    // pokedex o en la ficha de estado, no debe quedar atrapado ahi
    sfxPlay(SFX_DENY);
    return;
  }
  if (cardOpen) {
    if (cardPage == 4 && berryResultOpen) return;  // se cierra solo, ignorar toques
    if (cardPage == 4 && tripBerryChoiceOpen) {
      int which = -1;
      if (y >= 166 && y <= 226) which = (x < 213) ? 0 : (x >= 253 ? 1 : -1);        // seit 6.58 16 px tiefer (vorher 150..210)
      else if (y >= 266 && y <= 326) which = (x < 213) ? 2 : (x >= 253 ? 3 : -1);   // vorher 250..310
      if (which >= 0) {
        pet.eatMagicBerry((uint8_t)which);
        sfxPlay(SFX_CLEAN);  // mismo tono que el bano/limpieza
        berryResultOpen = true;
        berryResultWhich = (uint8_t)which;
        berryResultUntil = millis() + 3000;
      }
      tripBerryChoiceOpen = false;
      return;
    }
    if (cardPage == 0 && y < 84) openKeyboard();  // tocar el nombre = renombrar
    else if (cardPage == 3 && pet.canEvolveNow() && y >= 254 && y <= 294 && x >= 96 && x <= 370) {
      // tocar el aviso verde "Ready to evolve!": abre el dialogo aunque el
      // boton automatico ya no aparezca (se rechazo una vez)
      cardOpen = false;
      choiceKind = 1;
      choiceUntil = millis() + 12000;
    } else if (cardPage == 1 && y >= 285 && y <= 355 && x >= 67 && x <= 399) {
      // Tastenzeile (6.28): Training x 70-194, Pille 206-260, Kampf 272-396,
      // je 3 px Toleranz; die Luecken dazwischen sind tote Zone (kein Treffer)
      if (x >= 67 && x <= 197) { cardOpen = false; startSack(); }          // TRAINING
      else if (x >= 203 && x <= 263) { cardOpen = false; startPokeSearch(); }  // Bluetooth-Pille
      else if (x >= 269 && x <= 399) { cardOpen = false; startBattle(); }   // KAMPF
    } else if (cardPage == 1 && y > 355 && y < 385) {
      // zona muerta: no hace nada, separa el boton ampliado del area de
      // "volver" de mas abajo para evitar toques accidentales
    } else if (cardPage == 4 && y >= 200 && y <= 240 && x >= 96 && x <= 370) {
      if (choiceKind) {
        sleepMsgId = S_NOT_YET; sleepMsgUntil = millis() + 2500;
        sfxPlay(SFX_DENY);
        cardOpen = false;
      } else if (pet.sleeping) {
        sleepMsgId = S_IS_SLEEPING; sleepMsgUntil = millis() + 2500;
        sfxPlay(SFX_DENY);
        cardOpen = false;
      } else if (pet.magicBerries > 0) {
        tripBerryChoiceOpen = true;  // se queda en la ficha, solo cambia la vista
      } else {
        sfxPlay(SFX_DENY);
        cardOpen = false;
      }
    } else if (cardPage == 4 && y >= 250 && y <= 330 && x >= 96 && x <= 370) {
      cardOpen = false;
      openTripChoice();
    } else {
      cardOpen = false;
    }
    return;
  }
  if (sleepConfirmUntil && !cardOpen) {
    if (millis() >= sleepConfirmUntil) { sleepConfirmUntil = 0; return; }
    bool b1 = (x >= 93 && x <= 373 && y >= 183 && y <= 235);  // Ja, Licht aus!
    bool b2 = (x >= 93 && x <= 373 && y >= 267 && y <= 319);  // Nein, wach bleiben!
    if (b1) { pet.confirmSleep(); sleepConfirmUntil = 0; sleepMsgId = S_LIGHTS_OUT; sleepMsgUntil = millis() + 2000; }
    else if (b2) { sleepConfirmUntil = 0; }
    else sfxPlay(SFX_DENY);  // ausserhalb der Buttons: ignoriert, aber mit abweisendem Ton
    return;
  }
  if (choiceKind && !cardOpen) {  // dialogo de decision: boton accion (arriba) / mantener (abajo).
                                    // Solo intercepta en la pantalla principal (donde se dibuja);
                                    // en la ficha de estado mandan sus propios bloqueos (Kampf/
                                    // Ausflug/Beere), para poder consultarla antes de decidir
    bool b1 = (x >= 93 && x <= 373 && y >= 183 && y <= 235);  // accion
    bool b2 = (x >= 93 && x <= 373 && y >= 267 && y <= 319);  // mantener / quedaros
    if (choiceKind == 1) {                 // evolucion: hay que decidir de verdad, un toque
      if (b1) { int16_t old = pet.speciesId; pet.evolve(); evoPmd.load(old, pet.shiny); choiceKind = 0; }
      else if (b2) { pet.declineEvolve(); choiceKind = 0; }
      else sfxPlay(SFX_DENY);  // ni b1 ni b2: se ignora, pero con tono negativo
    } else if (choiceKind == 2) {          // despedida: hay que decidir de verdad, un toque
      if (b1) { pet.startFarewell(); choiceKind = 0; }       // fuera no hace nada
      else if (b2) { pet.declineFarewell(); choiceKind = 0; }
      else sfxPlay(SFX_DENY);  // ni b1 ni b2: se ignora, pero con tono negativo
    }
    return;
  }
  if (confirmUntil) {        // dialogo "soltar?": SI / NO
    if (releaseDenied) {     // ya se ve el aviso de limite: cualquier toque lo cierra
      confirmUntil = 0;
      releaseDenied = false;
      return;
    }
    if (millis() < confirmUntil && x >= 118 && x <= 218 && y >= 252 && y <= 304) {
      if (!pet.release()) {
        // limite diario alcanzado: el MISMO recuadro blanco pasa a mostrar
        // el aviso (sin botones) en vez de cerrarse, un rato breve para
        // que de tiempo a leerlo -- igual que un banner de medalla
        releaseDenied = true;
        confirmUntil = millis() + 3500;
        sfxPlay(SFX_DENY);
        return;
      }
    }
    confirmUntil = 0;
    return;
  }
  if (feedMenuUntil) {       // selector de comida
    if (millis() < feedMenuUntil && y >= 288 && y <= 352 && x >= 101 && x <= 365) {
      int item = (x - 101) / 66;
      if (item == 3) {
        if (pet.feedCandy()) {
          eatItem = 3;
          sfxPlay(SFX_EAT);
        } else {
          sleepMsgId = S_TOO_ROUND; sleepMsgUntil = millis() + 2500;
          sfxPlay(SFX_DENY);  // kugelrund: keine chuches mas
        }
      } else if (pet.berryOnCooldown()) {
        sleepMsgId = S_BERRY_CD; sleepMsgUntil = millis() + 2500;
        sfxPlay(SFX_DENY);  // Cooldown von 5 Minuten zwischen Beeren noch nicht um
      } else if (pet.feedBerry(item)) {
        eatItem = (uint8_t)item;
        sfxPlay(SFX_EAT);
      } else {
        sleepMsgId = S_NOT_HUNGRY; sleepMsgUntil = millis() + 2500;
        sfxPlay(SFX_DENY);  // casi lleno: las bayas ya no le apetecen
      }
    }
    feedMenuUntil = 0;
    return;
  }
  if (pet.isEgg()) {
    pet.eggTap();
    eggHeartAt[eggHeartIdx++ % 3] = millis();  // nur Optik: das Spiel-Herz dauert 0,3 s, B zeigt die volle Animation
    sfxPlay(pet.eggCracks() <= 10 ? SFX_HIT_STRONG : SFX_MISS);  // tief -> hell
    return;
  }
  // boton de evolucion: abre el dialogo evolucionar/mantener
  if (pet.wantEvolveButton() && x >= EVO_BTN_X && x <= EVO_BTN_X + EVO_BTN_W &&
      y >= EVO_BTN_Y && y <= EVO_BTN_Y + EVO_BTN_H) {
    choiceKind = 1; choiceUntil = millis() + 12000;
    return;
  }
  // botones de final (mismo recuadro): escapada directa; despedida abre dialogo
  if (x >= FAR_BTN_X && x <= FAR_BTN_X + FAR_BTN_W &&
      y >= FAR_BTN_Y && y <= FAR_BTN_Y + FAR_BTN_H) {
    if (pet.canRunawayNow()) { pet.startRunaway(); return; }
    if (pet.wantFarewellButton()) { choiceKind = 2; choiceUntil = millis() + 12000; return; }
  }
  if (pet.canRunawayNow()) {
    // la sperre de escapada esta activa: solo el boton de arriba sirve, todo
    // lo demas (incl. los cuatro botones de cuidado) se ignora con un tono
    // negativo -- ya no hay forma de evitarlo con un cuidado
    sfxPlay(SFX_DENY);
    return;
  }
  if (pet.wantFarewellButton()) {
    // igual que la sperre de escapada: desde que aparece "quiere decirte
    // algo" hasta que se decide de verdad, solo el boton de arriba (ya
    // tratado mas arriba) sirve -- ni los cuatro botones de cuidado ni
    // nada mas
    sfxPlay(SFX_DENY);
    return;
  }
  if (pet.wantEvolveButton()) {
    // igual que las otras sperren: desde que aparece el boton automatico de
    // evolucion hasta que se decide de verdad, solo ese boton (ya tratado
    // mas arriba) sirve
    sfxPlay(SFX_DENY);
    return;
  }
  if (pet.visiting) {
    // Besuch laeuft: die vier Knoepfe bleiben sichtbar, sind aber nicht
    // antippbar -- abweisender Ton, gleiches Muster wie bei den Sperren oben
    sfxPlay(SFX_DENY);
    return;
  }
  for (int i = 0; i < 4; i++) {
    int dx = x - btnCx(i), dy = y - btnCy(i);
    int hit = btnHit();
    if (dx * dx + dy * dy <= hit * hit) {
      Serial.printf("BTN %d\n", i);
      if (i == 0) {
        if (!pet.sleeping && !pet.napping()) feedMenuUntil = millis() + 6000;
      } else if (i == 1) {
        startGame();
      } else if (i == 2) {
        SleepAction act = pet.sleepTap();
        switch (act) {
          case SLEEP_LIGHTS_OUT:
            sleepMsgId = S_LIGHTS_OUT; sleepMsgUntil = millis() + 2000; break;
          case SLEEP_LIGHTS_ON:
            sleepMsgId = S_LIGHTS_ON; sleepMsgUntil = millis() + 2000; break;
          case SLEEP_NAP_START:
            sleepMsgId = S_NAP; sleepMsgUntil = millis() + 2000; break;
          case SLEEP_NOT_YET:
            sleepMsgId = S_NOT_YET; sleepMsgUntil = millis() + 2500; sfxPlay(SFX_DENY); break;
          case SLEEP_NAP_COOLDOWN:
            sleepMsgId = S_NAP_CD; sleepMsgUntil = millis() + 2500; sfxPlay(SFX_DENY); break;
          case SLEEP_BLOCKED:
            sfxPlay(SFX_DENY); break;
          case SLEEP_CONFIRM_NEEDED:
            sleepConfirmUntil = millis() + 10000; break;
        }
      } else {
        startBath();
      }
      return;
    }
  }
  // tocar al bicho = caricia
  if (inPetZone(x, y)) {
    Serial.println("PET");
    if (pet.caress()) sfxPlay(SFX_HEART);
  }
}

// ---------- render ----------

bool gNight = false;  // noche real (por hora) o durmiendo: lo fija render()
uint16_t inkColor() { return gNight ? UI_INK_NIGHT : UI_INK; }

// ---------- escena de fondo: bioma del tipo + hora real del RTC ----------

#define C565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define HORIZON 232  // linea donde el cielo se encuentra con el suelo

uint16_t lerp565(uint16_t a, uint16_t b, int i, int n) {
  if (n <= 0) return a;
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  return (uint16_t)((((ar + (br - ar) * i / n) << 11)) |
                    (((ag + (bg - ag) * i / n) << 5)) | (ab + (bb - ab) * i / n));
}

// hora del dia 0-23 (de la hora real cacheada cada 30s; 13 si no hay reloj)
int sceneHour() {
  uint32_t e = pet.lastSeenEpoch;
  return e ? (int)((e / 3600) % 24) : 13;
}

// suelo de cada bioma de dia (de noche se mezcla hacia el azul nocturno)
static const uint16_t BIOME_SOIL[6] = {
  C565(0x7e, 0xc0, 0x7f),  // 0 pradera
  C565(0xdc, 0xca, 0x94),  // 1 playa (arena)
  C565(0x4f, 0x8a, 0x55),  // 2 bosque
  C565(0x8a, 0x55, 0x44),  // 3 volcan
  C565(0xa8, 0x90, 0x6a),  // 4 montana
  C565(0xe6, 0xee, 0xf5),  // 5 nieve
};

void drawClouds(uint32_t now, uint16_t col) {
  for (int k = 0; k < 2; k++) {
    int cx = (int)((now / 50 + k * 250) % 560) - 40;
    int cy = 70 + k * 34;
    gfx->fillCircle(cx, cy, 16, col);
    gfx->fillCircle(cx + 18, cy + 3, 13, col);
    gfx->fillCircle(cx - 15, cy + 4, 12, col);
  }
}

// ---------- Optik B: Wolken (Vorlage: crenberries428, MIT) ----------

// Eine Quellwolke: flacher Koerper aus ueberlappenden Kreisen, darunter ein
// weiches Schattenband und Lichter auf den oberen Woelkchen. s = Massstab in %.
void drawCloudB(int cx, int cy, int s, uint16_t col, uint16_t shade) {
  auto px = [&](int v) { return v * s / 100; };
  int r1 = px(17), r2 = px(13), r3 = px(11), r4 = px(9);
  gfx->fillRoundRect(cx - px(30), cy + px(2), px(60), px(14), px(7), shade);
  gfx->fillCircle(cx + px(20), cy + px(5), r3, shade);
  gfx->fillCircle(cx - px(20), cy + px(6), r4, shade);
  gfx->fillRoundRect(cx - px(30), cy - px(2), px(60), px(14), px(7), col);
  gfx->fillCircle(cx - px(2), cy - px(6), r1, col);
  gfx->fillCircle(cx + px(18), cy + px(1), r2, col);
  gfx->fillCircle(cx - px(19), cy + px(3), r3, col);
  gfx->fillCircle(cx + px(30), cy + px(6), r4, col);
  gfx->fillCircle(cx - px(30), cy + px(7), r4, col);
}

void drawCloudsB(uint32_t now, uint16_t col, uint16_t shade) {
  static const int Y[3] = { 62, 108, 170 };
  static const int S[3] = { 120, 80, 100 };
  static const int OFS[3] = { 0, 190, 360 };
  static const int DIV[3] = { 70, 110, 90 };  // ms pro px: ferne Wolken ziehen langsamer
  for (int k = 0; k < 3; k++) {
    int cx = (int)((now / DIV[k] + OFS[k]) % 600) - 70;
    drawCloudB(cx, Y[k], S[k], col, shade);
  }
}

// ---------- Optik B: Sonne, Mond, Sterne (Vorlage: crenberries428, MIT) ----------

// Sonne: gestufter Halo, langsam drehende Strahlen, Scheibe und Glanzpunkt.
// 'sky' ist die Himmelsfarbe auf Hoehe der Sonne.
void drawSunB(int cx, int cy, int r, uint16_t core, uint16_t sky, uint32_t now) {
  for (int i = 0; i < 4; i++)
    gfx->fillCircle(cx, cy, r + 30 - i * 8, lerp565(sky, core, i + 1, 9));
  float a0 = (now % 24000) * (2.0f * PI / 24000.0f);
  for (int k = 0; k < 8; k++) {
    float a = a0 + k * (PI / 4.0f);
    float ca = cosf(a), sa = sinf(a), cb = cosf(a + 0.16f), sb = sinf(a + 0.16f);
    int r0 = r + 4, r1 = r + (k & 1 ? 20 : 28);
    gfx->fillTriangle(cx + (int)(ca * r0), cy + (int)(sa * r0),
                      cx + (int)(cb * r0), cy + (int)(sb * r0),
                      cx + (int)(ca * r1 + cb * r1) / 2, cy + (int)(sa * r1 + sb * r1) / 2,
                      lerp565(sky, core, 6, 9));
  }
  gfx->fillCircle(cx, cy, r, core);
  gfx->fillCircle(cx, cy, r - 5, lerp565(core, C565(0xff, 0xff, 0xff), 1, 3));
  gfx->fillCircle(cx - r / 3, cy - r / 3, r / 4, C565(0xff, 0xff, 0xff));
}

// Sterne: zufaellige Orte im runden Display (einmal gewuerfelt), jeder funkelt
// mit eigener Periode und Phase; die hellen bekommen ein kleines Kreuz.
#define STAR_COUNT_B 40
struct StarB { int16_t x, y; uint16_t period, phase; uint8_t size; };
void drawStarsB(uint32_t now, uint16_t top, uint16_t bot, int yMax) {
  static StarB st[STAR_COUNT_B];
  static bool rolled = false;
  if (!rolled) {
    for (int i = 0; i < STAR_COUNT_B; i++) {
      int x, y;
      do {  // innerhalb des Kreises (Mitte 233,233, Radius 215)
        x = 20 + random(426); y = 24 + random(yMax - 24);
      } while ((x - 233) * (x - 233) + (y - 233) * (y - 233) > 215 * 215);
      st[i].x = (int16_t)x; st[i].y = (int16_t)y;
      st[i].period = (uint16_t)(1400 + random(2600));
      st[i].phase = (uint16_t)random(4000);
      st[i].size = (uint8_t)(random(4) == 0 ? 2 : 1);
    }
    rolled = true;
  }
  for (int i = 0; i < STAR_COUNT_B; i++) {
    int t = (int)((now + st[i].phase) % st[i].period) * 16 / st[i].period;  // Dreieckswelle 0..8
    int b = t < 8 ? t : 16 - t;
    int sy = st[i].y;
    uint16_t sky = lerp565(top, bot, sy, HORIZON);
    uint16_t c = lerp565(sky, C565(0xff, 0xff, 0xf0), 3 + b, 11);
    int sz = st[i].size * 2;
    gfx->fillRect(st[i].x, sy, sz, sz, c);
    if (st[i].size == 2 && b >= 5) {  // Funkelkreuz
      uint16_t d = lerp565(sky, c, 1, 2);
      gfx->fillRect(st[i].x - 3, sy + 1, 10, 2, d);
      gfx->fillRect(st[i].x + 1, sy - 3, 2, 10, d);
    }
  }
}

// Mond: weicher Halo, dann eine Sichel Zeile fuer Zeile (Mondscheibe minus
// versetzte Ausschnitt-Scheibe), damit die dunkle Seite nie ueber den Rand ragt.
void drawMoonB(int cx, int cy, uint16_t sky) {
  const int R = 26, CR = 22, cutX = cx + 12;
  for (int i = 0; i < 4; i++)
    gfx->fillCircle(cx, cy, 50 - i * 6, lerp565(sky, C565(0xb8, 0xc4, 0xe0), i + 1, 12));
  uint16_t lit = C565(0xf2, 0xf0, 0xdc);
  for (int dy = -R; dy <= R; dy++) {
    int mw = (int)sqrtf((float)(R * R - dy * dy));
    int x0 = cx - mw, x1 = cx + mw;
    int ady = dy < 0 ? -dy : dy;
    if (ady >= CR) { gfx->fillRect(x0, cy + dy, x1 - x0 + 1, 1, lit); continue; }
    int cw = (int)sqrtf((float)(CR * CR - dy * dy));
    int c0 = cutX - cw;
    if (c0 > x0) {
      int e = (c0 < x1 + 1) ? c0 : x1 + 1;
      gfx->fillRect(x0, cy + dy, e - x0, 1, lit);
    }
    if (cutX + cw < x1) gfx->fillRect(cutX + cw + 1, cy + dy, x1 - cutX - cw, 1, lit);
  }
  uint16_t cr = C565(0xd6, 0xd6, 0xc2);
  gfx->fillCircle(cx - 18, cy + 8, 3, cr);
  gfx->fillCircle(cx - 20, cy - 8, 2, cr);
  gfx->fillCircle(cx - 12, cy + 18, 2, cr);
}

// ---------- Optik B: Hintergrund-Tiere (fest im Programm, nicht von der SD) ----------
// Die kleinen Tiere (bgmons.h, nur ihre Lauf-Animationen, komprimiert) werden beim
// Betreten eines Bioma in einen kleinen PSRAM-Block entpackt (20-90 KB je Tier,
// unter 1 ms) und beim Verlassen wieder freigegeben. Kein SD-Zugriff, kein Laden
// waehrend des Zeichnens, also auch kein Ruckeln. Hoechstens zwei gleichzeitig.
#define BG_SLOTS 2
// Kampf- und Ballspiel-Bildschirm in Optik B (seit 6.31/6.32): dieselbe Landschaft wie auf dem
// Hauptbildschirm, aber tiefer (Kampf 50 px, Ballspiel 120 px; gSceneDY), damit Namen, Typen
// und Ergebnistexte auf freiem Himmel stehen.
// gBattleScene schaltet das um: ohne Tiere, Moewen und Rauch, ohne Vulkan, grossen
// Berggipfel, Felsstufe und Friedhofshaus, am Strand mit weniger Wasser.
// Alle Landschaftsfunktionen zeichnen ueber sg; sg->dy verschiebt sie senkrecht.
bool gBattleScene = false;
bool gClockScene = false;   // seit 6.57: nur waehrend renderClockPeek() true; Vulkan, Friedhof und Gebirge weichen dann der Uhr aus
#define BATTLE_DY 50       // Verschiebung der Landschaft im Kampf
#define BALL_DY 120        // Verschiebung der Landschaft im Ballspiel (das Pokemon steht dort bei y 394)
#define BALL_RESULT_UP 46  // Ballspiel Optik B: Ergebnistext so viel hoeher (mittig zwischen Rand oben und Horizont)
#define TRIP_DY 120        // Verschiebung der Landschaft auf den Ausflug-Ergebnisseiten (Optik B)
#define TRIP_PET_Y 396     // Boden-Linie des Pokemon auf den Ausflug-Ergebnisseiten (Optik B)
#define TRIP_WEITER_Y 424  // "weiter" auf den Ausflug-Ergebnisseiten (Optik B)
#define SACK_RESULT_UP (-4)  // Training Optik B: Ergebnistext um diesen Wert hoeher (negativ = tiefer; 6.38: eine Zeile tiefer als bei 6.37)
#define SACK_MIN_HIT_GAP 50  // Training (A und B): Mindestabstand zwischen zwei gezaehlten Schlaegen (ms)
#define SACK_HINT_Y_B 402  // Training Optik B: "SCHNELL HAUEN!" unter dem Zeitbalken
#define SACK_BAR_Y_B 372   // Training Optik B: Zeitbalken auf dem Boden
#define TRIP_SUN_DX 28     // Sonne/Mond auf den Ausflug-Seiten mit Uhrzeit: nach rechts ...
#define TRIP_SUN_DY 34     // ... und nach unten, damit sie die Uhrzeit nicht beruehren
#define TRIP_SOLO_SUN_UP 25  // Pokemon-Ausflug (seit 6.61): Abendsonne so viel hoeher, weil der Button dort 25 px hoeher sitzt
int gSceneDY = BATTLE_DY;  // gewaehlte Verschiebung, solange gBattleScene gilt
#define BATTLE_SEA_ADD 24  // Strand im Kampf: Wasserhorizont so viel tiefer
struct SceneG {
  int dy = 0;
  void fillRect(int x, int y, int w, int h, uint16_t c) { gfx->fillRect(x, y + dy, w, h, c); }
  void fillCircle(int x, int y, int r, uint16_t c) { gfx->fillCircle(x, y + dy, r, c); }
  void fillRoundRect(int x, int y, int w, int h, int r, uint16_t c) { gfx->fillRoundRect(x, y + dy, w, h, r, c); }
  void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) { gfx->fillTriangle(x0, y0 + dy, x1, y1 + dy, x2, y2 + dy, c); }
  void drawLine(int x0, int y0, int x1, int y1, uint16_t c) { gfx->drawLine(x0, y0 + dy, x1, y1 + dy, c); }
};
SceneG sgObj;
SceneG *sg = &sgObj;

PmdMon gBg[BG_SLOTS];
int8_t gBgWant[BG_SLOTS] = { -1, -1 };

// Zeichnet eine Aktion verankert an den Fuessen (cx = Mitte, groundY = Boden).
// zoom100 skaliert zusaetzlich zur ganzzahligen Grundstufe (100 = unveraendert),
// maxPx begrenzt die Hoehe bei Vergroesserung. Pixelraender werden aus der
// Bruchposition abgeleitet, damit bei gebrochenem Zoom keine Luecken entstehen.
void drawPmdActZ(PmdMon &m, uint8_t actId, int cx, int groundY, uint32_t t, bool loop, bool sil,
                 uint8_t maxS, uint16_t zoom100, uint16_t maxPx) {
  const PmdAct &a = m.acts[actId];
  if (!a.frames) return;
  uint8_t sBase = m.acts[PMD_IDLE].h ? 170 / m.acts[PMD_IDLE].h : 5;
  if (sBase < 2) sBase = 2;
  if (sBase > maxS) sBase = maxS;
  uint8_t s = sBase;
  while (s > 2 && a.h * s > 250) s--;
  uint32_t sc = (uint32_t)s * zoom100;  // Massstab x100
  if (zoom100 > 100 && a.h * sc > (uint32_t)maxPx * 100) sc = (uint32_t)maxPx * 100 / a.h;
  uint8_t fi = pmdFrameAt(a, t, loop);
  const uint8_t *fr = a.data + (uint32_t)fi * a.w * a.h;
  int x0 = cx - (int)(a.w * sc / 200), y0 = groundY - (int)((a.base ? a.base : a.h) * sc / 100);
  for (int r = 0; r < a.h; r++) {
    const uint8_t *row = fr + r * a.w;
    int ya = y0 + (int)(r * sc / 100), yb = y0 + (int)((r + 1) * sc / 100);
    for (int c = 0; c < a.w; c++) {
      uint8_t idx = row[c];
      if (idx == 0xFF) continue;
      int xa = x0 + (int)(c * sc / 100), xb = x0 + (int)((c + 1) * sc / 100);
      sg->fillRect(xa, ya, xb - xa, yb - ya, sil ? INK_K : m.pal[idx]);
    }
  }
}

// Entpackt ein eingebautes Tier in ein PmdMon (nur Gehen links/rechts; das
// Leerlauf-Bild fehlt, nur seine Hoehe ist fuer die Massstabsberechnung gesetzt).
bool bgAttach(PmdMon &m, int id) {
  m.unload();
  if (id < 0 || id >= BG_COUNT) return false;
  const BgSprite &sp = BG_SPR[id];
  uint32_t total = 0;
  for (int k = 0; k < 2; k++) total += (uint32_t)sp.w[k] * sp.h[k] * sp.nf[k];
  uint8_t *blob = (uint8_t *)ps_malloc(total);
  if (!blob) return false;
  m.blob = blob;
  uint8_t *dst = blob;
  for (int k = 0; k < 2; k++) {
    uint32_t need = (uint32_t)sp.w[k] * sp.h[k] * sp.nf[k];
    uint32_t o = 0;
    for (uint16_t i = 0; i < sp.rleLen[k] && o < need;) {
      uint8_t v = sp.rle[k][i];
      if (v == 0xFF) {
        uint8_t n = sp.rle[k][i + 1];
        if (o + n > need) n = (uint8_t)(need - o);
        memset(dst + o, 0xFF, n);
        o += n;
        i += 2;
      } else {
        dst[o++] = v;
        i++;
      }
    }
    if (o != need) { m.unload(); return false; }  // beschaedigte Daten: lieber kein Tier
    PmdAct &a = m.acts[k == 0 ? PMD_WALKL : PMD_WALKR];
    a.w = sp.w[k];
    a.h = sp.h[k];
    a.frames = sp.nf[k];
    for (uint8_t f = 0; f < a.frames && f < 24; f++) a.ms[f] = sp.ms[k][f];
    a.data = dst;
    uint8_t base = 1;  // unterste Zeile mit Inhalt: an den Fuessen verankern
    for (uint8_t f = 0; f < a.frames; f++) {
      const uint8_t *fr = a.data + (uint32_t)f * a.w * a.h;
      for (int r = a.h - 1; r >= 0; r--) {
        bool any = false;
        for (int c = 0; c < a.w && !any; c++)
          if (fr[r * a.w + c] != 0xFF) any = true;
        if (any) { if (r + 1 > base) base = r + 1; break; }
      }
    }
    a.base = base;
    dst += need;
  }
  m.acts[PMD_IDLE].h = sp.idleH;
  m.palCount = sp.palCount;
  for (uint8_t i = 0; i < sp.palCount && i < 256; i++) m.pal[i] = sp.pal[i];
  m.loaded = true;
  return true;
}

// Slot auf ein Tier (oder -1 = keins) stellen. Aendert sich nichts, passiert nichts.
// Ein fehlgeschlagenes Entpacken wird nicht in jedem Bild neu versucht.
void bgSet(int slot, int id) {
  if (gBgWant[slot] == id) return;
  gBgWant[slot] = (int8_t)id;
  gBg[slot].unload();
  if (id >= 0) {
    bool ok = bgAttach(gBg[slot], id);
    char tag[16];
    snprintf(tag, sizeof(tag), ok ? "BG+%s" : "BG!%s", BG_SPR[id].name);
    memLog(tag);
  }
}

// Welche Tiere gehoeren zu diesem Bioma (Tag/Nacht)? biome 255 = keins (Look A).
void bgSelect(uint8_t biome, bool night) {
  int a = -1, b = -1;
  switch (biome) {
    case 0: a = night ? BG_VENOMOTH : BG_BUTTERFREE; b = night ? -1 : BG_RATTATA; break;       // Wiese
    case 1: a = night ? -1 : BG_WINGULL; b = BG_EXEGGUTOR; break;                              // Strand
    case 2: a = night ? BG_ZUBAT : BG_BEEDRILL; b = night ? -1 : BG_CATERPIE; break;           // Wald
    case 3: a = BG_SLUGMA; break;                                                              // Vulkan
    case 4: a = night ? BG_GOLBAT : BG_FEAROW; b = night ? BG_CUBONE : BG_GEODUDE; break;      // Gebirge
    case 5: a = BG_PIPLUP; break;                                                              // Schnee
    case 6: a = night ? BG_HAUNTER : BG_GASTLY; break;                                         // Friedhof
    default: break;
  }
  bgSet(0, a);
  bgSet(1, b);
}

// ---------- Optik B: sieben Hintergruende (Vorlage: crenberries428, MIT) ----------
// Wiese, Strand, Wald, Vulkan, Gebirge, Schnee und Friedhof. Die kleinen
// Bewohner (kleine Pokemon-Sprites) laufen und fliegen weit hinter dem Pokemon.
// Friedhof (6) gibt es nur in B und nur fuer Geist-Pokemon (siehe biomeFor).


// Volcano biome, in the forest's style: layered tones lit from the sun side, and
// hand-built shapes rather than flat triangles. Back to front: a jagged distant
// range with a small second cone, a smoke plume lit from below, the main cone
// (craggy scanline-built edges, four rock tones, eroded gullies, a rim warmed by
// the crater), lava streams, an occasional eruption burst and lava bombs, glowing
// ground cracks, a small SLUGMA crawling far back, faceted boulders and a charred
// dead tree. The cone sits left of centre so its plume stays clear of the sun and
// moon (upper right). Keep the ground's lower left clear: poops land there (see
// drawPoops). The step counter covers the far left, the HUD everything below ~310.

// Cheap deterministic noise 0..255: fixed per (a, seed), so edges never shimmer.
int volNoise(int a, int seed) {
  uint32_t v = (uint32_t)a * 374761393u + (uint32_t)seed * 668265263u;
  v = (v ^ (v >> 13)) * 1274126177u;
  return (int)((v >> 16) & 0xff);
}

// A boulder: a fan of facets around a noisy outline, tone chosen by the facet's
// facing (lit toward the upper right), with a lava rim-light on the lower left.
void volBoulder(int cx, int cy, int r, int seed, bool night, uint16_t lava) {
  uint16_t t[4];
  if (night) { t[0] = C565(0x0c, 0x08, 0x0e); t[1] = C565(0x18, 0x12, 0x18); t[2] = C565(0x24, 0x1c, 0x24); t[3] = C565(0x34, 0x28, 0x30); }
  else       { t[0] = C565(0x2a, 0x1e, 0x22); t[1] = C565(0x42, 0x30, 0x34); t[2] = C565(0x5e, 0x46, 0x46); t[3] = C565(0x80, 0x60, 0x58); }
  const int N = 8;
  int vx[N + 1], vy[N + 1];
  for (int i = 0; i < N; i++) {
    float an = i * 6.2832f / N;
    int rr = r * (78 + volNoise(i, seed) % 30) / 100;
    vx[i] = cx + (int)(cosf(an) * rr * 1.25f);                // wider than tall: it sits on the ground
    vy[i] = cy + (int)(sinf(an) * rr * 0.8f);
  }
  vx[N] = vx[0]; vy[N] = vy[0];
  for (int i = 0; i < N; i++) {
    float an = (i + 0.5f) * 6.2832f / N;                       // facet facing; y is down, so up-right is negative sin
    float lit = cosf(an) * 0.6f - sinf(an) * 0.8f;             // dot with the sun direction
    uint16_t c = lit > 0.75f ? t[3] : (lit > 0.2f ? t[2] : (lit > -0.4f ? t[1] : t[0]));
    sg->fillTriangle(cx, cy - r / 6, vx[i], vy[i], vx[i + 1], vy[i + 1], c);
  }
  uint16_t rim = lerp565(t[0], lava, 1, 2);                    // lava glow along the lower-left edge
  for (int i = 3; i <= 5; i++) sg->drawLine(vx[i], vy[i], vx[i + 1], vy[i + 1], rim);
}

// Charred dead tree: forked bare limbs, a lit edge, and ember cracks that pulse.
// Thickness steps down limb by limb; the tips sway.
void volDeadTree(int cx, int footY, uint32_t now, float sway, bool night, uint16_t lava) {
  uint16_t ch  = night ? C565(0x14, 0x0e, 0x10) : C565(0x2e, 0x22, 0x22);
  uint16_t chL = night ? C565(0x24, 0x1a, 0x1c) : C565(0x58, 0x40, 0x3a);
  static const int16_t LB[12][5] = {                          // x0 y0 x1 y1 width
    { 0, 0, -3, -50, 4 }, { -3, -50, 2, -96, 3 },
    { -3, -50, -26, -74, 2 }, { -26, -74, -38, -98, 2 },
    { -3, -56, 22, -72, 2 }, { 22, -72, 34, -100, 2 },
    { 2, -96, -12, -122, 2 }, { 2, -96, 16, -118, 2 },
    { -38, -98, -47, -110, 1 }, { 34, -100, 45, -111, 1 },
    { -12, -122, -18, -136, 1 }, { 16, -118, 25, -132, 1 } };
  for (int i = 0; i < 4; i++)                                // root flare
    sg->fillTriangle(cx + (i < 2 ? -14 : 14), footY + 2, cx + (i < 2 ? -4 : 4), footY - 14, cx + (i < 2 ? -4 : 4), footY + 2, ch);
  for (int i = 0; i < 12; i++) {
    int x0 = cx + LB[i][0] + (int)(sway * -LB[i][1] / 100.0f), y0 = footY + LB[i][1];
    int x1 = cx + LB[i][2] + (int)(sway * -LB[i][3] / 100.0f), y1 = footY + LB[i][3];
    for (int w = 0; w < LB[i][4]; w++) sg->drawLine(x0 + w - LB[i][4] / 2, y0, x1 + w - LB[i][4] / 2, y1, ch);
    sg->drawLine(x0 + LB[i][4] / 2, y0, x1 + LB[i][4] / 2, y1, chL);            // lit right edge
  }
  float pulse = (sinf(now / 600.0f) + 1.0f) * 0.5f;
  uint16_t ember = lerp565(lava, C565(0xff, 0xd0, 0x58), (int)(pulse * 4), 8);
  static const int8_t EM[5][3] = { { -1, -14, 8 }, { 0, -30, 6 }, { -3, -52, 6 }, { 1, -74, 7 }, { -27, -80, 5 } };
  for (int i = 0; i < 5; i++)                                // glowing cracks in the char
    sg->fillRect(cx + EM[i][0] + (int)(sway * -EM[i][1] / 100.0f), footY + EM[i][1], 1, EM[i][2], ember);
}

void drawVolcanoB(uint32_t now, bool night, uint16_t top, uint16_t bot) {
  // Seit 6.63: Hauptbild und Uhransicht zeigen den Vulkan gleich: 90 % Groesse bei x=80 (1 px Abstand zur Uhr,
  // Uhr x 128..338, y 108..164). Kampf/Ausflug (gBattleScene) unveraendert: volle Groesse bei x=170, ohne Rauch.
  // Alle Masse am Kegel laufen ueber s_() (90 %, gerundet; im Kampf unveraendert).
  auto s_ = [](int v) { return gBattleScene ? v : (v * 90 + (v >= 0 ? 50 : -50)) / 100; };
  const int VX = gBattleScene ? 170 : 80, VT = HORIZON - s_(104), B = HORIZON + 2;   // crater centre, cone top, base
  float pulse = (sinf(now / 420.0f) + 1.0f) * 0.5f;          // 0..1
  uint16_t rockD = night ? C565(0x0e, 0x0a, 0x10) : C565(0x34, 0x24, 0x28);
  uint16_t rock  = night ? C565(0x1c, 0x16, 0x1c) : C565(0x56, 0x3e, 0x40);
  uint16_t rockM = night ? C565(0x2a, 0x20, 0x28) : C565(0x74, 0x52, 0x4c);
  uint16_t rockL = night ? C565(0x3a, 0x2e, 0x36) : C565(0x98, 0x6c, 0x60);
  uint16_t lava  = lerp565(C565(0xe8, 0x4a, 0x14), C565(0xff, 0x9a, 0x2a), (int)(pulse * 8), 8);
  uint16_t hot   = C565(0xff, 0xd0, 0x58);
  auto skyAt = [&](int y) { return lerp565(top, bot, y < 0 ? 0 : y, HORIZON); };
  auto hw = [&](int y) { return s_(26 + (y - VT) * 100 / (B - VT)); };   // cone half-width at y
  auto seg = [&](int x0, int x1, int y, uint16_t c) { if (x1 > x0) sg->fillRect(x0, y, x1 - x0, 2, c); };

  // heat glow along the horizon, then a jagged distant range and a small far cone
  sg->fillRect(0, HORIZON - 12, 466, 14, lerp565(skyAt(HORIZON), lava, 1, 7));
  uint16_t far = lerp565(rock, skyAt(HORIZON - 40), 1, 2);
  for (int x = -10; x < 480; x += 14) {                      // ridge profile interpolated between noisy peaks
    int h1 = 14 + volNoise(x / 14, 11) % 30, h2 = 14 + volNoise(x / 14 + 1, 11) % 30;
    sg->fillTriangle(x, B, x + 14, B, x, B - h1, far);
    sg->fillTriangle(x + 14, B, x + 14, B - h2, x, B - h1, far);
  }
  sg->fillTriangle(330, B, 480, B, 400, HORIZON - 54, far);                              // far second cone
  sg->fillRect(393, HORIZON - 56, 14, 3, lerp565(far, lava, 1, 3 + (int)(pulse * 1)));  // its faint crater glow

  if (!gBattleScene) {
  // smoke: puffs rise, grow and fade; each is a dark body with a lighter disc
  // low on it, so the young ones read as lit orange from the crater below
  uint16_t smoke = night ? C565(0x34, 0x2c, 0x3c) : C565(0x5c, 0x4a, 0x50);
  for (int k = 0; k < 7; k++) {
    int t = (int)((now / 36 + k * 31) % 217);               // 0..216 life of one puff
    int y = VT - 6 - t;
    int x = VX + t * t / 150 + (int)(sinf((t + k * 20) / 22.0f) * 8);   // seit 6.63: steigt ueber dem Krater auf, zieht dann schraeg nach rechts oben ab
    int r = 10 + t / 8;
    uint16_t c = lerp565(smoke, skyAt(y), t, 217);
    uint16_t glowC = t < 90 ? lerp565(c, lava, 90 - t, 260) : lerp565(c, C565(0xff, 0xff, 0xff), 1, night ? 14 : 8);
    sg->fillCircle(x, y, r, c);
    sg->fillCircle(x + 1, y + r / 4, r * 65 / 100, glowC);
  }

  // lightning flickering inside the plume every few seconds
  if (now % 5200 < 140) {
    int lx = VX + 8, ly = VT - s_(100);   // seit 6.63: ueber dem Krater; die Spitze liegt knapp ausserhalb des runden Displays (Blitz kommt aus dem Rand)
    uint16_t bolt = C565(0xff, 0xf4, 0xc8);
    static const int8_t Z[5][2] = { { 0, 0 }, { -9, 14 }, { 6, 28 }, { -8, 44 }, { 2, 62 } };
    for (int i = 0; i < 4; i++) {
      sg->drawLine(lx + Z[i][0], ly + Z[i][1], lx + Z[i + 1][0], ly + Z[i + 1][1], bolt);
      sg->drawLine(lx + Z[i][0] + 1, ly + Z[i][1], lx + Z[i + 1][0] + 1, ly + Z[i + 1][1], bolt);
    }
  }

  }

  if (!gBattleScene) {
  // main cone, one 2 px scanline at a time: noisy edges, four rock bands whose
  // borders wobble, lava warmth near the rim, and rock/light specks
  for (int y = VT; y <= B; y += 2) {
    int w = hw(y);
    int xl = VX - w + volNoise(y / 4, 1) % 5 - 2, xr = VX + w + volNoise(y / 4, 2) % 5 - 2, W = xr - xl;   // noise per 8 px: craggy, not stair-stepped
    int wob = (int)(sinf(y / 9.0f) * 4.0f);
    int b1 = xl + W * 30 / 100 + wob + volNoise(y, 3) % 7 - 3;
    int b2 = xl + W * 55 / 100 - wob + volNoise(y, 4) % 7 - 3;
    int b3 = xl + W * 76 / 100 + wob + volNoise(y, 5) % 7 - 3;
    int k = y < VT + 30 ? (30 - (y - VT)) * (50 + (int)(pulse * 14)) / 30 : 0;   // lava warmth, 0..~64
    seg(xl, b1, y, k ? lerp565(rockD, lava, k, 160) : rockD);
    seg(b1, b2, y, k ? lerp565(rock, lava, k, 160) : rock);
    seg(b2, b3, y, k ? lerp565(rockM, lava, k, 160) : rockM);
    seg(b3, xr, y, k ? lerp565(rockL, lava, k, 160) : rockL);
    if (volNoise(y, 6) < 70 && W > 4) sg->fillRect(xl + volNoise(y, 7) % W, y, 3, 2, rockD);
    if (volNoise(y, 8) < 60 && xr > b3 + 3) sg->fillRect(b3 + volNoise(y, 9) % (xr - b3 - 2), y, 2, 2, lerp565(rockL, C565(0xff, 0xff, 0xff), 1, 6));
  }
  for (int g = 0; g < 6; g++)                                // eroded gullies fanning from the rim
    for (int y = VT + 12; y < B - 4; y += 3) {
      int x = VX + s_(g * 46 - 115) * (y - VT) / (B - VT) + volNoise(y + g * 31, 12) % 5 - 2;
      sg->fillRect(x, y, 2, 3, rockD);
    }
  }
  volBoulder(VX - s_(98), B + 6, 8, 5, night, lava);              // a few boulders at the foot
  volBoulder(VX + s_(58), B + 8, 9, 6, night, lava);
  volBoulder(VX + s_(108), B + 4, 6, 7, night, lava);

  if (!gBattleScene) {
  // crater: a rocky lip, a dark throat, a lava lake with a hot centre and bubbles
  sg->fillRoundRect(VX - s_(34), VT - s_(9), s_(68), s_(17), s_(8), rockM);
  sg->fillRoundRect(VX - s_(34), VT - s_(9), s_(68), s_(5) < 3 ? 3 : s_(5), 3, rockL);                       // lit lip
  sg->fillRoundRect(VX - s_(28), VT - s_(6), s_(56), s_(11), s_(5), rockD);
  sg->fillRoundRect(VX - s_(25), VT - s_(5), s_(50), s_(9), s_(4), lava);
  sg->fillRoundRect(VX - s_(13), VT - s_(3), s_(26), 4, 2, hot);
  for (int i = 0; i < 3; i++)                                                  // bubbles popping
    if ((now / 260 + i * 2) % 5 < 2) sg->fillCircle(VX - s_(16) + i * s_(16) + (int)(sinf(now / 300.0f + i) * 3), VT - s_(3), 2, hot);

  // lava streams, wiggling, white-hot at the top, with a bright core and a glow at the foot
  const int SX[3] = { VX - s_(10), VX + s_(8), VX + s_(18) };
  const int SD[3] = { s_(-34), s_(-4), s_(40) };                         // total drift of each stream
  const int SL[3] = { s_(96), s_(70), s_(100) };                         // length
  for (int i = 0; i < 3; i++) {
    for (int y = 0; y < SL[i]; y += 2) {
      int x = SX[i] + SD[i] * y / SL[i] + (int)(sinf(y / 9.0f + i * 2) * 3);
      sg->fillRect(x - 3, VT + 4 + y, 7, 3, lerp565(lava, rockD, 1, 5));   // dark crust edge
      sg->fillRect(x - 2, VT + 4 + y, 5, 3, y < 10 ? hot : lava);
      sg->fillRect(x - 1, VT + 4 + y, 2, 3, lerp565(lava, hot, 1, 2));     // core
      if (y > 20 && volNoise(y + i * 17, 16) < 50) sg->fillRect(x - 2, VT + 4 + y, 5, 3, lerp565(lava, rockD, 2, 3));   // cooled crust patches
    }
    int fx = SX[i] + SD[i];
    if (VT + 4 + SL[i] > B - 14) sg->fillCircle(fx, B - 2, 6 + (int)(pulse * 2), lerp565(rock, lava, 1, 2));
  }

  // every ~9 s the crater erupts for a second: a fountain of sparks and a flare
  uint32_t ep = now % 9000;
  if (ep < 1100) {
    int et = (int)(ep * 100 / 1100);                         // 0..99
    sg->fillCircle(VX, VT - 2, 16 * (100 - et) / 100 + 4, lerp565(lava, hot, 1, 2));
    for (int i = 0; i < 16; i++) {
      int dx = s_((volNoise(i, 17) % 121) - 60), vy = s_(70 + volNoise(i, 18) % 70);
      int x = VX + dx * et / 100, y = VT - vy * et / 100 + 90 * et * et / 10000;
      if (y > 14 && y < B - 4) sg->fillRect(x, y, 3, 3, i & 1 ? hot : lava);
    }
  }

  // lava bombs: thrown from the crater on arcs, trailing sparks
  static const int8_t BVX[4] = { -92, -40, 56, 104 };         // horizontal reach
  static const uint8_t BH[4] = { 130, 104, 120, 90 };         // arc height
  for (int k = 0; k < 4; k++) {
    for (int tr = 0; tr < 3; tr++) {                         // head, then two fading trail dots
      int ph = (int)((now / 12 + k * 50) % 210) - tr * 5;     // 0..209, tr steps back in time
      if (ph < 0 || ph >= 150) continue;
      int x = VX + s_(BVX[k]) * ph / 150;
      int y = VT - 4 * s_(BH[k]) * ph * (150 - ph) / (150 * 150);
      uint16_t c = tr == 0 ? hot : (tr == 1 ? lava : lerp565(skyAt(y), lava, 1, 2));
      int sz = tr == 0 ? 5 : (tr == 1 ? 4 : 3);
      if (y > 14 && y < B - 4) sg->fillRect(x - sz / 2, y - sz / 2, sz, sz, c);
    }
  }

  }

  // cracked ground: thin jagged fissures with dim lava showing through, kept to
  // the edges and well under the stream brightness so the pet stays the focus
  static const int16_t CK[5][8][2] = {                        // x,y points, x < 0 ends the line
    { { 332, 236 }, { 348, 248 }, { 338, 262 }, { 362, 274 }, { 354, 290 }, { 378, 304 }, { -1, 0 } },
    { { 348, 248 }, { 372, 254 }, { 390, 246 }, { -1, 0 } },
    { { 414, 240 }, { 426, 256 }, { 416, 270 }, { 442, 284 }, { -1, 0 } },
    { { 112, 238 }, { 126, 250 }, { 116, 262 }, { 138, 272 }, { -1, 0 } },
    { { 262, 246 }, { 274, 260 }, { 266, 274 }, { 286, 286 }, { -1, 0 } } };
  uint16_t crack = lerp565(lerp565(rockD, lava, 5, 8), lava, (int)(pulse * 3), 8);
  for (int c = 0; c < 5; c++)
    for (int i = 0; CK[c][i + 1][0] >= 0; i++) {
      int x0 = CK[c][i][0], y0 = CK[c][i][1], x1 = CK[c][i + 1][0], y1 = CK[c][i + 1][1];
      sg->drawLine(x0 - 1, y0, x1 - 1, y1, rockD);           // dark lip either side
      sg->drawLine(x0 + 1, y0, x1 + 1, y1, rockD);
      sg->drawLine(x0, y0, x1, y1, crack);
    }

  // a SLUGMA creeping along behind the pet, slowly
  if (gBg[0].loaded) {
    const uint32_t P = 34000;
    uint32_t u = now % (2 * P);
    bool right = u < P;
    uint32_t tri = right ? u : 2 * P - u;
    int x = 250 + (int)(100ULL * tri / P);
    uint8_t act = right ? PMD_WALKR : PMD_WALKL;
    if (!gBg[0].has(act)) act = PMD_IDLE;
    drawPmdActZ(gBg[0], act, x, 262, now, true, false, 2, 70, 220);
  }

  // faceted boulders in front, on the right and the middle ground
  volBoulder(412, 296, 17, 1, night, lava);
  volBoulder(380, 302, 10, 2, night, lava);
  volBoulder(306, 292, 11, 3, night, lava);
  volBoulder(452, 300, 12, 4, night, lava);
  // the charred tree last, so it stands in front
  volDeadTree(436, 274, now, sinf(now / 1700.0f) * 2.0f, night, lava);

  // ash drifting down across the sky, embers rising from the crater
  for (int a = 0; a < 14; a++) {
    int x = (a * 67 + (int)(now / 70) + a * a * 5) % 466;
    int y = (a * 41 + (int)(now / 34)) % (HORIZON - 30) + 24;
    sg->fillRect(x, y, 2, 2, lerp565(skyAt(y), rockD, 1, 2));
  }
  for (int e = 0; e < (gBattleScene ? 0 : 8); e++) {
    int life = (int)((now / (14 + e * 2) + e * 53) % 190);
    int x0 = VX - 20 + e * 6;
    int y0 = VT;
    int x = x0 + (int)(sinf((life + e * 17) / 14.0f) * 10);
    int y = y0 - life;
    if (y < 20) continue;
    sg->fillRect(x, y, life < 120 ? 3 : 2, life < 120 ? 3 : 2, life < 120 ? hot : lava);
  }
}

// Beach biome: a hazy island and a sailboat on a banded sea whose crests drift
// at depth-scaled speeds, a glitter path under the sun or moon, gulls by day, a
// tide that washes foam in and out over wet sand, an EXEGGUTOR pacing the
// shore, a WINGULL overhead and shells.
// The sea horizon is higher than HORIZON so there is room for water above the
// pet; the lower left stays bare (poops land there, see drawPoops).
#define SEA_TOP 176   // sea horizon y

void drawBeachB(uint32_t now, bool night, int h, uint16_t top, uint16_t bot, uint16_t soil) {
  const int SEAT = SEA_TOP + (gBattleScene ? BATTLE_SEA_ADD : 0);   // im Kampf liegt der Wasserhorizont tiefer
  bool sunset = !night && h >= 18, sunrise = !night && h < 8;
  auto skyAt = [&](int y) { return lerp565(top, bot, y < 0 ? 0 : y, HORIZON); };

  uint16_t farC, nearC;
  if (night)  { farC = C565(0x1e, 0x38, 0x58); nearC = C565(0x0c, 0x1c, 0x34); }
  else        { farC = C565(0x86, 0xcc, 0xdc); nearC = C565(0x2a, 0x82, 0xb0); }
  if (sunset || sunrise) {                                  // tint the water by the sky
    farC = lerp565(farC, bot, 1, 3);
    nearC = lerp565(nearC, bot, 1, 5);
  }
  uint16_t crest = night ? lerp565(nearC, C565(0x8a, 0xa8, 0xd0), 1, 3) : lerp565(farC, C565(0xff, 0xff, 0xff), 2, 3);

  // distant island and sailboat sit on the horizon, hazed toward the sky
  uint16_t isl = lerp565(night ? C565(0x10, 0x20, 0x1c) : C565(0x3e, 0x72, 0x56), skyAt(SEAT), 1, 3);
  sg->fillCircle(408, SEAT + 2, 24, isl);
  sg->fillCircle(380, SEAT + 4, 15, isl);
  if (!night) {
    int bob = (int)(sinf(now / 700.0f) * 1.5f);
    int bx = 172;
    sg->fillTriangle(bx - 1, SEAT - 22 + bob, bx - 1, SEAT - 4 + bob, bx + 11, SEAT - 4 + bob, C565(0xff, 0xff, 0xff));
    sg->fillTriangle(bx - 3, SEAT - 17 + bob, bx - 3, SEAT - 4 + bob, bx - 13, SEAT - 4 + bob, lerp565(C565(0xff, 0xff, 0xff), skyAt(SEAT), 1, 4));
    sg->fillRect(bx - 12, SEAT - 3 + bob, 26, 3, C565(0x7a, 0x4a, 0x30));
  }

  // sea: depth bands, haze at the horizon
  const int ROWS = 6, RH = gBattleScene ? 7 : 11;
  for (int r = 0; r < ROWS; r++)
    sg->fillRect(0, SEAT + r * RH, 466, RH, lerp565(farC, nearC, r, ROWS - 1));
  sg->fillRect(0, SEAT, 466, 3, lerp565(skyAt(SEAT), farC, 1, 2));

  // drifting crests: farther rows are slower and shorter (parallax)
  for (int r = 0; r < ROWS; r++) {
    int y0 = SEAT + r * RH;
    for (int k = 0; k < 7; k++) {
      int x = ((k * 83 + r * 37) + (int)(now / (52 - r * 6))) % 540 - 40;
      int yy = y0 + 5 + (int)(sinf(now / 520.0f + k + r) * 2);
      int w = 10 + r * 5;
      sg->fillRect(x, yy, w, 2, crest);
      sg->fillRect(x + w / 3, yy + 2, w / 3, 1, lerp565(lerp565(farC, nearC, r, ROWS - 1), crest, 1, 2));
    }
  }

  // glitter path under the sun (or moon): flickering dashes that widen toward us
  int gx = sunset ? 233 : 360;
  uint16_t gl = night ? C565(0xd8, 0xe0, 0xf0) : C565(0xff, 0xf4, 0xd0);
  for (int r = 0; r < ROWS; r++)
    for (int k = 0; k < 4; k++) {
      if ((now / 140 + k * 2 + r * 3) % 4 == 0) continue;     // flicker
      int dx = (int)(sinf(now / 420.0f + k * 1.9f + r) * (5 + r * 5));
      int w = 4 + r * 2;
      sg->fillRect(gx + dx - w / 2, SEAT + r * RH + 2 + k * 2, w, 1, gl);
    }

  // sand, then the tide: per column the water reaches ye(x), wet sand just beyond
  sg->fillRect(0, SEAT + ROWS * RH, 466, 466 - (SEAT + ROWS * RH), soil);
  uint16_t wet = lerp565(soil, night ? C565(0x08, 0x0c, 0x18) : C565(0x70, 0x5a, 0x3c), 1, 4);
  for (int x = 0; x < 466; x += 4) {
    int ye = 252 + (int)(sinf(now / 1100.0f + x / 46.0f) * 6);
    sg->fillRect(x, SEAT + ROWS * RH, 4, ye - (SEAT + ROWS * RH), nearC);
    sg->fillRect(x, ye, 4, 14, wet);
    sg->fillRect(x, ye - 1, 4, 3, night ? C565(0xa8, 0xc0, 0xe0) : C565(0xff, 0xff, 0xff));      // foam line
    if (((x / 4) + (int)(now / 300)) % 3) sg->fillRect(x, ye + 5, 4, 1, lerp565(wet, crest, 1, 2));  // thin trail
  }

  // a WINGULL crossing the sky by day, turning round at each edge: the same
  // triangle-wave beat as the walkers, with a slow bob. Its walk cycle is the
  // wingbeat, so the walk action doubles as flying.
  if (!night && gBg[0].loaded) {
    const uint32_t P = 17000;
    uint32_t u = (now + 5000) % (2 * P);
    bool right = u < P;
    uint32_t tri = right ? u : 2 * P - u;
    int x = -40 + (int)(540ULL * tri / P);
    int y = 156 + (int)(sinf(now / 900.0f) * 9.0f);
    uint8_t act = right ? PMD_WALKR : PMD_WALKL;
    if (!gBg[0].has(act)) act = PMD_IDLE;
    drawPmdActZ(gBg[0], act, x, y, now, true, false, 1, 140, 220);
  }

  // an EXEGGUTOR pacing the waterline, small and far, turning at each end of its
  // beat. Behind the pet, in front of the sea.
  if (gBg[1].loaded) {
    static const struct { int16_t y; uint16_t zoom; uint32_t period; uint32_t phase; int16_t lo, hi; } WK[1] = {
      { 262, 125, 30000, 0, 50, 420 } };
    for (int i = 0; i < 1; i++) {
      uint32_t u = (now + WK[i].phase) % (2 * WK[i].period);
      bool right = u < WK[i].period;
      uint32_t tri = right ? u : 2 * WK[i].period - u;
      int x = WK[i].lo + (int)((WK[i].hi - WK[i].lo) * (uint64_t)tri / WK[i].period);
      uint8_t act = right ? PMD_WALKR : PMD_WALKL;
      if (!gBg[1].has(act)) act = PMD_IDLE;
      drawPmdActZ(gBg[1], act, x, WK[i].y, now, true, false, 1, WK[i].zoom, 220);
    }
  }

  // shells and a starfish on the dry sand, on the right where the pet is not
  sg->fillCircle(366, 296, 5, night ? C565(0x70, 0x58, 0x60) : C565(0xf0, 0xb8, 0xb0));
  sg->fillCircle(366, 296, 2, lerp565(soil, C565(0xff, 0xff, 0xff), 1, 3));
  uint16_t star = night ? C565(0x80, 0x48, 0x30) : C565(0xf0, 0x7a, 0x3a);
  for (int a = 0; a < 5; a++) {
    float an = a * 1.2566f - 1.5708f;
    sg->fillTriangle(424, 292, 424 + (int)(cosf(an) * 11), 292 + (int)(sinf(an) * 11),
                      424 + (int)(cosf(an + 0.6f) * 4), 292 + (int)(sinf(an + 0.6f) * 4), star);
  }

  // gulls drifting across the sky by day, clear of the header text above y 120
  if (!night && !gBattleScene)
    for (int g = 0; g < 2; g++) {
      int x = (int)(now / 30 + g * 260) % 540 - 40;
      int y = 130 + g * 18 + (int)(sinf(now / 600.0f + g) * 4);
      int up = ((now / 200 + g) & 1) ? -4 : 2;
      uint16_t gc = lerp565(C565(0x30, 0x38, 0x48), skyAt(y), 1, 4);
      sg->drawLine(x - 7, y + up, x, y, gc);
      sg->drawLine(x, y, x + 7, y + up, gc);
    }
}

// Forest biome, back to front: two hazy tree lines, drifting mist, a small
// BEEDRILL and CATERPIE, two big trees on the right that sway, ferns and mushrooms on the ground, leaves
// drifting down by day and fireflies by night. The big trees stay right of the
// pet and below the sun and moon; the lower left is bare (poops land there, see
// drawPoops) and the step counter covers the far left.
// Tree-line layer: a mix of spiky pines and rounded leafy crowns, the kind and
// height fixed per x so nothing flickers. Crowns are two overlapping circles on
// a filled base; pines are single triangles.
// Seitenansichten (Optik B, drawSideSceneBio): flachere Waldhuegel und weniger Grasreihen
// auf der Wiese, damit Texte/Zahlen lesbar bleiben. Standardwerte = unveraendertes Aussehen
// (Hauptbildschirm).
int gRidgePct = 100;   // Hoehe der Waldhuegel in Prozent
int gMeadowRows = 7;   // Zahl der gezeichneten Grasreihen (Wiese)

// Leaf canopy tones, dark to bright, by day or night.
void forestTones(bool night, uint16_t t[4]) {
  if (night) { t[0] = C565(0x08, 0x18, 0x12); t[1] = C565(0x0e, 0x26, 0x1a); t[2] = C565(0x16, 0x36, 0x24); t[3] = C565(0x20, 0x46, 0x30); }
  else       { t[0] = C565(0x24, 0x62, 0x38); t[1] = C565(0x38, 0x80, 0x44); t[2] = C565(0x5c, 0xa6, 0x4a); t[3] = C565(0x92, 0xcc, 0x58); }
}

// A leafy clump lit from the upper right: dark body, then mid, light and
// highlight discs stepping toward the sun, with a few leaf specks.
void forestClump(int cx, int cy, int r, int seed, bool night) {
  uint16_t t[4]; forestTones(night, t);
  sg->fillCircle(cx, cy, r, t[0]);
  sg->fillCircle(cx + r / 8, cy - r / 8, r * 85 / 100, t[1]);
  sg->fillCircle(cx + r / 4, cy - r / 4, r * 58 / 100, t[2]);
  sg->fillCircle(cx + r * 3 / 8, cy - r * 3 / 8, r * 30 / 100, t[3]);
  for (int i = 0; i < 5; i++) {                              // specks of light and shade
    int hsh = (seed * 53 + i * 29) & 0xff;
    int dx = (hsh % (r + 1)) - r / 3, dy = ((hsh * 7) % (r + 1)) - r * 2 / 3;
    if (dx * dx + dy * dy < r * r * 7 / 10) sg->fillRect(cx + dx, cy + dy, 2, 2, i & 1 ? t[3] : t[0]);
  }
}

// Broadleaf tree: flared roots, a barked trunk that forks, and a crown of
// clumps, with Oran berries in it. The whole crown sways; each clump also
// flutters a little on its own.
void forestOak(int cx, int footY, uint32_t now, float sway, bool night) {
  uint16_t bark  = night ? C565(0x1c, 0x14, 0x12) : C565(0x5a, 0x3c, 0x28);
  uint16_t barkD = night ? C565(0x10, 0x0c, 0x0a) : C565(0x3c, 0x26, 0x1a);
  uint16_t barkL = night ? C565(0x2a, 0x1e, 0x1a) : C565(0x84, 0x5c, 0x3a);
  const int FORK = footY - 74;
  sg->fillTriangle(cx - 16, footY + 2, cx - 6, footY - 16, cx - 6, footY + 2, bark);   // roots
  sg->fillTriangle(cx + 16, footY + 2, cx + 6, footY - 16, cx + 6, footY + 2, bark);
  sg->fillRect(cx - 7, FORK, 14, footY - FORK + 2, bark);
  sg->fillRect(cx + 3, FORK, 4, footY - FORK + 2, barkL);                              // lit edge
  sg->fillRect(cx - 7, FORK, 3, footY - FORK + 2, barkD);                              // shaded edge
  for (int i = 0; i < 5; i++)                                                           // bark grooves
    sg->fillRect(cx - 2 + (i % 2) * 3, FORK + 8 + i * 13, 1, 9, barkD);
  int csx = (int)sway, ctop = FORK - 38;                                                // crown centre
  for (int k = 0; k < 2; k++) {                                                         // the two limbs
    int ex = cx + (k ? 24 : -22) + csx, ey = FORK - 26;
    for (int w = -1; w <= 1; w++) sg->drawLine(cx + w, FORK + 2, ex + w, ey, bark);
  }
  static const int8_t CL[8][3] = { { -34, 26, 20 }, { 32, 28, 21 }, { -16, 6, 26 }, { 22, 4, 24 },
                                   { 0, -18, 24 }, { -40, 4, 16 }, { 38, 2, 16 }, { 0, 30, 20 } };
  for (int i = 0; i < 8; i++)                                                           // back to front, lower first
    forestClump(cx + CL[i][0] + csx + (int)(sinf(now / 1100.0f + i) * 1.5f),
                ctop + 28 + CL[i][1], CL[i][2], i + 3, night);
  uint16_t berry = night ? C565(0x20, 0x30, 0x70) : C565(0x4a, 0x78, 0xe0);
  static const int8_t BERRY[4][2] = { { -18, 18 }, { 14, 30 }, { 30, 14 }, { -4, 2 } };
  for (int i = 0; i < 4; i++) {
    int bx = cx + BERRY[i][0] + csx, by = ctop + 28 + BERRY[i][1];
    sg->fillCircle(bx, by, 3, berry);
    sg->fillRect(bx - 1, by - 1, 1, 1, night ? berry : C565(0xc8, 0xe0, 0xff));
  }
}

// A ragged pine: trunk plus four tiers with a jagged hem, three tones, the top
// tiers swaying most. The sun-facing (right) side is lit by day.
void forestPine(int cx, int footY, int h, int w, float sway, bool night) {
  uint16_t trunk = night ? C565(0x20, 0x18, 0x14) : C565(0x5a, 0x3c, 0x28);
  uint16_t dark  = night ? C565(0x08, 0x14, 0x0e) : C565(0x1a, 0x4c, 0x32);
  uint16_t mid   = night ? C565(0x0c, 0x1e, 0x16) : C565(0x2a, 0x6c, 0x40);
  uint16_t lit   = night ? C565(0x12, 0x2c, 0x20) : C565(0x44, 0x92, 0x4c);
  int top = footY - 22 - h, tierH = h * 45 / 100, step = (h - tierH) / 3;
  sg->fillRect(cx - 4, footY - 26, 8, 28, trunk);
  sg->fillRect(cx + 1, footY - 26, 3, 28, night ? C565(0x2a, 0x1e, 0x1a) : C565(0x84, 0x5c, 0x3a));
  for (int i = 0; i < 4; i++) {
    int ay = top + i * step, by = ay + tierH;
    int hw = w * (45 + 18 * i) / 200;
    int sx = (int)(sway * (4 - i) / 4.0f);
    sg->fillTriangle(cx + sx, ay, cx - hw, by, cx + hw, by, dark);
    sg->fillTriangle(cx + sx, ay, cx + hw, by, cx - hw / 4, by, mid);
    sg->fillTriangle(cx + sx, ay, cx + hw, by, cx + hw / 2, by, lit);
    for (int x = cx - hw; x < cx + hw; x += 7) {             // jagged hem: drooping boughs
      int t = x - cx + hw;
      uint16_t c = t > hw * 3 / 2 ? lit : (t > hw * 3 / 4 ? mid : dark);
      sg->fillTriangle(x, by - 1, x + 7, by - 1, x + 3, by + 5 + (x & 3), c);
    }
  }
}

// A low bush: three clumps.
void forestBush(int x, int y, int r, bool night) {
  forestClump(x - r * 2 / 3, y, r * 3 / 4, 1, night);
  forestClump(x + r * 2 / 3, y + 1, r * 3 / 4, 2, night);
  forestClump(x, y - r / 3, r, 3, night);
}

// Zufallszahl 0..32767 aus zwei Werten (stabil, fuer die unregelmaessige Waldlinie)
static int forestHash(int a, int b) { unsigned h = (unsigned)a * 374761393u + (unsigned)b * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return (int)((h ^ (h >> 16)) & 0x7fff); }

// Unregelmaessige Waldlinie (Optik B): Abstand, Position, Breite und Hoehe schwanken, grosse Wellen in der
// Hoehe, Tannen stehen etwas hoeher; Mitte hinter dem Pokemon flacher. side: 0 = hintere Reihe (hell),
// 1 = vordere Reihe (dunkel). hintere Reihe: links niedriger, im 3. Viertel etwas hoeher (lugt hervor),
// im letzten Viertel niedriger. vordere Reihe: rechts tiefer (Baum), links ragen nur die Tannenspitzen
// (oberes Drittel) ueber die Kronenlinie, am Fuss der Tannen sitzt ein Waldsaum.
struct ForestEl { int16_t xx, ww, ht, x; bool pine; };
static void forestRidge2(int baseY, int step, int w, int hmin, int hmax, int seed, int x0, uint16_t col, int grow, int side) {
  ForestEl es[48];
  int n = 0, i = 0;
  sg->fillRect(0, baseY - 14, 466, 14, col);   // durchgehender Sockel: zwischen den Kronen schaut nie Himmel bis zum Boden durch
  for (int x = x0; x < 480 && n < 48; i++) {
    int xx = x + forestHash(i, seed * 7 + 1) % 9 - 4, ww = w + forestHash(i, seed * 7 + 2) % 13 - 6;
    int hsh = forestHash(i, seed);
    int ht = (hmin + hsh % (hmax - hmin + 1)) * gRidgePct / 100;
    int env = 100 + (int)(24 * sinf(x * 0.012f + seed * 1.7f) + 12 * sinf(x * 0.031f + seed * 0.9f));
    int dd = abs(x + w / 2 - 233);
    if (dd < 120) env = env * (40 + dd * 60 / 120) / 100;          // Mitte (hinter dem Pokemon) flacher
    ht = ht * env / 100;
    if (side == 1) {                                               // vordere Reihe: rechts tiefer (Baum)
      int t = x < 250 ? 0 : (x > 350 ? 100 : (x - 250));
      ht = ht * (100 - 40 * t / 100) / 100;
    } else {                                                       // hintere Reihe
      int f = 100;
      if (x >= 233 && x < 300) f = 100 + 14 * (x - 233) / 67;
      else if (x >= 300 && x < 330) f = 114;
      else if (x >= 330 && x < 380) f = 114 - 54 * (x - 330) / 50;
      else if (x >= 380) f = 60;
      ht = ht * f / 100;
      int t = x > 260 ? 0 : (x < 40 ? 100 : (260 - x) * 100 / 220);   // links niedriger
      ht = ht * (100 - 30 * t / 100) / 100;
    }
    bool pine = (hsh / 7) % 3 == 0;
    if (pine) ht += forestHash(i, seed * 7 + 4) % 9;
    ht += grow;
    es[n].xx = xx; es[n].ww = ww; es[n].ht = ht; es[n].x = x; es[n].pine = pine; n++;
    x += step + forestHash(i, seed * 7 + 3) % 7 - 3;
  }
  if (side == 1) {   // links: nur das obere Drittel jeder Tanne ragt ueber die Kronenlinie der Nachbarn hinaus
    for (int k = 0; k < n; k++) {
      if (!es[k].pine || es[k].x >= 260) continue;
      int cx = es[k].xx + es[k].ww / 2, sky = 0;
      for (int j = 0; j < n; j++) {
        if (es[j].pine) continue;
        int d = abs(es[j].xx + es[j].ww / 2 - cx);
        if (d < 75 && es[j].ht > sky) sky = es[j].ht;
      }
      if (sky == 0) sky = 40 + grow;
      int tgt = sky * 125 / 100 + forestHash(k, seed * 7 + 5) % 7 - 3;
      int t = es[k].x < 120 ? 0 : (es[k].x - 120) * 100 / 140;
      es[k].ht = (tgt * (100 - t) + es[k].ht * t) / 100;
    }
  }
  for (int k = 0; k < n; k++) {
    int xx = es[k].xx, ww = es[k].ww, ht = es[k].ht;
    if (es[k].pine) {
      sg->fillTriangle(xx, baseY, xx + ww, baseY, xx + ww / 2, baseY - ht, col);
      if (side == 1 && es[k].x < 200) {   // Waldsaum um den Fuss der Tanne
        int sh = ht * 2 / 3, r = sh * 2 / 5 + 4;
        sg->fillCircle(xx + ww / 2 - r / 2, baseY - sh + r + r / 4, r, col);
        sg->fillCircle(xx + ww / 2 + r / 2, baseY - sh + r, r - 1, col);
        sg->fillRect(xx + ww / 2 - r, baseY - sh + r, 2 * r, sh - r, col);
      }
    } else {
      int r = ht * 2 / 5 + 4, cy = baseY - ht + r;
      sg->fillCircle(xx + ww / 2 - r / 2, cy + r / 4, r, col);
      sg->fillCircle(xx + ww / 2 + r / 2, cy, r - 1, col);
      sg->fillRect(xx + ww / 2 - r, cy, 2 * r, baseY - cy, col);
    }
  }
}

void drawForestB(uint32_t now, bool night, int h, uint16_t top, uint16_t bot, uint16_t soil) {
  auto skyAt = [&](int y) { return lerp565(top, bot, y < 0 ? 0 : y, HORIZON); };
  uint16_t farC = lerp565(night ? C565(0x10, 0x24, 0x2c) : C565(0x4a, 0x8c, 0x6c), skyAt(HORIZON - 40), 1, 2);
  uint16_t midC = lerp565(night ? C565(0x0c, 0x1c, 0x1c) : C565(0x2e, 0x6c, 0x4a), skyAt(HORIZON - 40), 1, 5);
  float sway = sinf(now / 1500.0f) * 3.0f;

  const int RB = HORIZON + 9, GR = RB - (HORIZON + 2);   // Grundlinie 7 px tiefer, dafuer die Kronen um GR hoeher
  forestRidge2(RB, 20, 30, 54, 90, 1, -10, farC, GR, 0);

  // Dunst: weiche Baenke aus drei uebereinanderliegenden Ellipsen, hellgrau (nachts blaulich), langsam
  // ziehend und atmend; morgens/abends gleich kraeftig wie tags
  for (int k = 0; k < 5; k++) {
    int rx = 70 + (k * 37) % 60;
    int x = (int)((k * 163 + now / (120 + k * 41)) % (466 + 2 * rx + 80)) - rx - 40;
    int y = HORIZON - 46 + (k % 3) * 11 + (int)(sinf(now / 4300.0f + k * 2.1f) * 3.0f);
    uint16_t m0 = skyAt(y);
    if (night) {
      uint16_t wc = C565(0xb0, 0xc8, 0xd8);
      snowEll(x, y, rx, 9, lerp565(m0, wc, 1, 27));
      snowEll(x + rx / 8, y + 1, rx * 72 / 100, 6, lerp565(m0, wc, 1, 18));
      snowEll(x - rx / 7, y + 1, rx * 42 / 100, 3, lerp565(m0, wc, 1, 10));
    } else {
      uint16_t wc = C565(0xc6, 0xca, 0xce);
      snowEll(x, y, rx, 9, lerp565(m0, wc, 1, 3));
      snowEll(x + rx / 8, y + 1, rx * 72 / 100, 6, lerp565(m0, wc, 1, 2));
      snowEll(x - rx / 7, y + 1, rx * 42 / 100, 3, lerp565(m0, wc, 2, 3));
    }
  }

  forestRidge2(RB, 26, 38, 40, 72, 2, -4, midC, GR, 1);
  // unruhige Unterkante des Waldstreifens (im Mittel 2 px tiefer als frueher). Sie uebermalt zugleich die
  // runden Baumkronen, die frueher bis zu 6 Zeilen unter die Grundlinie reichten und dort helle
  // Himmelspixel im Boden stehen liessen.
  uint16_t g0 = lerp565(soil, night ? C565(0x0c, 0x12, 0x24) : C565(0xff, 0xff, 0xff), 3, 16);   // Huegelfarbe des Bodens (wie in drawBiomeB)
  for (int x = 0; x < 466; x += 2) {
    int e = HORIZON + 4 + (int)(2.4f * sinf(x * 0.027f) + 1.4f * sinf(x * 0.071f + 1.3f));
    sg->fillRect(x, e, 2, 20, g0);
  }
  // Bodendunst: flache, langsame Baenke am Fuss des Waldes (laufen ueber die ganze Breite)
  for (int k = 0; k < 3; k++) {
    int rx = 110 + k * 24;
    int x = (int)((k * 230 + now / (170 + k * 50)) % (466 + 2 * rx)) - rx;
    int y = HORIZON + 3 + k * 4;
    if (night) {
      uint16_t wc = C565(0xb0, 0xc8, 0xd8);
      snowEll(x, y, rx, 6, lerp565(g0, wc, 1, 18));
      snowEll(x + rx / 6, y, rx * 65 / 100, 3, lerp565(g0, wc, 1, 9));
    } else {
      uint16_t wc = C565(0xd2, 0xd4, 0xd6);
      snowEll(x, y, rx, 6, lerp565(g0, wc, 1, 4));
      snowEll(x + rx / 6, y, rx * 65 / 100, 3, lerp565(g0, wc, 1, 3));
    }
  }

  // the BEEDRILL drifts on a slow lissajous across the mid air; the CATERPIE crawls
  // on the ground behind the ferns; at night a ZUBAT flutters about instead
  if (night && gBg[0].loaded) {                          // erratic: two beats of different speed
    float a = now / 3100.0f, b = now / 1300.0f;
    int x = 190 + (int)(120.0f * sinf(a) + 30.0f * sinf(b * 1.7f));
    int y = 178 + (int)(22.0f * sinf(b) + 8.0f * sinf(a * 3.0f));
    uint8_t act = cosf(a) > 0 ? PMD_WALKR : PMD_WALKL;
    if (!gBg[0].has(act)) act = PMD_IDLE;
    drawPmdActZ(gBg[0], act, x, y, now, true, false, 1, 100, 220);
  }
  if (!night && gBg[1].loaded) {
    const uint32_t P = 26000;
    uint32_t u = now % (2 * P);
    bool right = u < P;
    uint32_t tri = right ? u : 2 * P - u;
    int x = 230 + (int)(150ULL * tri / P);
    uint8_t act = right ? PMD_WALKR : PMD_WALKL;
    if (!gBg[1].has(act)) act = PMD_IDLE;
    drawPmdActZ(gBg[1], act, x, 258, now, true, false, 2, 70, 220);
  }
  if (!night && gBg[0].loaded) {
    float ph = now / 5200.0f;
    int x = 190 + (int)(130.0f * sinf(ph));
    int y = 176 + (int)(16.0f * sinf(now / 1700.0f));
    uint8_t act = cosf(ph) > 0 ? PMD_WALKR : PMD_WALKL;
    if (!gBg[0].has(act)) act = PMD_IDLE;
    drawPmdActZ(gBg[0], act, x, y, now, true, false, 1, 110, 220);
  }

  // ferns and mushrooms, to the right of the pet
  uint16_t fern = night ? C565(0x16, 0x3a, 0x28) : C565(0x4c, 0xa0, 0x58);
  static const int16_t FN[3][2] = { { 306, 266 }, { 322, 288 }, { 338, 262 } };
  for (int f = 0; f < 3; f++)
    for (int b = -2; b <= 2; b++) {
      int ex = FN[f][0] + b * 7, ey = FN[f][1] - 16 + abs(b) * 4 + (int)(sway * 0.3f);
      sg->drawLine(FN[f][0], FN[f][1], ex, ey, fern);
      sg->drawLine(FN[f][0] + 1, FN[f][1], ex + 1, ey, fern);
    }
  uint16_t cap = night ? C565(0x70, 0x20, 0x28) : C565(0xe0, 0x44, 0x40);
  uint16_t stem = night ? C565(0x80, 0x78, 0x70) : C565(0xf4, 0xec, 0xdc);
  static const int16_t MU[3][3] = { { 360, 298, 7 }, { 376, 304, 5 }, { 346, 306, 4 } };
  for (int m = 0; m < 3; m++) {
    int x = MU[m][0], y = MU[m][1], r = MU[m][2];
    sg->fillRect(x - r / 3, y - 2, r * 2 / 3 + 1, r, stem);
    sg->fillRoundRect(x - r, y - r, r * 2, r + 1, r / 2 + 1, cap);
    sg->fillRect(x - r / 2, y - r + 2, 2, 2, stem);
    sg->fillRect(x + r / 3, y - r + 3, 2, 2, stem);
  }

  // the two big pines last, so they stand in front of everything else
  forestPine(384, 262, 66, 44, sway * 0.7f, night);
  forestOak(440, 272, now, sway, night);
  forestBush(404, 296, 13, night);

  if (!night) {  // leaves drifting down on a slow slant
    static const uint16_t LC[3] = { C565(0x6c, 0xb8, 0x4c), C565(0xd8, 0xc0, 0x40), C565(0xd8, 0x80, 0x38) };
    for (int i = 0; i < 6; i++) {
      int x = (i * 83 + (int)(now / 55) + (int)(sinf(now / 600.0f + i * 2.0f) * 12)) % 520 - 30;
      int y = (i * 47 + (int)(now / 30)) % 270 + 36;
      sg->fillRect(x, y, 4, 2, LC[i % 3]);
      sg->fillRect(x + 1, y + 2, 2, 1, LC[i % 3]);
    }
  } else {       // fireflies: bob about, each pulsing on its own beat
    uint16_t glow = C565(0xd8, 0xff, 0x70);
    for (int i = 0; i < 10; i++) {
      float pulse = (sinf(now / 350.0f + i * 2.0f) + 1.0f) * 0.5f;
      if (pulse < 0.25f) continue;
      int x = 40 + (i * 97) % 400 + (int)(sinf(now / 700.0f + i) * 14);
      int y = 170 + (i * 53) % 120 + (int)(cosf(now / 900.0f + i * 1.3f) * 10);
      sg->fillRect(x - 1, y, 5, 3, lerp565(soil, glow, (int)(pulse * 4), 16));   // halo
      sg->fillRect(x, y - 1, 3, 5, lerp565(soil, glow, (int)(pulse * 4), 16));
      sg->fillRect(x, y, 3, 3, lerp565(soil, glow, (int)(pulse * 16), 16));
    }
  }
}

// Snow biome, drawn as the POLAR ICE: a flat pale sheet under a wide cold sky,
// a few faceted icebergs standing on it, one open lead, soft pressure ridges and
// cracks, and a thin wind-blown snowfall. At night a full aurora: swaying
// curtains of rays, bright at the base and fading green to cyan to violet,
// mirrored faintly in the ice. Deliberately sparse. Critters: a PIPLUP waddling
// slowly.
// Keep the lower left bare (poops land there, see drawPoops) and everything below
// the sun and moon.

// scanline ellipse (the emulator's GFX has no fillEllipse, and 1 px rows are cheap here)
void snowEll(int cx, int cy, int rx, int ry, uint16_t c) {
  for (int dy = -ry; dy <= ry; dy++) {
    int w = (int)(rx * sqrtf(1.0f - (float)(dy * dy) / (float)(ry * ry)));
    if (w > 0) sg->fillRect(cx - w, cy + dy, 2 * w, 1, c);
  }
}

// An iceberg: a fan of seven facets around a centre point, dark on the left to
// bright on the lit right, a darker waterline band and a turquoise glow where it
// meets the ice. 'haze' (0..8) fades it toward the sky for the distant ones.
void iceberg(int cx, int baseY, int w, int h, int haze, uint16_t sky, uint16_t soil, bool night) {
  static const int8_t P[8][2] = {                              // x, y as percent of w, h (y up)
    { -50, 0 }, { -34, 50 }, { -12, 68 }, { 8, 100 }, { 30, 62 }, { 50, 0 }, { 0, 0 }, { -5, 25 } };
  uint16_t tone[4];
  if (night) { tone[0] = C565(0x20, 0x30, 0x58); tone[1] = C565(0x30, 0x44, 0x74); tone[2] = C565(0x44, 0x5c, 0x90); tone[3] = C565(0x64, 0x80, 0xb4); }
  else       { tone[0] = C565(0x8c, 0xb0, 0xd4); tone[1] = C565(0xb4, 0xd0, 0xe8); tone[2] = C565(0xd8, 0xea, 0xf6); tone[3] = C565(0xff, 0xff, 0xff); }
  for (int i = 0; i < 4; i++) tone[i] = lerp565(tone[i], sky, haze, 10);
  auto X = [&](int i) { return cx + P[i][0] * w / 100; };
  auto Y = [&](int i) { return baseY - P[i][1] * h / 100; };
  static const uint8_t FACE[7][3] = { { 7, 0, 1 }, { 7, 1, 2 }, { 7, 2, 3 }, { 7, 3, 4 }, { 7, 4, 5 }, { 7, 5, 6 }, { 7, 6, 0 } };
  static const uint8_t TONE[7] = { 0, 1, 2, 3, 2, 1, 0 };
  snowEll(cx, baseY + 2, w * 55 / 100, 4, lerp565(soil, night ? C565(0x30, 0x60, 0x90) : C565(0x70, 0xc8, 0xe0), 1, 3 + haze / 2));   // glow in the ice
  for (int f = 0; f < 7; f++)
    sg->fillTriangle(X(FACE[f][0]), Y(FACE[f][0]), X(FACE[f][1]), Y(FACE[f][1]), X(FACE[f][2]), Y(FACE[f][2]), tone[TONE[f]]);
  sg->fillRect(cx - w / 2, baseY - 2, w, 3, lerp565(tone[0], night ? C565(0x10, 0x20, 0x50) : C565(0x40, 0x78, 0xb0), 1, 2));   // waterline band
}

// Aurora: swaying curtains of vertical rays. Each column has its own base height,
// ray length and brightness, all moving; rays are brightest at the base, fade
// upward, and shift green -> cyan -> violet. Two layers at different phases give
// depth. Kept clear of the moon by dimming near it. Returns nothing; also tints
// the ice faintly through 'glowAt' so the sky seems to light the ground.
void drawAurora(uint32_t now, uint16_t top, uint16_t bot, int moonX, int moonY) {
  float t = now / 1000.0f;
  for (int layer = 0; layer < 2; layer++)
    for (int x = 12; x < 454; x += 4) {
      float ph = layer * 1.9f;
      int base = 118 + layer * 14 + (int)(20.0f * sinf(x / 71.0f + t * 0.35f + ph) + 9.0f * sinf(x / 23.0f - t * 0.9f + ph));
      int len = 34 + (int)(24.0f * sinf(x / 43.0f + t * 0.5f + ph) + 12.0f * sinf(x / 15.0f + t * 1.3f));
      float br = 0.55f + 0.45f * sinf(x / 19.0f + t * 1.1f + ph * 2.0f);          // flicker along the curtain
      int dx = x - moonX, dy = base - len / 2 - moonY;
      float d2 = (float)(dx * dx + dy * dy);
      if (d2 < 3600.0f) continue;                                                    // inside 60 px of the moon: leave it alone
      if (d2 < 10000.0f) br *= (d2 - 3600.0f) / 6400.0f;                             // fade in out to 100 px
      if (len < 8 || br < 0.12f) continue;
      for (int j = 0; j < len; j += 6) {
        float f = (float)j / len;                                                    // 0 at the base, 1 at the tip
        int a = (int)(br * (1.0f - f) * (1.0f - f * 0.3f) * 20.0f) + 1;              // brighter at the base
        uint16_t c = f < 0.5f ? lerp565(C565(0x40, 0xff, 0x98), C565(0x40, 0xd8, 0xff), (int)(f * 20), 10)
                              : lerp565(C565(0x40, 0xd8, 0xff), C565(0xb0, 0x60, 0xff), (int)((f - 0.5f) * 20), 10);
        int y = base - j - 6;
        if (y < 18) break;
        sg->fillRect(x, y, 4, 6, lerp565(lerp565(top, bot, y, HORIZON), c, a, 28));
      }
    }
}

void drawSnowB(uint32_t now, bool night, int h, uint16_t top, uint16_t bot, uint16_t soil) {
  auto skyAt = [&](int y) { return lerp565(top, bot, y < 0 ? 0 : y, HORIZON); };
  const int B = HORIZON + 2;
  uint16_t sW = night ? C565(0x98, 0xaa, 0xd0) : C565(0xf6, 0xfb, 0xff);
  uint16_t haze = skyAt(HORIZON - 30);

  if (night && !gBattleScene) drawAurora(now, top, bot, 360, 78);   // auf Kampf-/Ballspiel-/Ausflug-Seiten ohne Polarlichter (Text steht auf dem Himmel)

  // distant icebergs on the horizon, hazed toward the sky
  iceberg(92, B, 120, 62, 3, haze, soil, night);
  iceberg(300, B, 80, 40, 4, haze, soil, night);
  sg->fillRect(0, B - 1, 466, 3, lerp565(soil, haze, 1, 3));            // soft horizon line

  // the ice sheet: faint aurora glow in it at night, soft ridges, cracks, one open lead
  if (night) {
    float t = now / 1000.0f;
    for (int x = 0; x < 466; x += 8) {
      int a = 1 + (int)((sinf(x / 40.0f + t * 0.5f) + 1.0f) * 2.5f);
      sg->fillRect(x, B + 2, 8, 22, lerp565(soil, C565(0x40, 0xe0, 0x98), a, 40));
    }
  }
  uint16_t shade = night ? C565(0x30, 0x40, 0x70) : C565(0xc0, 0xd4, 0xea);
  uint16_t lite = night ? C565(0x78, 0x8c, 0xb8) : C565(0xff, 0xff, 0xff);
  snowEll(240, 256, 84, 4, shade);                                      // two long, low pressure ridges
  snowEll(244, 254, 80, 3, lite);
  snowEll(150, 284, 60, 3, shade);
  snowEll(153, 283, 56, 2, lite);
  uint16_t crack = night ? C565(0x28, 0x38, 0x68) : C565(0x88, 0xa8, 0xd0);
  static const int16_t CK[2][6][2] = { { { 296, 246 }, { 312, 258 }, { 306, 270 }, { 330, 282 }, { 326, 296 }, { -1, 0 } },
                                       { { 100, 248 }, { 118, 256 }, { 112, 266 }, { -1, 0 } } };
  for (int c = 0; c < 2; c++)
    for (int i = 0; CK[c][i + 1][0] >= 0; i++)
      sg->drawLine(CK[c][i][0], CK[c][i][1], CK[c][i + 1][0], CK[c][i + 1][1], crack);
  uint16_t water = night ? C565(0x10, 0x20, 0x4c) : C565(0x3c, 0x6c, 0x9c);   // the open lead, ice-rimmed
  snowEll(398, 272, 62, 6, night ? C565(0x70, 0x84, 0xb0) : C565(0xe8, 0xf2, 0xfa));
  snowEll(398, 272, 56, 4, water);
  snowEll(404, 271, 28, 1, lerp565(water, sW, 1, 3));                    // sky reflected in it
  iceberg(418, 276, 56, 42, 1, haze, soil, night);                       // one nearer berg beside it

  // sparkles on the ice: a few fixed spots, each twinkling on its own beat
  for (int i = 0; i < 8; i++) {
    int x = 130 + volNoise(i, 23) % 320, y = 244 + volNoise(i, 24) % 54;
    if (x < 200 && y > 262) continue;                        // poops land bottom left
    if ((now / 260 + i * 3) % 8 != 0) continue;
    sg->fillRect(x, y, 2, 2, sW);
    sg->fillRect(x - 2, y, 6, 1, lerp565(soil, sW, 1, 2));
    sg->fillRect(x, y - 2, 1, 6, lerp565(soil, sW, 1, 2));
  }

  // the walker: a PIPLUP waddling slowly behind the pet at any hour
  if (gBg[0].loaded) {
    const uint32_t P = 30000;
    uint32_t u = now % (2 * P);
    bool right = u < P;
    uint32_t tri = right ? u : 2 * P - u;
    int x = 250 + (int)(80ULL * tri / P);
    uint8_t act = right ? PMD_WALKR : PMD_WALKL;
    if (!gBg[0].has(act)) act = PMD_IDLE;
    drawPmdActZ(gBg[0], act, x, 262, now, true, false, 2, 70, 220);
  }

  // heavy snowfall in three depths, blown sideways by the wind: many small far
  // flakes, fewer big near ones, each with its own speed and a little wobble
  for (int d = 0; d < 3; d++) {
    int n = 46 - d * 12, sz = 2 + d, drift = 22 - d * 5, fall = 60 + d * 25;
    for (int i = 0; i < n; i++) {
      int x = (i * 71 + d * 37 + 466 + (int)(now / drift) + (int)(sinf(now / 700.0f + i) * 5.0f)) % 466;
      int y = (i * 53 + d * 29 + (int)(now / (38 - d * 9 - i % 3 * 3))) % (HORIZON + fall) - 10;
      if (night) { sg->fillRect(x, y, sz, sz, d == 0 ? lerp565(skyAt(y), sW, 2, 3) : sW); continue; }
      uint16_t tint = C565(0xa8, 0xc4, 0xe4);                // by day white-on-pale is invisible: tint, plus a shadow
      if (d == 0) { sg->fillRect(x, y, sz, sz, lerp565(skyAt(y), tint, 2, 3)); continue; }
      sg->fillRect(x + 1, y + 1, sz, sz, tint);
      sg->fillRect(x, y, sz, sz, C565(0xff, 0xff, 0xff));
    }
  }
}

// Meadow biome: an open flower meadow of tall grass in the wind. Layered rolling
// hills behind; in front, seven rows of long curved blades whose tips sway in a
// wave that travels across the field (brighter where it crests, stronger in
// gusts), with wildflowers standing above the grass and seed fluff drifting.
// Day: a small BUTTERFREE overhead and a RATTATA scurrying through the grass;
// night: fireflies and a VENOMOTH, everything darker. Kept calm. The lower left
// stays short and bare (poops land there, see drawPoops), the sun and moon clear.

void drawMeadowB(uint32_t now, bool night, int h, uint16_t top, uint16_t bot, uint16_t soil) {
  auto skyAt = [&](int y) { return lerp565(top, bot, y < 0 ? 0 : y, HORIZON); };
  const int B = HORIZON + 2;
  float gust = 1.0f + 0.6f * sinf(now / 3300.0f);             // 0.4..1.6: the wind comes and goes

  // layered hills: pale and hazy far away, richer green near, tiny bushes on the middle one
  uint16_t farH = lerp565(night ? C565(0x18, 0x34, 0x34) : C565(0x6c, 0xaa, 0x8c), skyAt(HORIZON - 30), 1, 3);
  uint16_t midH = night ? C565(0x14, 0x38, 0x2a) : C565(0x4c, 0x98, 0x58);
  snowEll(70, B, 150, 26, farH);
  snowEll(330, B, 170, 22, farH);
  snowEll(455, B, 100, 30, farH);
  snowEll(190, B + 2, 165, 18, midH);
  snowEll(425, B + 2, 125, 20, midH);
  forestBush(150, B - 14, 9, night);
  forestBush(322, B - 12, 8, night);
  forestBush(262, B - 8, 6, night);

  // residents: the flyer by day/night (the rattata is drawn mid-grass below)
  if (gBg[0].loaded) {
    float ph = now / 6200.0f;
    int x = 200 + (int)(120.0f * sinf(ph)), y = 188 + (int)(14.0f * sinf(now / 1500.0f));
    uint8_t act = cosf(ph) > 0 ? PMD_WALKR : PMD_WALKL;
    if (!gBg[0].has(act)) act = PMD_IDLE;
    drawPmdActZ(gBg[0], act, x, y, now, true, false, 1, 110, 220);
  }

  // grass tones, dark to bright, and the flower palette (day / night)
  uint16_t gT[4];
  if (night) { gT[0] = C565(0x0e, 0x24, 0x1a); gT[1] = C565(0x18, 0x38, 0x28); gT[2] = C565(0x26, 0x50, 0x36); gT[3] = C565(0x5a, 0x7c, 0x5a); }
  else       { gT[0] = C565(0x34, 0x7c, 0x3c); gT[1] = C565(0x4c, 0x9c, 0x46); gT[2] = C565(0x70, 0xbc, 0x54); gT[3] = C565(0xdc, 0xf2, 0x8c); }
  static const uint16_t FC[2][4] = {
    { C565(0xff, 0xff, 0xff), C565(0xe8, 0x40, 0x3c), C565(0x58, 0x88, 0xf0), C565(0xff, 0xd8, 0x3c) },
    { C565(0xa0, 0xa8, 0xc0), C565(0x80, 0x34, 0x40), C565(0x34, 0x4c, 0x90), C565(0xa0, 0x90, 0x40) } };
  uint16_t lav = night ? C565(0x58, 0x40, 0x80) : C565(0xa8, 0x70, 0xe0);

  // seven rows of grass, back to front, each with the flowers that stand in it
  for (int r = 0; r < gMeadowRows; r++) {
    int y0 = 238 + r * 11, len = 10 + r * 4, sp = 9 + r / 2;
    float amp = 2.0f + r * 0.8f;
    sg->fillRect(0, y0 - 2, 466, 8, lerp565(soil, gT[0], 1, 3));        // shadowed root band under the blades
    if (r == 3 && gBg[1].loaded && !night) {                         // the rattata runs through the middle of the grass
      const uint32_t P = 22000;
      uint32_t u = now % (2 * P);
      bool right = u < P;
      uint32_t tri = right ? u : 2 * P - u;
      int rx = 236 + (int)(90ULL * tri / P);
      uint8_t act = right ? PMD_WALKR : PMD_WALKL;
      if (!gBg[1].has(act)) act = PMD_IDLE;
      drawPmdActZ(gBg[1], act, rx, 262, now, true, false, 2, 70, 220);
    }
    for (int i = -1; i * sp < 480; i++) {
      int x = i * sp + volNoise(i + r * 17, 45) % sp;
      if (x < 170 && y0 > 262) continue;                                  // keep the poop corner short and bare
      float wv = sinf(now / 560.0f + x / 48.0f + r * 0.5f);              // the travelling wave
      int off = (int)(wv * amp * gust) + volNoise(i + r * 7, 46) % 5 - 2;
      int bl = len - volNoise(i + r * 11, 47) % (len / 3 + 1);
      int mx = x + off * 2 / 5, my = y0 - bl * 55 / 100, tx = x + off, ty = y0 - bl;
      uint16_t c = gT[volNoise(i + r * 5, 48) % 3];
      uint16_t tip = (wv * gust > 0.8f) ? gT[3] : lerp565(c, gT[3], 1, 4); // crests glint
      sg->drawLine(x, y0 + 4, mx, my, c);
      sg->drawLine(mx, my, tx, ty, tip);
      if (r >= 3) { sg->drawLine(x + 1, y0 + 4, mx + 1, my, c); sg->drawLine(mx + 1, my, tx + 1, ty, tip); }
    }
    for (int f = 0; f < 44; f++) {                                       // wildflowers rooted in this row
      int fy = 240 + volNoise(f, 42) % 66;
      if (fy < y0 || fy >= y0 + 11) continue;
      int fx = 14 + volNoise(f, 43) % 440;
      if (fx < 170 && fy > 262) continue;
      float wv = sinf(now / 560.0f + fx / 48.0f + r * 0.5f);
      int sw = (int)(wv * (amp + 1.0f) * gust), fl = len + 8 + volNoise(f, 49) % 8;
      int hx = fx + sw, hy = fy - fl, kind = volNoise(f, 44) % 6;
      sg->drawLine(fx, fy + 2, fx + sw / 2, fy - fl / 2, gT[1]);
      sg->drawLine(fx + sw / 2, fy - fl / 2, hx, hy, gT[1]);
      if (kind == 5) {                                                    // lavender: a tall violet spike
        sg->fillRect(hx - 1, hy - 7, 3, 9, lav);
        sg->fillRect(hx - 2, hy - 4, 5, 2, lav);
      } else {
        uint16_t fc = FC[night][kind % 4];
        if (kind == 1) { sg->fillRect(hx - 3, hy - 2, 7, 5, fc); sg->fillRect(hx - 1, hy - 1, 3, 3, night ? C565(0x20, 0x10, 0x14) : C565(0x30, 0x20, 0x20)); }   // poppy
        else if (kind == 3) { sg->fillRect(hx - 2, hy - 2, 5, 5, fc); sg->fillRect(hx - 1, hy - 2, 2, 1, C565(0xff, 0xf4, 0xa0)); }                              // buttercup
        else { sg->fillRect(hx - 3, hy - 1, 7, 3, fc); sg->fillRect(hx - 1, hy - 3, 3, 7, fc); sg->fillRect(hx - 1, hy - 1, 3, 3, night ? C565(0x80, 0x70, 0x30) : C565(0xff, 0xc0, 0x20)); }   // daisy / cornflower
      }
    }
  }

  if (!night) {  // seed fluff and pollen carried along on the wind
    for (int i = 0; i < 10; i++) {
      int x = (i * 53 + (int)(now / 14) + (int)(sinf(now / 600.0f + i) * 6.0f)) % 520 - 30;
      int y = 190 + (i * 11) % 90 + (int)(sinf(now / 450.0f + i * 1.7f) * 5.0f);
      sg->fillRect(x, y, 2, 2, C565(0xff, 0xf6, 0xd0));
      sg->fillRect(x + 2, y - 1, 1, 1, C565(0xff, 0xff, 0xff));
    }
  } else {       // fireflies: bob about, each pulsing on its own beat
    uint16_t glow = C565(0xd8, 0xff, 0x70);
    for (int i = 0; i < 10; i++) {
      float pulse = (sinf(now / 350.0f + i * 2.0f) + 1.0f) * 0.5f;
      if (pulse < 0.25f) continue;
      int x = 40 + (i * 97) % 400 + (int)(sinf(now / 700.0f + i) * 14);
      int y = 200 + (i * 53) % 100 + (int)(cosf(now / 900.0f + i * 1.3f) * 10);
      sg->fillRect(x - 1, y, 5, 3, lerp565(soil, glow, (int)(pulse * 4), 16));
      sg->fillRect(x, y - 1, 3, 5, lerp565(soil, glow, (int)(pulse * 4), 16));
      sg->fillRect(x, y, 3, 3, lerp565(soil, glow, (int)(pulse * 16), 16));
    }
  }
}

// Mountain biome: high alpine country. Back to front: a hazy range, a mid range,
// one big near peak on the left (all faceted fans with snow caps and ragged snow
// hems, lit from the right), drifting fog banks, a rocky cliff on the right with
// a waterfall into a misty pool, one small pine, boulders and scree. Day: a
// FEAROW soaring and a GEODUDE trundling behind the pet; night: a GOLBAT and a
// CUBONE. The peaks stay below the sun and moon; the lower left is bare (poops
// land there, see drawPoops).

// A peak: a fan of eight facets around a centre point, dark on the left to bright
// on the lit right, then a snow cap (shaded left, lit right) with a ragged hem.
// 'haze' (0..10) fades it toward the sky for the far ones; snowPct is how far down
// the cap reaches.
void mtnPeak(int cx, int baseY, int w, int wl, int H, int seed, int haze, uint16_t sky, int snowPct, bool night) {
  static const int8_t V[8][2] = { { -100, 0 }, { -52, 46 }, { -26, 70 }, { 0, 100 }, { 24, 72 }, { 50, 44 }, { 100, 0 }, { 12, 0 } };
  uint16_t tone[4], sW, sS;
  if (night) { tone[0] = C565(0x14, 0x18, 0x2c); tone[1] = C565(0x1e, 0x24, 0x40); tone[2] = C565(0x2c, 0x34, 0x54); tone[3] = C565(0x40, 0x4a, 0x70); sW = C565(0xa0, 0xb0, 0xd8); sS = C565(0x5c, 0x6c, 0x9c); }
  else       { tone[0] = C565(0x4a, 0x4e, 0x6a); tone[1] = C565(0x6a, 0x6e, 0x86); tone[2] = C565(0x8a, 0x8e, 0xa4); tone[3] = C565(0xb4, 0xb6, 0xc8); sW = C565(0xf8, 0xfc, 0xff); sS = C565(0xc0, 0xd0, 0xea); }
  for (int i = 0; i < 4; i++) tone[i] = lerp565(tone[i], sky, haze, 10);
  sW = lerp565(sW, sky, haze, 12); sS = lerp565(sS, sky, haze, 12);
  int px[8], py[8];
  for (int i = 0; i < 8; i++) {
    int jx = volNoise(i, seed) % 9 - 4, jy = (i == 0 || i == 6 || i == 7) ? 0 : volNoise(i + 9, seed) % 7 - 3;
    px[i] = cx + (V[i][0] + jx) * (V[i][0] < 0 ? wl : w) / 100;       // wl: Breite der linken Haelfte (steilere linke Flanke)
    py[i] = baseY - (V[i][1] + jy) * H / 100;
  }
  int ccx = cx, ccy = baseY - 34 * H / 100;
  static const uint8_t TN[8] = { 0, 1, 2, 3, 2, 1, 0, 0 };         // tone of the facet starting at vertex i
  for (int i = 0; i < 8; i++) {
    int j = (i + 1) & 7;
    sg->fillTriangle(ccx, ccy, px[i], py[i], px[j], py[j], tone[TN[i]]);
  }
  // Schneekappe (seit 6.62): ihre Seitenpunkte liegen genau auf der Silhouette (Gipfel -> Schulter -> Flanke),
  // damit die Kappe nirgends ueber den Berg hinaus in die Luft ragt.
  const int pk = px[3], pyk = py[3];
  auto chainX = [&](int a, int b, int c, int ty) {                  // x auf dem Linienzug Gipfel -> a -> b -> c in der Hoehe ty
    const int cxs[4] = { pk, px[a], px[b], px[c] }, cys[4] = { pyk, py[a], py[b], py[c] };
    for (int k = 0; k < 3; k++)
      if (ty <= cys[k + 1] || k == 2) {
        int dy = cys[k + 1] - cys[k]; if (dy < 1) dy = 1;
        int t = ty - cys[k]; if (t < 0) t = 0; if (t > dy) t = dy;
        return cxs[k] + (cxs[k + 1] - cxs[k]) * t / dy;
      }
    return pk;
  };
  const int dLo = (py[1] - pyk) * snowPct / 100 * 130 / 100;        // Tiefe der Kappe am Rand
  const int dMid = dLo * 55 / 100;
  const int pcx = pk, pcy = pyk + (baseY - pyk) * snowPct / 160;    // Mitte der unteren Kante
  int sx[5], sy[5];
  sy[0] = pyk + dLo;  sx[0] = chainX(2, 1, 0, sy[0]);
  sy[1] = pyk + dMid; sx[1] = chainX(2, 1, 0, sy[1]);
  sx[2] = pcx; sy[2] = pcy;
  sy[3] = pyk + dMid; sx[3] = chainX(4, 5, 6, sy[3]);
  sy[4] = pyk + dLo;  sx[4] = chainX(4, 5, 6, sy[4]);
  const int mx = pk, my = pyk + (pcy - pyk) / 2;                    // Faecher vom Innenpunkt aus
  sg->fillTriangle(mx, my, pk, pyk, sx[1], sy[1], sS);
  sg->fillTriangle(mx, my, sx[1], sy[1], sx[0], sy[0], sS);
  sg->fillTriangle(mx, my, sx[0], sy[0], sx[2], sy[2], sS);
  sg->fillTriangle(mx, my, sx[2], sy[2], sx[4], sy[4], sW);
  sg->fillTriangle(mx, my, sx[4], sy[4], sx[3], sy[3], sW);
  sg->fillTriangle(mx, my, sx[3], sy[3], pk, pyk, sW);
  sg->drawLine(pk, pyk, sx[1], sy[1], sS); sg->drawLine(sx[1], sy[1], sx[0], sy[0], sS);   // Rand der Kappe deckt die Silhouettenkante vollstaendig ab
  sg->drawLine(pk, pyk, sx[3], sy[3], sW); sg->drawLine(sx[3], sy[3], sx[4], sy[4], sW);
  for (int k = 0; k < 4; k++)                                        // ragged hem: teeth hanging off the lower edge
    for (int t = 0; t < 3; t++) {
      int ax = sx[k] + (sx[k + 1] - sx[k]) * t / 3, ay = sy[k] + (sy[k + 1] - sy[k]) * t / 3;
      int bx = sx[k] + (sx[k + 1] - sx[k]) * (t + 1) / 3, by = sy[k] + (sy[k + 1] - sy[k]) * (t + 1) / 3;
      sg->fillTriangle(ax, ay, bx, by, (ax + bx) / 2, (ay + by) / 2 + 3 + volNoise(k * 3 + t, seed + 3) % 6, k < 2 ? sS : sW);
    }
}

// A grey boulder with a lit upper right, shaded lower left, and a tuft on top.
void mtnBoulder(int cx, int cy, int r, int seed, bool night) {
  uint16_t t[4];
  if (night) { t[0] = C565(0x18, 0x1c, 0x2c); t[1] = C565(0x28, 0x2e, 0x44); t[2] = C565(0x3a, 0x42, 0x5c); t[3] = C565(0x50, 0x5a, 0x78); }
  else       { t[0] = C565(0x5c, 0x58, 0x5c); t[1] = C565(0x7c, 0x78, 0x78); t[2] = C565(0x9c, 0x96, 0x92); t[3] = C565(0xc0, 0xb8, 0xae); }
  const int N = 8;
  int vx[N + 1], vy[N + 1];
  for (int i = 0; i < N; i++) {
    float an = i * 6.2832f / N;
    int rr = r * (80 + volNoise(i, seed) % 26) / 100;
    vx[i] = cx + (int)(cosf(an) * rr * 1.25f);
    vy[i] = cy + (int)(sinf(an) * rr * 0.8f);
  }
  vx[N] = vx[0]; vy[N] = vy[0];
  for (int i = 0; i < N; i++) {
    float an = (i + 0.5f) * 6.2832f / N, lit = cosf(an) * 0.6f - sinf(an) * 0.8f;
    sg->fillTriangle(cx, cy - r / 6, vx[i], vy[i], vx[i + 1], vy[i + 1], lit > 0.75f ? t[3] : (lit > 0.2f ? t[2] : (lit > -0.4f ? t[1] : t[0])));
  }
  uint16_t g = night ? C565(0x1c, 0x3a, 0x2c) : C565(0x58, 0x9c, 0x4c);                // moss on top
  sg->fillRect(cx - r / 3, cy - r * 4 / 5, r * 2 / 3, 2, g);
  sg->fillRect(cx - r / 4, cy - r * 4 / 5 - 2, r / 2, 2, g);
}

void drawMountainB(uint32_t now, bool night, int h, uint16_t top, uint16_t bot, uint16_t soil) {
  auto skyAt = [&](int y) { return lerp565(top, bot, y < 0 ? 0 : y, HORIZON); };
  const int B = HORIZON + 2;
  float sway = sinf(now / 1500.0f) * 3.0f;
  uint16_t sW = night ? C565(0xa0, 0xb0, 0xd8) : C565(0xf8, 0xfc, 0xff);

  // three ranges, back to front: hazy and low, mid, then the big near peak on the left
  uint16_t hz = skyAt(HORIZON - 50);
  for (int i = 0; i < 6; i++)
    mtnPeak(10 + i * 88, B, 56 + volNoise(i, 61) % 30, 56 + volNoise(i, 61) % 30, 46 + volNoise(i, 62) % 34, 61 + i, 7, hz, 42, night);
  mtnPeak(60, B, 110, 110, 78, 71, 4, hz, 40, night);
  mtnPeak(300, B, 90, 90, 52, 72, 4, hz, 44, night);                 // below the sun and moon
  mtnPeak(430, B, 80, 80, 48, 73, 4, hz, 44, night);
  if (!gBattleScene) mtnPeak(89, B, 124, 93, 118, 81, 0, hz, 44, night);   // the big one, lit from the right (Uhransicht seit 6.57: Gipfel links der Uhr, flacher)

  // fog banks drifting across the ranges
  uint16_t fog = lerp565(skyAt(HORIZON - 20), C565(0xff, 0xff, 0xff), 1, night ? 9 : 4);
  for (int k = 0; k < 3; k++) {
    int x = (int)((k * 190 + now / 90) % 640) - 150;
    snowEll(x, HORIZON - 18 + k * 9, 100 + k * 14, 4, fog);
    snowEll(x + 30, HORIZON - 21 + k * 9, 60, 3, fog);
  }

  if (!gBattleScene) {
  // the cliff on the right: ragged-edged strata, shaded left edge, lit right side, grass on top
  uint16_t rc[4];
  if (night) { rc[0] = C565(0x14, 0x18, 0x28); rc[1] = C565(0x20, 0x26, 0x3c); rc[2] = C565(0x2e, 0x36, 0x50); rc[3] = C565(0x42, 0x4c, 0x6c); }
  else       { rc[0] = C565(0x4c, 0x44, 0x48); rc[1] = C565(0x6c, 0x62, 0x60); rc[2] = C565(0x88, 0x7c, 0x74); rc[3] = C565(0xa8, 0x98, 0x88); }
  const int CT = 178, CB = 266;                                     // cliff top and foot
  for (int y = CT; y < CB; y += 2) {
    int xl = 376 + volNoise(y / 4, 63) % 9 - (y - CT) / 14;          // ragged, flaring out toward the foot
    int band = (y / 12 + volNoise(y / 12, 64) % 2) % 3;               // strata
    sg->fillRect(xl, y, 12, 2, rc[0]);
    sg->fillRect(xl + 12, y, 466 - xl - 12, 2, rc[band == 2 ? 2 : 1]);
    sg->fillRect(430, y, 40, 2, rc[band == 0 ? 2 : 3]);             // the lit side
    if (volNoise(y, 65) < 50) sg->fillRect(xl + 14 + volNoise(y, 66) % 40, y, 3, 2, rc[0]);   // cracks and pits
  }
  uint16_t gTop = night ? C565(0x1c, 0x3c, 0x2a) : C565(0x58, 0xa0, 0x4c);
  uint16_t gTopL = night ? C565(0x2a, 0x54, 0x38) : C565(0x86, 0xcc, 0x58);
  sg->fillRect(372, CT - 5, 100, 6, gTop);
  sg->fillRect(420, CT - 5, 50, 3, gTopL);
  for (int x = 374; x < 466; x += 6)                                 // grass hanging over the edge
    sg->fillTriangle(x, CT, x + 6, CT, x + 3, CT + 4 + volNoise(x, 67) % 5, gTop);

  // the waterfall: a pale ribbon from a notch in the lip to a misty pool, with bright
  // streaks racing down it and spray rising at the foot
  const int WX = 414;
  uint16_t wt = night ? C565(0x70, 0x90, 0xc8) : C565(0xb8, 0xdc, 0xf4);
  sg->fillRect(WX - 5, CT - 3, 10, 6, lerp565(rc[0], wt, 1, 2));    // the notch
  for (int y = CT + 2; y < CB + 6; y += 2) {
    int wob = (int)(sinf(y / 14.0f + now / 500.0f) * 1.0f);
    sg->fillRect(WX - 4 + wob, y, 8, 2, wt);
    sg->fillRect(WX + 2 + wob, y, 2, 2, lerp565(wt, C565(0x30, 0x58, 0x88), 1, 4));   // shaded edge
  }
  int wlen = CB - CT;
  for (int k = 0; k < 5; k++) {
    int yy = CT + 4 + (int)((now / 9 + k * 37) % wlen);
    sg->fillRect(WX - 3 + (k * 3) % 5, yy, 2, 8, C565(0xff, 0xff, 0xff));
  }
  snowEll(WX + 4, 270, 46, 8, night ? C565(0x3a, 0x4a, 0x70) : C565(0x78, 0x9c, 0xc0));            // pool rim
  snowEll(WX + 4, 270, 42, 6, night ? C565(0x1a, 0x2c, 0x58) : C565(0x4c, 0x80, 0xb4));            // water
  for (int k = 0; k < 3; k++) {                                                                      // ripples spreading out from the splash
    int r = (int)((now / 40 + k * 14) % 42);
    sg->fillRect(WX + 4 - r, 270, 1, 1, lerp565(C565(0x4c, 0x80, 0xb4), sW, 2, 3));
    sg->fillRect(WX + 4 + r, 270, 1, 1, lerp565(C565(0x4c, 0x80, 0xb4), sW, 2, 3));
  }
  for (int k = 0; k < 6; k++) {                                                                      // spray: puffs rising and thinning
    int life = (int)((now / 26 + k * 40) % 120);
    int px = WX + (int)(sinf(k * 2.1f + now / 400.0f) * (8 + life / 8)), py = 266 - life / 3;
    sg->fillCircle(px, py, 3 + life / 24, lerp565(skyAt(py), C565(0xff, 0xff, 0xff), 8 - life / 18, 14));
  }

  }
  // an alpine pine at the cliff foot, boulders around it; tufts and scree on the ground
  if (!gBattleScene) forestPine(380, 270, 52, 34, sway * 0.6f, night);
  mtnBoulder(352, 292, 12, 1, night);
  mtnBoulder(300, 300, 10, 2, night);
  mtnBoulder(460, 296, 11, 3, night);
  uint16_t gD = night ? C565(0x14, 0x30, 0x24) : C565(0x4c, 0x86, 0x44);
  for (int i = 0; i < 16; i++) {
    int x = 190 + volNoise(i, 68) % 220, y = 250 + volNoise(i, 69) % 50;
    if (x > 372 && y < 270) continue;
    int sw = (int)(sinf(now / 600.0f + x / 40.0f) * 1.5f);
    sg->drawLine(x, y, x - 3 + sw, y - 6, gD);
    sg->drawLine(x + 1, y, x + 1 + sw, y - 8, gD);
    sg->drawLine(x + 2, y, x + 5 + sw, y - 5, gD);
  }
  for (int i = 0; i < 26; i++) {                                    // scree: scattered pebbles
    int x = 140 + volNoise(i, 70) % 320, y = 240 + volNoise(i, 71) % 62;
    if ((x < 200 && y > 262) || (x > 366 && y < 274)) continue;
    sg->fillRect(x, y, 2 + i % 2, 2, night ? C565(0x2a, 0x30, 0x48) : (i & 1 ? C565(0x84, 0x7c, 0x78) : C565(0xb8, 0xae, 0xa0)));
  }
  static const int16_t EW[5][2] = { { 268, 262 }, { 318, 278 }, { 232, 296 }, { 340, 258 }, { 280, 292 } };   // edelweiss
  for (int i = 0; i < 5; i++) {
    int x = EW[i][0], y = EW[i][1];
    sg->drawLine(x, y + 2, x, y - 4, gD);
    sg->fillRect(x - 2, y - 6, 5, 3, night ? C565(0x90, 0xa0, 0xc0) : C565(0xff, 0xff, 0xf0));
    sg->fillRect(x, y - 5, 1, 1, C565(0xe0, 0xc0, 0x40));
  }

  // residents: FEAROW soars by day / GOLBAT at night, a GEODUDE or CUBONE trundles behind the pet
  if (gBg[0].loaded) {                                            // a long slow glide, turning at each end, clear of the moon
    const uint32_t P = 20000;
    uint32_t u = now % (2 * P);
    bool right = u < P;
    uint32_t tri = right ? u : 2 * P - u;
    int x = 60 + (int)(250ULL * tri / P), y = 168 + (int)(sinf(now / 1400.0f) * 8.0f);
    uint8_t act = right ? PMD_WALKR : PMD_WALKL;
    if (!gBg[0].has(act)) act = PMD_IDLE;
    drawPmdActZ(gBg[0], act, x, y, now, true, false, 1, 100, 220);
  }
  if (gBg[1].loaded) {
    const uint32_t P = 26000;
    uint32_t u = now % (2 * P);
    bool right = u < P;
    uint32_t tri = right ? u : 2 * P - u;
    int x = 240 + (int)(80ULL * tri / P);
    uint8_t act = right ? PMD_WALKR : PMD_WALKL;
    if (!gBg[1].has(act)) act = PMD_IDLE;
    drawPmdActZ(gBg[1], act, x, 262, now, true, false, 2, 70, 220);
  }
}

// Graveyard biome for ghost types: always dusky, never cheerful. Back to front:
// bare gnarled trees on the horizon, a haunted house with flickering windows on a
// far hill, drifting fog, an iron fence, weathered gravestones (round, cross,
// obelisk, slab) with moss and carved lines, scattered bones, more fog, and a few
// will-o'-wisps. A small GASTLY (HAUNTER at night) drifts about; at night two bats
// cross the sky. The lower left is bare (poops land there, see
// drawPoops), and the sun and moon stay clear.

// One gravestone, lit cold from the upper right: a stone body per 'kind', a lit
// right edge, a shadowed left, moss at the foot, a hairline crack, carved lines.
// kind: 0 rounded, 1 cross, 2 obelisk, 3 slab (leaning by 'lean' px).
void graveStone(int cx, int footY, int kind, int w, int h, int lean, int seed, bool night) {
  uint16_t stL = night ? C565(0x5c, 0x64, 0x88) : C565(0xa8, 0xa2, 0xb8);
  uint16_t stM = night ? C565(0x40, 0x48, 0x68) : C565(0x84, 0x7e, 0x98);
  uint16_t stD = night ? C565(0x26, 0x2c, 0x48) : C565(0x5a, 0x56, 0x6c);
  uint16_t moss = night ? C565(0x1e, 0x3a, 0x34) : C565(0x5c, 0x8c, 0x58);
  uint16_t earth = night ? C565(0x1c, 0x1a, 0x2a) : C565(0x44, 0x40, 0x52);
  snowEll(cx, footY + 1, w * 70 / 100, 4, earth);                              // the mound it stands in
  int x0 = cx - w / 2, top = footY - h;
  if (kind == 0) {                                                            // rounded headstone
    sg->fillRect(x0, top + w / 2, w, h - w / 2, stM);
    sg->fillCircle(cx, top + w / 2, w / 2, stM);
    sg->fillRect(x0 + w - 3, top + w / 2, 3, h - w / 2, stL);
    sg->fillRect(x0, top + w / 2, 3, h - w / 2, stD);
    sg->fillRect(cx - 2, top + 4, 4, 5, stD);                                // a small carved cross
    sg->fillRect(cx - 1, top + 2, 2, 9, stD);
  } else if (kind == 1) {                                                     // a cross
    int bw = w / 3;
    sg->fillRect(cx - bw / 2, top, bw, h, stM);
    sg->fillRect(x0, top + h / 4, w, bw, stM);
    sg->fillRect(cx + bw / 2 - 2, top, 2, h, stL);
    sg->fillRect(x0 + w - 2, top + h / 4, 2, bw, stL);
    sg->fillRect(cx - bw / 2, top + h / 4 + bw, 2, h - h / 4 - bw, stD);
  } else if (kind == 2) {                                                     // an obelisk on a plinth
    int pw = w, ph = h / 5;
    sg->fillRect(cx - pw / 2, footY - ph, pw, ph, stM);
    sg->fillRect(cx - pw / 2, footY - ph, pw, 2, stL);
    int bw = w * 60 / 100;
    sg->fillRect(cx - bw / 2, top + bw, bw, h - ph - bw, stM);
    sg->fillTriangle(cx - bw / 2, top + bw, cx + bw / 2, top + bw, cx, top, stL);
    sg->fillRect(cx + bw / 2 - 3, top + bw, 3, h - ph - bw, stL);
    sg->fillRect(cx - bw / 2, top + bw, 3, h - ph - bw, stD);
  } else {                                                                    // a slab, leaning, a corner chipped off
    for (int y = 0; y < h; y += 2) {
      int xo = x0 + lean * (h - y) / h;
      sg->fillRect(xo, top + y, w, 2, stM);
      sg->fillRect(xo + w - 3, top + y, 3, 2, stL);
      sg->fillRect(xo, top + y, 3, 2, stD);
    }
    sg->fillTriangle(x0 + w - 5 + lean, top, x0 + w + lean, top, x0 + w + lean, top + 5, earth);
  }
  sg->fillRect(cx - w / 2, footY - 4, w, 4, moss);                           // moss creeping up the foot
  for (int i = 0; i < 4; i++) sg->fillRect(cx - w / 2 + volNoise(i, seed) % w, footY - 4 - volNoise(i + 5, seed) % 5, 2, 2, moss);
  sg->drawLine(cx - 2, top + h / 2, cx + 3, top + h * 3 / 4, stD);           // a hairline crack
  if (kind != 1) for (int i = 0; i < 2; i++) sg->fillRect(cx - w / 4, top + h / 2 + i * 4 - 2, w / 2, 1, stD);   // carved lines
}

// A will-o'-wisp: a flickering teardrop flame, violet outside, white-hot inside.
void graveWisp(int x, int y, uint32_t now, int seed, bool night) {
  float fl = sinf(now / 90.0f + seed * 2.3f);
  int r = 3 + (fl > 0.3f);
  uint16_t out = night ? C565(0x90, 0x70, 0xe8) : C565(0xa8, 0x98, 0xd0);
  sg->fillTriangle(x - r, y, x + r, y, x + (int)(fl * 2), y - r * 3, out);
  sg->fillCircle(x, y + 1, r, out);
  sg->fillTriangle(x - r / 2, y, x + r / 2, y, x + (int)(fl * 1.5f), y - r * 2, C565(0xe4, 0xd8, 0xff));
  sg->fillRect(x - 1, y, 2, 2, C565(0xff, 0xff, 0xff));
}

// Bones for the graveyard floor. Ivory, lit on the upper right, shaded below.
void graveBoneCols(bool night, uint16_t c[3]) {
  if (night) { c[0] = C565(0xb4, 0xbc, 0xd4); c[1] = C565(0x7c, 0x86, 0xa4); c[2] = C565(0x2a, 0x30, 0x4c); }
  else       { c[0] = C565(0xf0, 0xe8, 0xd0); c[1] = C565(0xbc, 0xb0, 0x94); c[2] = C565(0x60, 0x56, 0x4a); }
}

// A long bone (femur): a shaded shaft with a lit edge and a knobbed pair at each end.
void graveFemur(int x0, int y0, int x1, int y1, bool night) {
  uint16_t c[3]; graveBoneCols(night, c);
  sg->drawLine(x0, y0 + 1, x1, y1 + 1, c[1]);
  sg->drawLine(x0, y0, x1, y1, c[0]);
  int dx = x1 - x0, dy = y1 - y0, len = (int)sqrtf((float)(dx * dx + dy * dy));
  if (len < 1) return;
  int px = -dy * 2 / len, py = dx * 2 / len;                                 // perpendicular, 2 px
  for (int e = 0; e < 2; e++) {
    int ex = e ? x1 : x0, ey = e ? y1 : y0;
    sg->fillCircle(ex + px, ey + py, 2, c[0]);
    sg->fillCircle(ex - px, ey - py, 2, c[1]);
  }
}

// A skull, optionally sunk into the ground up to the jaw.
void graveSkull(int x, int y, int r, bool buried, bool night, uint16_t earth) {
  uint16_t c[3]; graveBoneCols(night, c);
  sg->fillCircle(x, y, r, c[0]);
  sg->fillCircle(x - r / 4, y + r / 4, r * 3 / 4, c[1]);                    // shaded lower left
  sg->fillCircle(x + r / 5, y - r / 5, r * 3 / 4, c[0]);                    // lit upper right
  sg->fillRect(x - r * 2 / 3, y + r / 2, r * 4 / 3, r / 2 + 1, c[1]);       // jaw
  for (int t = 0; t < 4; t++) sg->fillRect(x - r / 2 + t * (r / 3 + 1), y + r / 2 + 1, 1, r / 3, c[2]);   // teeth
  sg->fillRect(x - r / 2 - 1, y - r / 6, r * 2 / 5 + 2, r / 2 + 1, c[2]);   // eye sockets
  sg->fillRect(x + r / 10, y - r / 6, r * 2 / 5 + 2, r / 2 + 1, c[2]);
  sg->fillTriangle(x - 1, y + r / 3, x + 2, y + r / 3, x, y + r / 6, c[2]);   // nose
  if (buried) snowEll(x, y + r, r + 3, r / 2 + 1, earth);
}

// Half a ribcage: a short spine with curved ribs both sides, shrinking toward the bottom.
void graveRibs(int x, int y, bool night) {
  uint16_t c[3]; graveBoneCols(night, c);
  sg->drawLine(x, y, x, y + 20, c[1]);
  sg->drawLine(x + 1, y, x + 1, y + 20, c[0]);
  for (int k = 0; k < 5; k++)
    for (int side = -1; side <= 1; side += 2) {
      int ry = y + 2 + k * 4, w = 11 - k;
      sg->drawLine(x, ry, x + side * w, ry + 3, side > 0 ? c[0] : c[1]);
      sg->drawLine(x + side * w, ry + 3, x + side * (w - 2), ry + 8, side > 0 ? c[0] : c[1]);
    }
}

// A row of vertebrae curving across the ground.
void graveSpine(int x, int y, bool night) {
  uint16_t c[3]; graveBoneCols(night, c);
  for (int i = 0; i < 7; i++) {
    int vx = x + i * 5, vy = y + (int)(sinf(i * 0.7f) * 3.0f);
    sg->fillRect(vx, vy, 4, 3, c[i & 1 ? 1 : 0]);
    sg->fillRect(vx + 1, vy - 2, 2, 2, c[1]);                              // the spinous process
  }
}

void drawGraveyardB(uint32_t now, bool night, int h, uint16_t top, uint16_t bot, uint16_t soil) {
  auto skyAt = [&](int y) { return lerp565(top, bot, y < 0 ? 0 : y, HORIZON); };
  const int B = HORIZON + 2;
  uint16_t hz = skyAt(HORIZON - 30);
  uint16_t dark = night ? C565(0x0c, 0x0a, 0x1c) : C565(0x30, 0x2a, 0x40);

  // bare gnarled trees along the horizon, hazed toward the sky
  uint16_t farT = lerp565(dark, hz, 1, 3);
  for (int i = 0; i < 9; i++) {
    int x = 14 + i * 56 + volNoise(i, 81) % 20, ht = 34 + volNoise(i, 82) % 30;
    sg->drawLine(x, B, x + volNoise(i, 83) % 5 - 2, B - ht, farT);
    sg->drawLine(x + 1, B, x + 1 + volNoise(i, 83) % 5 - 2, B - ht, farT);
    for (int b = 0; b < 4; b++) {                                              // crooked branches, alternating sides
      int by = B - ht * (30 + b * 18) / 100, dir = (b & 1) ? 1 : -1, len = 8 + volNoise(i + b, 84) % 10;
      sg->drawLine(x, by, x + dir * len, by - len / 2 - 3, farT);
      sg->drawLine(x + dir * len, by - len / 2 - 3, x + dir * (len + 5), by - len - 4, farT);
    }
  }

  if (!gBattleScene) {
  // the haunted house on a far hill: a main block, a roof with a gable, a tower, lit windows
  // Nur in der Uhransicht (seit 6.57): Haus um 180 px nach links, damit es nicht in die Uhr (x 128..338) ragt
  // seit 6.61 auch im Hauptbild (nicht nur in der Uhransicht); Kampf/Ausflug zeichnen das Haus nicht
  const int HX = -180;
  snowEll(280 + HX, B, 100, 12, lerp565(dark, hz, 1, 4));
  uint16_t hs = lerp565(dark, hz, 1, 5);
  sg->fillRect(246 + HX, B - 40, 70, 40, hs);                                      // main block
  sg->fillTriangle(240 + HX, B - 40, 322 + HX, B - 40, 281 + HX, B - 62, hs);               // roof
  sg->fillRect(252 + HX, B - 66, 16, 26, hs);                                      // tower
  sg->fillTriangle(249 + HX, B - 66, 271 + HX, B - 66, 260 + HX, B - 88, hs);               // its spire
  sg->fillRect(300 + HX, B - 58, 6, 14, hs);                                       // chimney
  uint16_t win = night ? C565(0xff, 0xd0, 0x70) : C565(0xd8, 0xc0, 0x80);
  static const int16_t WN[5][2] = { { 256, 56 }, { 262, 28 }, { 274, 24 }, { 290, 24 }, { 303, 24 } };   // x, height above the base
  for (int i = 0; i < 5; i++) {
    int flick = volNoise((int)(now / 160) + i * 7, 85);                         // some windows flicker out for a beat
    if (flick < (night ? 26 : 90)) continue;
    sg->fillRect(WN[i][0] + HX, B - WN[i][1], 5, 7, i == 1 ? C565(0xa8, 0x80, 0xff) : win);
  }

  }

  // low fog behind the fence
  uint16_t fog = lerp565(soil, C565(0xd0, 0xc8, 0xf0), 1, night ? 7 : 4);
  for (int k = 0; k < 3; k++) {
    int x = (int)((k * 220 + now / (60 + k * 17)) % 700) - 170;
    snowEll(x, B + 6 + k * 6, 110 + k * 10, 5, fog);
  }

  // iron fence along the back: pointed bars between two rails, with taller posts
  uint16_t fc = night ? C565(0x10, 0x10, 0x20) : C565(0x2e, 0x2a, 0x3c);
  uint16_t fl = night ? C565(0x34, 0x3c, 0x60) : C565(0x6c, 0x66, 0x80);
  sg->fillRect(0, B + 6, 466, 2, fc);
  sg->fillRect(0, B + 16, 466, 2, fc);
  for (int x = 4; x < 466; x += 9) {
    sg->fillRect(x, B, 2, 22, fc);
    sg->fillTriangle(x - 1, B, x + 3, B, x + 1, B - 5, fc);
    sg->fillRect(x + 1, B + 1, 1, 18, fl);                                    // lit edge
  }
  for (int x = 40; x < 466; x += 117) {                                          // taller posts with ball tops
    sg->fillRect(x, B - 8, 5, 30, fc);
    sg->fillCircle(x + 2, B - 10, 4, fc);
    sg->fillRect(x + 3, B - 6, 2, 26, fl);
  }

  // gravestones: three rows back to front, nothing in the poop corner
  graveStone(196, B + 20, 0, 22, 28, 0, 1, night);
  graveStone(262, B + 18, 1, 26, 34, 0, 2, night);
  graveStone(330, B + 20, 2, 22, 40, 0, 3, night);
  graveStone(404, B + 22, 3, 24, 28, 4, 4, night);
  graveStone(300, B + 40, 3, 26, 28, -5, 5, night);
  graveStone(372, B + 46, 0, 30, 36, 0, 6, night);
  graveStone(442, B + 40, 1, 26, 32, 0, 7, night);

  // bones scattered about the ground, clear of the poop corner
  {
    uint16_t earth = night ? C565(0x1c, 0x1a, 0x2a) : C565(0x44, 0x40, 0x52);
    graveSpine(178, 274, night);
    graveFemur(288, 268, 312, 261, night);
    graveSkull(356, 262, 6, true, night, earth);
    graveSkull(330, 284, 8, false, night, earth);
    graveFemur(380, 288, 406, 280, night);
    graveFemur(388, 280, 402, 292, night);                                  // the two cross in the middle
    graveRibs(422, 262, night);
  }

  // fog drifting in front of the stones
  for (int k = 0; k < 3; k++) {
    int x = (int)((k * 240 + 200 + now / (45 + k * 13)) % 720) - 180;
    snowEll(x, 286 + k * 8, 130 + k * 12, 6, lerp565(soil, C565(0xd0, 0xc8, 0xf0), 1, night ? 6 : 4));
  }

  // wisps drifting among the graves, more of them at night
  for (int i = 0; i < (night ? 5 : 2); i++) {
    int x = 190 + (int)(sinf(now / 2100.0f + i * 1.9f) * 80.0f) + i * 36;
    int y = 262 - (int)((sinf(now / 1300.0f + i * 2.4f) + 1.0f) * 12.0f) - i * 4;
    if (x < 170 && y > 250) continue;
    graveWisp(x, y, now, i, night);
  }

  // the small ghost: GASTLY by day, HAUNTER at night, drifting about the upper air
  if (gBg[0].loaded) {
    float ph = now / 7400.0f;
    int x = 180 + (int)(110.0f * sinf(ph)), y = 176 + (int)(12.0f * sinf(now / 1900.0f));
    uint8_t act = cosf(ph) > 0 ? PMD_WALKR : PMD_WALKL;
    if (!gBg[0].has(act)) act = PMD_IDLE;
    drawPmdActZ(gBg[0], act, x, y, now, true, false, 1, night ? 80 : 100, 220);
  }

  // two bats crossing the night sky
  if (night)
    for (int g = 0; g < 2; g++) {
      int x = 520 - (int)(now / 22 + g * 300) % 640, y = 96 + g * 30 + (int)(sinf(now / 500.0f + g * 2.0f) * 8.0f);
      int up = ((now / 110 + g) & 1) ? -5 : 3;
      uint16_t bc = C565(0x0a, 0x08, 0x14);
      sg->fillRect(x - 1, y - 1, 3, 3, bc);
      sg->drawLine(x - 8, y + up, x - 1, y, bc);
      sg->drawLine(x + 1, y, x + 8, y + up, bc);
      sg->drawLine(x - 8, y + up, x - 5, y + up + 3, bc);
      sg->drawLine(x + 8, y + up, x + 5, y + up + 3, bc);
    }
}

enum { BIO_MEADOW, BIO_BEACH, BIO_FOREST, BIO_VOLCANO, BIO_MOUNTAIN, BIO_SNOW, BIO_GRAVEYARD, BIO_COUNT_B };

// Boden je Bioma in B (Tag; nachts mischt drawScene zum Nachtblau)
static const uint16_t BIOME_SOIL_B[BIO_COUNT_B] = {
  C565(0x7e, 0xc0, 0x7f),  // Wiese
  C565(0xdc, 0xca, 0x94),  // Strand (Sand)
  C565(0x3f, 0x78, 0x4c),  // Wald
  C565(0x5e, 0x40, 0x3c),  // Vulkan (dunkler Basalt)
  C565(0xa8, 0x90, 0x6a),  // Gebirge
  C565(0xe6, 0xee, 0xf5),  // Schnee
  C565(0x5e, 0x5a, 0x6e),  // Friedhof (kalte violett-graue Erde)
};

// Welches Bioma zeigt die Szene fuer diese Art? In A gibt es nur 0..5 (Wert aus
// dex.h), dazu Unlicht-Pokemon im Wald (2, in beiden Looks). In B sitzen Geist-Pokemon im Friedhof (6). Die Wahl haengt nur vom
// gerade aktiven Look ab und wird jedes Bild neu bestimmt: ein Wechsel A<->B
// hinterlaesst also keinen Zustand und kann nie ein Bioma ausserhalb des
// jeweiligen Wertebereichs liefern.
uint8_t biomeFor(int16_t dex) {
  if (dex < 1 || dex > DEX_COUNT) return 0;
  if (LOOK_B && DEX_TBL[dex].type == TYPE_GHOST) return BIO_GRAVEYARD;
  if (DEX_TBL[dex].type == TYPE_DARK) return BIO_FOREST;  // Unlicht lebt im Wald (A und B; ab 6.21)
  uint8_t b = DEX_TBL[dex].biome;
  return b < 6 ? b : 0;
}

// Himmel je Bioma leicht einfaerben (B)
void tintSkyB(uint8_t biome, bool night, uint16_t &top, uint16_t &bot) {
  if (biome == BIO_GRAVEYARD) {  // immer duester, auch mittags
    top = lerp565(top, night ? C565(0x1c, 0x10, 0x34) : C565(0x4c, 0x40, 0x6c), night ? 1 : 3, night ? 2 : 4);
    bot = lerp565(bot, night ? C565(0x42, 0x2c, 0x5c) : C565(0xb4, 0x9c, 0xc4), 1, 2);
  }
  if (biome == BIO_SNOW) {  // kalt, bedeckt
    top = lerp565(top, night ? C565(0x0a, 0x18, 0x30) : C565(0xa8, 0xc0, 0xd8), 1, 3);
    bot = lerp565(bot, night ? C565(0x2a, 0x3c, 0x66) : C565(0xea, 0xf2, 0xfa), 1, 2);
  }
  if (biome == BIO_FOREST && !night) bot = lerp565(bot, C565(0xcc, 0xe8, 0xd4), 1, 3);  // leichter gruener Dunst
  if (biome == BIO_VOLCANO) {  // Asche-roter Himmel, zum Horizont dunstiger
    top = lerp565(top, night ? C565(0x2a, 0x0e, 0x14) : C565(0x6a, 0x2c, 0x2c), 1, 2);
    bot = lerp565(bot, night ? C565(0x6a, 0x24, 0x1c) : C565(0xf0, 0x7c, 0x3c), 1, 2);
  }
}

// Boden und Landschaft des Biomas in B
void drawBiomeB(uint8_t biome, uint32_t now, bool night, int h, uint16_t top, uint16_t bot, uint16_t soil) {
  if (biome == BIO_BEACH) {  // der Strand zeichnet Meer, Sand und Gezeiten selbst
    drawBeachB(now, night, h, top, bot, soil);
    return;
  }
  sg->fillRect(0, HORIZON, 466, 466 - HORIZON, soil);
  uint16_t hill = lerp565(soil, night ? C565(0x0c, 0x12, 0x24) : C565(0xff, 0xff, 0xff), 3, 16);
  sg->fillRoundRect(-60, HORIZON - 14, 586, 60, 30, hill);
  if (biome == BIO_FOREST) drawForestB(now, night, h, top, bot, soil);
  else if (biome == BIO_VOLCANO) drawVolcanoB(now, night, top, bot);
  else if (biome == BIO_MOUNTAIN) drawMountainB(now, night, h, top, bot, soil);
  else if (biome == BIO_SNOW) drawSnowB(now, night, h, top, bot, soil);
  else if (biome == BIO_GRAVEYARD) drawGraveyardB(now, night, h, top, bot, soil);
  else drawMeadowB(now, night, h, top, bot, soil);
}

int gSunUp = 0;   // seit 6.61: Abendsonne (Berg/Vulkan/Friedhof/Wald) so viel hoeher, nur waehrend der Pokemon-Ausflug-Seite zeichnet
int gSunDX = 0, gSunDY = 0;   // Optik B: Sonne/Mond verschoben (nur waehrend die Ausflug-Seiten zeichnen)
int gSkyH = 232;   // Himmelshoehe von drawScene (HORIZON, im Kampf/Ballspiel HORIZON + gSceneDY)
void drawScene(uint8_t biome, uint32_t now, bool night) {
  int h = sceneHour();
  if (LOOK_B) { if (biome >= BIO_COUNT_B) biome = 0; }
  else if (biome > 5) biome = 0;  // A kennt nur sechs Biome
  gSkyH = gBattleScene ? HORIZON + gSceneDY : HORIZON;   // Hoehe des Himmels (im Kampf/Ballspiel tiefer)
  bgSelect((LOOK_B && !gBattleScene) ? biome : 255, night);   // im Kampf keine Hintergrundtiere  // Tiere nur in B; in A werden sie freigegeben
  uint16_t top, bot;
  if (night)            { top = C565(0x0c, 0x12, 0x24); bot = C565(0x1e, 0x26, 0x46); }
  else if (h < 8)       { top = C565(0xd1, 0x6a, 0x86); bot = C565(0xf3, 0xb8, 0x7c); }  // amanecer
  else if (h < 18)      { top = C565(0x8f, 0xc8, 0xea); bot = C565(0xdc, 0xee, 0xe6); }  // dia
  else                  { top = C565(0xc7, 0x5a, 0x4a); bot = C565(0xf0, 0xae, 0x64); }  // atardecer

  if (LOOK_B) tintSkyB(biome, night, top, bot);

  // cielo en bandas
  for (int y = 0; y < gSkyH; y += 8)
    gfx->fillRect(0, y, 466, 8, lerp565(top, bot, y, gSkyH));

  // sol o luna
  if (night) {
    if (LOOK_B) {
      drawStarsB(now, top, bot, gSkyH - 10);
      drawMoonB(360 + gSunDX, 78 + gSunDY, lerp565(top, bot, 78 + gSunDY, gSkyH));
    } else {
      gfx->fillCircle(360, 78, 24, C565(0xe8, 0xee, 0xf5));
      gfx->fillCircle(370, 72, 22, lerp565(top, bot, 78, gSkyH));  // creciente
      for (auto &st : STARS) gfx->fillRect(st[0], st[1], 4, 4, UI_WHITE);
    }
  } else if (h < 18) {
    if (LOOK_B)
      drawSunB(360 + gSunDX, 84 + gSunDY, 24, h < 8 ? C565(0xff, 0xc8, 0x6a) : C565(0xff, 0xd9, 0x5c),
               lerp565(top, bot, 84 + gSunDY, gSkyH), now);
    else
      gfx->fillCircle(360, 84, 26, h < 8 ? C565(0xff, 0xd9, 0x8a) : C565(0xff, 0xe7, 0x9f));
    if (LOOK_B)
      drawCloudsB(now, biome == 3 ? C565(0xc8, 0x9c, 0x90) : C565(0xff, 0xff, 0xff),
                  lerp565(top, bot, 150, gSkyH));
    else
      drawClouds(now, C565(0xff, 0xff, 0xff));
  } else {
    if (LOOK_B)
    // Sonnenhoehe (Mitte, px ueber dem Horizont): Wald 38 (Haupt/Uhr) bzw. 24 (Ball/Kampf/Ausflug); Berg, Vulkan, Friedhof 22; sonst 6
    { int sunY = biome == BIO_BEACH ? SEA_TOP + (gBattleScene ? gSceneDY + BATTLE_SEA_ADD : 0) : gSkyH - 6;
      if (biome == BIO_FOREST) sunY = gSkyH - (gBattleScene ? 24 : 38) - gSunUp;
      else if (biome == BIO_MOUNTAIN || biome == BIO_VOLCANO || biome == BIO_GRAVEYARD) sunY = gSkyH - 22 - gSunUp;
      drawSunB(233, sunY, 32, C565(0xff, 0xe6, 0xa0), lerp565(top, bot, sunY, gSkyH), now); }  // sol poniente
    else
      gfx->fillCircle(233, gSkyH - 6, 34, C565(0xff, 0xf1, 0xc8));  // sol poniente
  }

  // mar de la playa: una franja de agua sobre la arena
  uint16_t soil = LOOK_B ? BIOME_SOIL_B[biome] : BIOME_SOIL[biome];
  if (night) soil = lerp565(soil, C565(0x16, 0x1c, 0x30), 9, 16);
  if (LOOK_B) {  // Optik B: eigene Landschaften (inkl. Strand und Friedhof)
    sg->dy = gBattleScene ? gSceneDY : 0;
    drawBiomeB(biome, now, night, h, top, bot, soil);
    sg->dy = 0;
    return;
  }
  if (biome == 1) {
    uint16_t sea = night ? C565(0x1c, 0x34, 0x52) : C565(0x4f, 0x96, 0xc4);
    gfx->fillRect(0, gSkyH - 26, 466, 26, sea);
    for (int i = 0; i < 3; i++) {
      int wy = gSkyH - 22 + i * 7;
      uint16_t fc = night ? C565(0x3a, 0x58, 0x78) : C565(0xbf, 0xe6, 0xf5);
      gfx->fillRect(60 + ((now / 60 + i * 30) % 60), wy, 26, 2, fc);
      gfx->fillRect(300 - ((now / 60 + i * 20) % 60), wy, 26, 2, fc);
    }
  }

  // suelo
  gfx->fillRect(0, gSkyH, 466, 466 - gSkyH, soil);
  uint16_t hill = lerp565(soil, night ? C565(0x0c, 0x12, 0x24) : C565(0xff, 0xff, 0xff), 3, 16);
  gfx->fillRoundRect(-60, gSkyH - 14, 586, 60, 30, hill);

  // detalles del bioma
  uint16_t dk = lerp565(soil, C565(0x10, 0x18, 0x20), night ? 11 : 7, 16);
  if (biome == 2) {  // bosque: coniferas en silueta
    for (int tx : { 60, 150, 360, 416 }) {
      gfx->fillTriangle(tx, gSkyH - 46, tx - 16, gSkyH, tx + 16, gSkyH, dk);
      gfx->fillTriangle(tx, gSkyH - 60, tx - 12, gSkyH - 28, tx + 12, gSkyH - 28, dk);
    }
  } else if (biome == 3) {  // volcan: rocas y brasas
    gfx->fillTriangle(70, gSkyH, 40, gSkyH + 30, 100, gSkyH + 30, dk);
    gfx->fillTriangle(400, gSkyH + 4, 372, gSkyH + 30, 430, gSkyH + 30, dk);
    if (!night)
      for (int e = 0; e < 4; e++)
        gfx->fillRect(120 + e * 70, gSkyH + 8 + (e % 2) * 6, 4, 4, C565(0xff, 0x9b, 0x3a));
  } else if (biome == 4) {  // montana: cumbres al fondo
    gfx->fillTriangle(140, gSkyH - 50, 60, gSkyH, 220, gSkyH, dk);
    gfx->fillTriangle(330, gSkyH - 38, 250, gSkyH, 410, gSkyH, dk);
  } else if (biome == 5 && !night) {  // nieve: copos cayendo
    for (int f = 0; f < 10; f++) {
      int fx = (f * 53 + now / 40) % 466;
      int fy = (f * 90 + now / 18) % gSkyH;
      gfx->fillRect(fx, fy, 3, 3, UI_WHITE);
    }
  } else if (biome == 0) {  // pradera: matas de hierba
    for (int gx : { 80, 175, 300, 395 })
      for (int b = -1; b <= 1; b++)
        gfx->fillRect(gx + b * 5, gSkyH + 6, 2, 8 + (b == 0 ? 4 : 0), dk);
  }
}

// primera partida: elige inicial entre Bulbasaur / Charmander / Squirtle
// Hintergrund fuer Kampf und Ballspiel in Optik B: Landschaft des Pokemon, um dy px tiefer,
// ohne Tiere (siehe gBattleScene). Kampf dy = BATTLE_DY, Ballspiel dy = BALL_DY.
void drawSideSceneBio(int dy, uint8_t biome) {
  int hh = sceneHour();
  bool night = hh < 6 || hh >= 20;
  gSceneDY = dy;
  gBattleScene = true;
  gRidgePct = 60; gMeadowRows = 3;   // ruhige Seitenansicht: niedrigere Waldhuegel, Wiese unten flach
  drawScene(biome, millis(), night);
  gRidgePct = 100; gMeadowRows = 7;  // Hauptbildschirm bleibt unveraendert
  gBattleScene = false;
}
void drawSideSceneB(int dy) { drawSideSceneBio(dy, pet.isEgg() ? 0 : biomeFor(pet.speciesId)); }
void drawBattleSceneB() { drawSideSceneB(BATTLE_DY); }
void drawBallSceneB() { drawSideSceneB(BALL_DY); }
// Ausflug-Ergebnisse (Optik B): Landschaft des angegebenen Pokemon-Bioms, 120 px tiefer
void drawTripSceneB(uint8_t biome) { drawSideSceneBio(TRIP_DY, biome); }
// Schrift auf dem Boden der Ausflug-Seiten (Optik B): bei Vulkan und Friedhof (dunkler Boden) tagsueber weiss
// statt dunkel (nachts ist die Schrift ohnehin hell). Alles andere bleibt unveraendert.
uint16_t groundTextCol(uint8_t bio, uint16_t ink) {
  if (LOOK_B && (bio == BIO_VOLCANO || bio == BIO_GRAVEYARD) && ink == UI_INK) return UI_WHITE;
  return ink;
}

void renderStarterSelect() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  const char *t = T(S_CHOOSE_STARTER);
  printUmlaut(t, CX - umlautLen(t) * 6, 68, 2, UI_INK);
  for (int i = 0; i < 3; i++) {
    int16_t d = STARTER_DEX[i];
    const DexEntry &de = DEX_TBL[d];
    int ry = STARTER_ROW_Y + i * (STARTER_ROW_H + STARTER_ROW_GAP);
    gfx->fillRoundRect(70, ry, 326, STARTER_ROW_H, 14, lerp565(de.accent, UI_WHITE, 6, 8));
    gfx->drawRoundRect(70, ry, 326, STARTER_ROW_H, 14, de.accent);
    const uint8_t *th = thumbs.get(d);     // miniatura del inicial (si la SD esta lista)
    if (th) drawThumb(th, 76, ry - 5, 3, false);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(3);
    gfx->setCursor(178, ry + 24);
    gfx->print(dexName(d));
  }
  gfx->flush();
}

void render() {
  if (pet.awaitingStarter()) {  // primera partida: elegir inicial (prioridad total)
    renderStarterSelect();
    return;
  }
  if (galleryOpen) {
    if (pokedexOverviewOpen) renderPokedexOverview();
    else if (favSlotChoiceOpen) renderFavSlot();
    else if (favConfirmShown && millis() < favConfirmUntil) renderFavConfirm();
    else { favConfirmShown = false; renderGallery(); }
    return;
  }
  if (gameOpen) {
    renderGame();
    return;
  }
  if (sackOpen) {
    renderSack();
    return;
  }
  if (battleOpen) {
    renderBattle();
    return;
  }
  if (tripOpen && !pet.tripActive) {  // el sueno forzado (22-6h) lo abandono desde pet.cpp
    tripOpen = false;
    tripEndConfirm = false;
    imuDisable();
  }
  if (pokeSearchOpen) {
    renderPokeSearch();
    return;
  }
  if (tripOpen) {
    renderTrip();
    return;
  }
  if (tripResultOpen) {
    renderTripResult();
    return;
  }
  if (tripChoiceOpen) {
    renderTripChoice();
    return;
  }
  if (tripWarningOpen) {
    renderTripWarning();
    return;
  }
  if (tripSoloResultOpen) {
    renderTripSoloResult();
    return;
  }
  if (tripSoloSadOpen) {
    renderTripSoloSad();
    return;
  }
  if (pet.tripSoloEndEpoch) {
    renderTripSolo();
    return;
  }
  if (kbOpen) {
    renderKeyboard();
    return;
  }
  if (clockOpen) {
    if (clockDeleteOpen) renderClockDelete();
    else renderClock();
    return;
  }
  if (clockSavedOpen) {
    renderClockSaved();
    return;
  }
  if (clockPeekOpen) {
    renderClockPeek();
    return;
  }
  if (weekdaySetOpen) {
    renderWeekdaySet();
    return;
  }
  if (cardOpen) {
    renderCard();
    return;
  }
  int h = sceneHour();
  gNight = pet.sleeping || h < 6 || h >= 20;
  // drawScene cubre los 466x466 completos: sin fillScreen(NEGRO) previo para
  // que un flush DMA solapado nunca capture negro a medias (anti-parpadeo)
  drawScene(pet.isEgg() ? 0 : biomeFor(pet.speciesId), millis(), gNight);

  if (pet.ceremony) {
    const DexEntry &d = DEX_TBL[pet.speciesId];
    const char *msg = (pet.ceremony == CER_FAREWELL) ? T(S_FAREWELL)
                      : (pet.ceremony == CER_RUNAWAY) ? T(S_RUNAWAY)
                                                      : T(S_GOODBYE);
    drawHeader(dexName(pet.speciesId), gNight ? UI_INK_NIGHT : UI_INK, msg);
    drawCeremony();
    gfx->flush();
    return;
  }

  if (pet.visiting || visitAnimIsExit) {
    const uint32_t ANIM_MS = 2800;
    bool wasActive = visitWasActive;
    visitWasActive = true;
    if (!wasActive && !visitAnimIsExit) {
      visitAnimStart = millis();  // frischer Einlauf-Start
      visitHostWaitUntil = millis() + 3000;  // Gastgeber wartet 3s vor dem Einlaufen des Besuchs-Pokemon
    }
    uint32_t elapsed = millis() - visitAnimStart;
    float prog = (elapsed >= ANIM_MS) ? 1.0f : (float)elapsed / ANIM_MS;
    drawHeader(dexName(pet.speciesId), gNight ? UI_INK_NIGHT : UI_INK,
               pet.visitIsHost ? (pet.visitFighting ? T(S_VISIT_FIGHTING) : T(S_VISIT_PLAYING)) : T(S_VISIT_AWAY));
    if (pet.visitIsHost) {
      // eigenes Pokemon wandert die ganze Zeit links der Mitte hin und her
      // (oder kaempft, falls Streit); das Besuchs-Pokemon lauft -- nach 3s
      // Wartezeit -- beim Beginn von links ein (WALKR), wandert/kaempft
      // waehrenddessen rechts der Mitte, und lauft am Ende (mit Herz, ausser
      // bei Streit) nach links wieder hinaus (WALKL)
      bool hostWaiting = !visitAnimIsExit && millis() < visitHostWaitUntil;
      uint32_t partnerElapsed = (millis() >= visitHostWaitUntil) ? (millis() - visitHostWaitUntil) : 0;
      float partnerProg = (partnerElapsed >= ANIM_MS) ? 1.0f : (float)partnerElapsed / ANIM_MS;
      bool partnerSteady = !visitAnimIsExit && !hostWaiting && partnerElapsed >= ANIM_MS;
      if (pmd.loaded) {
        if (pet.visitFighting && (partnerSteady || visitAnimIsExit))
          drawFightingPet(pmd, CX - 55, PET_GROUND, 0, true);
        else drawWanderingPet(pmd, CX - 90, 55, PET_GROUND, 0);
      }
      if (visitPartnerPmd.loaded && !hostWaiting) {
        if (!visitAnimIsExit && partnerElapsed < ANIM_MS) {
          int px = -70 + (int)((CX + 90 + 70) * partnerProg);
          uint8_t act = visitPartnerPmd.has(PMD_WALKR) ? PMD_WALKR : PMD_IDLE;
          drawPmdActM(visitPartnerPmd, act, px, PET_GROUND, millis(), true, false, 4, false);
          if (!pet.visitFighting) drawMap(SPR_HEART, 32, px + 40, PET_GROUND - 190, 2, false);
        } else if (visitAnimIsExit) {
          int px = (CX + 90) - (int)((CX + 90 + 70) * prog);
          uint8_t act = visitPartnerPmd.has(PMD_WALKL) ? PMD_WALKL : PMD_IDLE;
          drawPmdActM(visitPartnerPmd, act, px, PET_GROUND, millis(), true, false, 4, false);
          if (!pet.visitFighting) drawMap(SPR_HEART, 32, px + 40, PET_GROUND - 190, 2, false);
        } else if (pet.visitFighting) {
          drawFightingPet(visitPartnerPmd, CX + 55, PET_GROUND, 1200, false);
        } else {
          drawWanderingPet(visitPartnerPmd, CX + 90, 55, PET_GROUND, 2500);
        }
      }
    } else {
      // eigenes Pokemon lauft beim Beginn hinaus (nach rechts, WALKR), bleibt
      // waehrend der Besuchszeit unsichtbar, lauft am Ende von rechts
      // wieder zurueck (WALKL)
      if (pmd.loaded) {
        if (!visitAnimIsExit && elapsed < ANIM_MS) {
          int px = CX + (int)((520 - CX) * prog);
          uint8_t act = pmd.has(PMD_WALKR) ? PMD_WALKR : PMD_IDLE;
          drawPmdActM(pmd, act, px, PET_GROUND, millis(), true, false, 4, false);
        } else if (visitAnimIsExit) {
          int px = 520 - (int)((520 - CX) * prog);
          uint8_t act = pmd.has(PMD_WALKL) ? PMD_WALKL : PMD_IDLE;
          drawPmdActM(pmd, act, px, PET_GROUND, millis(), true, false, 4, false);
        }
      }
    }
    if (visitAnimIsExit && elapsed >= ANIM_MS) {
      visitAnimIsExit = false;
      visitPartnerPmd.unload();
    }
    gfx->fillRect(0, 312, 466, 154, gNight ? UI_BG_NIGHT : UI_BG_DAY);
    drawBars();
    drawButtons();  // bleiben sichtbar, aber nicht antippbar (siehe Touch-Sperre)
    gfx->flush();
    return;
  }

  if (pet.isEgg()) {
    drawHeader(T(S_EGG_HDR), inkColor(), eggMsg());
    int s = 5, x = CX - 16 * s, y = PET_CY - 16 * s;
    drawMap(SPR_EGG, SPRITE_H, x, y, s, false);
    if (pet.eggCracks() >= 5)
      for (auto &c : CRACK1) gfx->fillRect(x + c[0] * s, y + c[1] * s, s, s, INK_K);
    if (pet.eggCracks() >= 10)
      for (auto &c : CRACK2) gfx->fillRect(x + c[0] * s, y + c[1] * s, s, s, INK_K);
    if (LOOK_B) {  // B: jeder Tap laesst ein Herz ueber dem Ei aufsteigen (bis zu drei gleichzeitig)
      uint32_t nowH = millis();
      for (int k = 0; k < 3; k++)
        if (eggHeartAt[k] && nowH - eggHeartAt[k] < HEART_MS)
          drawHeartRise(x + 16 * s + (k - 1) * 14, y + 30, nowH - eggHeartAt[k], 56);
    } else if (pet.showHeart()) {
      drawMap(SPR_HEART, 32, x + 20 * s, y - 2 * s, 2, false);
    }
    if (pet.eggRarity() >= R_RARO) {
      const char *rar = (pet.eggRarity() == R_LEGENDARIO) ? T(S_EGG_LEGEND) : T(S_EGG_RARE);
      uint16_t rarColor = pet.eggRarity() == R_LEGENDARIO ? UI_BAR_WARN : 0x4C98;
      printUmlaut(rar, CX - umlautLen(rar) * 6, 316, 2, rarColor);
    }
    char reg[24];
    snprintf(reg, sizeof(reg), T(S_POKEDEX_FMT), pet.registeredCount(), (unsigned)DEX_COUNT);
    gfx->fillRect(0, 312, 466, 154, gNight ? UI_BG_NIGHT : UI_BG_DAY);
    gfx->setTextColor(inkColor());
    gfx->setTextSize(2);
    gfx->setCursor(CX - strlen(reg) * 6, 348);
    gfx->print(reg);
  } else {
    const DexEntry &d = DEX_TBL[pet.speciesId];
    char name[28];
    const char *base = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
    if (gNight) snprintf(name, sizeof(name), "%s%s", pet.shiny ? "*" : "", base);  // sin nivel: se pierde contra la luna blanca
    else snprintf(name, sizeof(name), T(S_NAME_FMT), pet.shiny ? "*" : "", base, pet.level());
    drawHeader(name, gNight ? UI_INK_NIGHT : UI_INK, statusMsg());
    drawStreakBadge();
    drawPet();
    if (LOOK_B) drawEatFoodB();
    drawBath();
    drawPoops();
    // panel inferior: base limpia para barras y botones sobre el paisaje
    gfx->fillRect(0, 312, 466, 154, gNight ? UI_BG_NIGHT : UI_BG_DAY);
    drawBars();
    drawButtons();
    drawCelebration();
    if (pet.achvCount == 0) {  // Medaillen/Meilensteine haben absoluten Vorrang: waehrenddessen keine der drei Buttons
      if (pet.canRunawayNow()) drawRunawayButton();          // CTA sombrio: escapada (abandono) -- maxima prioridad tras los logros
      else if (pet.wantEvolveButton()) drawEvolveButton();   // CTA rojo: evolucionar
      else if (pet.wantFarewellButton()) drawFarewellButton();  // CTA dorado: despedida
    }
  }

  if (pet.sleeping || pet.napping()) {
    if (LOOK_B) {
      drawSnoreB();
    } else {
      gfx->setTextColor(UI_INK_NIGHT);
      gfx->setTextSize(3);
      gfx->setCursor(320, 130);
      gfx->print("Zz");
    }
  }

  // selector de comida
  if (feedMenuUntil) {
    if (millis() > feedMenuUntil) {
      feedMenuUntil = 0;
    } else {
      gfx->fillRoundRect(101, 288, 264, 64, 14, UI_WHITE);
      gfx->drawRoundRect(101, 288, 264, 64, 14, inkColor());
      drawMap(LOOK_B ? SPR_ICONB_FOOD : SPR_ICON_FOOD, 16, 110, 296, 3, false);
      drawMap(LOOK_B ? SPR_ICONB_BERRY_B : SPR_ICON_BERRY_B, 16, 176, 296, 3, false);
      drawMap(LOOK_B ? SPR_ICONB_BERRY_G : SPR_ICON_BERRY_G, 16, 242, 296, 3, false);
      drawMap(LOOK_B ? SPR_ICONB_CANDY : SPR_ICON_CANDY, 16, 308, 296, 3, false);
    }
  }

  // dialogo "soltar?" (pulsacion larga sobre el bicho)
  if (confirmUntil) {
    if (millis() > confirmUntil) {
      confirmUntil = 0;
      releaseDenied = false;
    } else {
      gfx->fillRoundRect(94, 168, 278, 152, 16, UI_WHITE);
      gfx->drawRoundRect(94, 168, 278, 152, 16, UI_INK);
      if (releaseDenied) {
        // mismo recuadro, sin botones: el aviso de limite diario en vez de
        // la pregunta SI/NO -- se lee un momento y se cierra solo
        const char *l1 = T(S_RELEASE_LIMIT1), *l2 = T(S_RELEASE_LIMIT2), *l3 = T(S_RELEASE_LIMIT3);
        gfx->setTextColor(UI_INK);
        gfx->setTextSize(2);
        gfx->setCursor(CX - (int)strlen(l1) * 6, 215);  // seit 6.53: Textblock senkrecht mittig im Kasten
        gfx->print(l1);
        gfx->setCursor(CX - (int)strlen(l2) * 6, 237);
        gfx->print(l2);
        gfx->setCursor(CX - (int)strlen(l3) * 6, 259);
        gfx->print(l3);
      } else {
        char q[28];
        snprintf(q, sizeof(q), T(S_RELEASE_FMT), dexName(pet.speciesId));
        gfx->setTextColor(UI_INK);
        gfx->setTextSize(2);
        gfx->setCursor(CX - strlen(q) * 6, 203);  // seit 6.53: mittig zwischen Kastenrand (168) und Buttons (252)
        gfx->print(q);
        gfx->fillRoundRect(118, 252, 100, 52, 12, UI_BAR_OK);
        gfx->setTextColor(UI_WHITE);
        gfx->setCursor(118 + (100 - (int)strlen(T(S_YES)) * 12) / 2, 270);
        gfx->print(T(S_YES));
        gfx->fillRoundRect(248, 252, 100, 52, 12, UI_BAR_BAD);
        gfx->setCursor(248 + (100 - (int)strlen(T(S_NO)) * 12) / 2, 270);
        gfx->print(T(S_NO));
      }
    }
  }

  // dialogo de decision (evolucionar/mantener, despedirse/quedaros): ambos se
  // quedan abiertos hasta decidir de verdad, ningun timeout automatico
  if (choiceKind) drawChoiceDialog();
  if (sleepConfirmUntil) {
    if (millis() >= sleepConfirmUntil) sleepConfirmUntil = 0;  // 10s ohne Antwort: verschwindet wieder
    else drawSleepConfirmDialog();
  }

  gfx->flush();
}

// ---------- minijuego: toques con la pokeball ----------

#define PADDLE_Y 355     // altura de la superficie de rebote (el bicho esta con los pies en y=394)
#define PADDLE_HALF_W 42
#define CATCH_HALF_W 26  // tolerancia real de "atrapada": bastante mas estricta que antes
#define BALL_R 24
#define BALL_START_Y (PADDLE_Y - 90)  // bien por encima de la cabeza del bicho
#define BALL_APEX_Y 100   // hasta aqui sube antes de caer en picado
#define BALL_SPEED 27.0f  // rapida a proposito: apenas hay tiempo de reaccionar
// 5 puntos fijos de caida, con margen de sobra a los lados
static const int16_t GAME_SPOTS[5] = { 150, 192, 233, 275, 317 };

void startGame() {
  if (pet.isEgg() || pet.sleeping || pet.napping() || pet.blocked()) return;
  if (sceneHour() >= 20) {  // nach 20 Uhr: bald Schlafenszeit, kein Minispiel mehr
    sleepMsgId = S_TOO_LATE; sleepMsgUntil = millis() + 2500;
    sfxPlay(SFX_DENY);
    return;
  }
  gameOpen = true;
  gameOverUntil = 0;
  gameScore = 0;
  hitTime = 0;
  gamePetX = 233;
  gameLaunched = false;
  gameGoingUp = false;
  ballX = gamePetX;
  ballY = BALL_START_Y;  // posada bien alto, esperando el primer toque
  ballVX = 0;
  ballVY = 0;
}

// pala = el propio bicho: sigue al dedo en horizontal en todo momento.
// Al primer contacto (firstTouch) sobre el bicho, dispara la bola hacia arriba.
void gameDrag(int16_t x, int16_t y, bool firstTouch) {
  if (gameOverUntil) return;
  if (firstTouch && y < 72) {  // tocar la cabecera = abandonar sin premio
    gameOpen = false;
    return;
  }
  int px = x;
  if (px < PADDLE_HALF_W + 30) px = PADDLE_HALF_W + 30;
  if (px > 466 - PADDLE_HALF_W - 30) px = 466 - PADDLE_HALF_W - 30;
  gamePetX = px;
  if (!gameLaunched && firstTouch && y > 300) {  // toque "abajo" = sobre el bicho
    gameLaunched = true;
    gameGoingUp = true;
    ballX = 233;  // el primerisimo lanzamiento sigue siendo neutral, desde el centro
    gameAscentX = 233;
    ballVX = 0;
    ballVY = -BALL_SPEED;
    sfxPlay(SFX_PLAY);
  }
}

// solo velocidad y el azar de en cual de los 5 puntos cae: nada de
// gravedad, paredes ni desvios -- sube recta, y en el punto mas alto elige
// destino y se deja caer recta a toda velocidad. Practicamente sin aviso.
void stepGame() {
  if (!gameLaunched) return;
  ballY += ballVY;

  if (gameGoingUp && ballY <= BALL_APEX_Y) {
    gameGoingUp = false;
    gameTargetX = GAME_SPOTS[random(5)];
    ballVY = BALL_SPEED;  // ahora cae en picado
  } else if (!gameGoingUp) {
    // curva suave desde donde arranco esta subida (gameAscentX) hacia el
    // punto elegido: el grueso del giro pasa arriba, cerca del apice
    // (ease-out), y termina casi recta hacia el bicho -- así no cambia de
    // direccion pegada a el
    float t = (ballY - BALL_APEX_Y) / (float)(PADDLE_Y - BALL_APEX_Y);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float te = 1.0f - (1.0f - t) * (1.0f - t);
    ballX = gameAscentX + (gameTargetX - gameAscentX) * te;
    if (ballY + BALL_R >= PADDLE_Y) {
      bool caught = ballX > gamePetX - CATCH_HALF_W && ballX < gamePetX + CATCH_HALF_W;
      if (caught) {
        gameScore++;
        sfxPlay(SFX_PLAY);
        hitX = ballX;
        hitY = PADDLE_Y - BALL_R;
        hitTime = millis();
        gameGoingUp = true;
        // ballX bleibt, wo der Ball gerade gefangen wurde -- kein Sprung zur Mitte
        gameAscentX = ballX;
        ballY = PADDLE_Y - BALL_R;
        ballVY = -BALL_SPEED;
      } else {
        gameNewHi = (gameScore > pet.gameHi);
        gameSpdGain = pet.playResult(gameScore);  // da felicidad, actualiza el record
        sfxPlay(SFX_LEVEL);
        gameOverUntil = millis() + 4000;
        gameLaunched = false;
      }
    }
  }
}

// ---------- saco de entrenamiento (entrena la fuerza) ----------

void startSack() {
  if (pet.isEgg() || pet.napping() || pet.blocked()) return;
  if (choiceKind) {
    sleepMsgId = S_NOT_YET; sleepMsgUntil = millis() + 2500;
    sfxPlay(SFX_DENY);
    return;
  }
  if (pet.sleeping) {
    sleepMsgId = S_IS_SLEEPING; sleepMsgUntil = millis() + 2500;
    sfxPlay(SFX_DENY);
    return;
  }
  if (sceneHour() >= 20) {  // nach 20 Uhr: bald Schlafenszeit, kein Training mehr
    sleepMsgId = S_TOO_LATE; sleepMsgUntil = millis() + 2500;
    sfxPlay(SFX_DENY);
    return;
  }
  sackOpen = true;
  sackUntil = millis() + 10000;
  sackOverUntil = 0;
  sackHits = 0;
  sackShake = 0;
  sackStFrames = sackStRenderSum = sackStRenderMax = sackStGapMax = sackStLastStart = 0;
  sackStPolls = sackStPollGapMax = sackStLastPoll = 0;  // Messung zuruecksetzen
  sackStIrq0 = gTouchIrqCount;
}

void sackTap() {
  if (millis() >= sackUntil) return;  // ya termino el tiempo
  if (millis() - sackLastHit < SACK_MIN_HIT_GAP) return;  // Prellen/Doppelzaehlung ausschliessen (A und B)
  sackLastHit = millis();
  sackHits++;
  sackShake = 16;  // sacude el saco
  if (sackHits % 2 == 1) sfxPlay(SFX_TRAIN);  // jeder zweite Schlag, beginnend mit dem ersten
}

void drawGameScene();  // prototipo (definida mas abajo)

void renderSack() {
  uint32_t now = millis();
  if (LOOK_B) {  // Optik B: Landschaft des Pokemon (120 px tiefer), Sonne/Mond tiefer und rechts
    gSunDX = TRIP_SUN_DX; gSunDY = TRIP_SUN_DY;
    drawTripSceneB(pet.isEgg() ? 0 : biomeFor(pet.speciesId));
    gSunDX = gSunDY = 0;
  } else drawGameScene();  // fondo del habitat
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  int up = LOOK_B ? SACK_RESULT_UP : 0;  // Optik B: Ergebnistext hoeher, mittig im Himmel

  // pantalla de resultado
  if (sackOverUntil) {
    if (now > sackOverUntil) { sackOpen = false; return; }
    char b[20];
    snprintf(b, sizeof(b), T(S_HITS_FMT), sackHits);
    gfx->setTextColor(ink);
    gfx->setTextSize(4);
    gfx->setCursor(CX - strlen(b) * 12, 150 - up);
    gfx->print(b);
    char g[18];
    if (pet.trAtk >= 100) strncpy(g, T(S_ATK_MAX), sizeof(g) - 1);
    else snprintf(g, sizeof(g), T(S_STR_GAIN_FMT), sackGain);
    g[sizeof(g) - 1] = 0;
    gfx->setTextColor(LOOK_B ? ink : RGB565_BLACK);  // Optik B: auch nachts lesbar
    gfx->setTextSize(3);
    gfx->setCursor(CX - strlen(g) * 9, 210 - up);
    gfx->print(g);
    // el corazon (>=25 golpes) solo dura 1.5s, mucho menos que esta pantalla
    // de resultado (3.5s): sin pintarlo aqui, nunca llegaba a verse
    if (pet.showHeart()) drawMap(SPR_HEART, 32, CX - 32, 260 - up, 2, false);
    gfx->flush();
    return;
  }

  // se acabaron los 10 s: aplicar entrenamiento (ya sin record: eso ahora
  // vive en el minijuego de la pelota, que no tiene limite de tiempo)
  if (now >= sackUntil) {
    if (sackStFrames)  // Messung: Zeit pro Bild (inkl. Flush) und groesste Luecke zwischen zwei Bildern
      Serial.printf("[TRAIN] look=%c bio=%u hits=%u irq=%lu frames=%lu render avg=%lums max=%lums gapmax=%lums polls=%lu pollgap=%lums\n",
                    LOOK_B ? 'B' : 'A', pet.isEgg() ? 0 : biomeFor(pet.speciesId), (unsigned)sackHits,
                    (unsigned long)(gTouchIrqCount - sackStIrq0), (unsigned long)sackStFrames,
                    (unsigned long)(sackStRenderSum / sackStFrames), (unsigned long)sackStRenderMax,
                    (unsigned long)sackStGapMax, (unsigned long)sackStPolls, (unsigned long)sackStPollGapMax);
    sackGain = pet.trainStrength(sackHits);
    sfxPlay(sackHits >= 30 ? SFX_BATTLE_WIN : SFX_BATTLE_LOSE);
    sackOverUntil = now + 3500;
    gfx->flush();
    return;
  }

  // aporreo activo
  sackShake *= 0.84f;
  int off = (int)(sackShake * sinf(now * 0.05f));
  int sx = CX + off, top = 86, sy = 150;
  gfx->fillRect(CX - 3, 56, 6, top - 56, ink);          // gancho/cuerda
  gfx->fillRect(sx - 4, top - 30, 8, 34, ink);          // cadena
  gfx->fillRoundRect(sx - 42, top, 84, 150, 26, C565(0xb5, 0x3a, 0x3a));  // saco
  gfx->fillRoundRect(sx - 42, top, 84, 22, 18, C565(0x7e, 0x28, 0x28));   // tapa
  gfx->drawRoundRect(sx - 42, top, 84, 150, 26, ink);
  gfx->fillRect(sx - 42, top + 70, 84, 4, C565(0x7e, 0x28, 0x28));        // costura

  // contador de golpes
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", sackHits);
  gfx->setTextColor(ink);
  gfx->setTextSize(6);
  gfx->setCursor(CX - strlen(buf) * 18, 268);
  gfx->print(buf);

  gfx->setTextSize(2);
  if (LOOK_B) {  // Vulkan/Friedhof: dunkler Boden -> Hinweis weiss
    uint8_t hb = pet.isEgg() ? 0 : biomeFor(pet.speciesId);
    if (hb == BIO_VOLCANO || hb == BIO_GRAVEYARD) gfx->setTextColor(UI_WHITE);
  }
  gfx->setCursor(CX - strlen(T(S_HIT_FAST)) * 6, LOOK_B ? SACK_HINT_Y_B : 322);  // Optik B: Hinweis unter dem Zeitbalken
  gfx->print(T(S_HIT_FAST));

  // barra de tiempo
  uint32_t left = sackUntil - now;
  int bw = 280, fw = (int)((uint32_t)bw * left / 10000);
  int by = LOOK_B ? SACK_BAR_Y_B : 350;  // Optik B: Zeitbalken auf dem Boden, nicht auf der Horizontlinie
  if (LOOK_B) {  // Optik B: Rand aussen weiss, innen schwarz -> auf jedem Untergrund erkennbar
    gfx->fillRoundRect(CX - bw / 2 - 3, by - 3, bw + 6, 22, 8, UI_WHITE);
    gfx->fillRoundRect(CX - bw / 2 - 1, by - 1, bw + 2, 18, 6, RGB565_BLACK);
  }
  gfx->fillRoundRect(CX - bw / 2, by, bw, 16, 5, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(CX - bw / 2, by, fw, 16, 5, UI_BAR_OK);

  handleTouch();  // zusaetzliche Touch-Abfrage vor dem langen, blockierenden Flush (A und B)
  gfx->flush();
  uint32_t dt = millis() - now;   // Messung (nur Serial-Ausgabe, keine Wirkung aufs Spiel)
  if (dt > sackStRenderMax) sackStRenderMax = dt;
  sackStRenderSum += dt;
  if (sackStLastStart) { uint32_t gap = now - sackStLastStart; if (gap > sackStGapMax) sackStGapMax = gap; }
  sackStLastStart = now;
  sackStFrames++;
}

// fondo del minijuego: hatibat del bicho (cielo por hora + suelo del bioma)
// nube sencilla estilo pixel-art: tres bultos + base para que no quede hueca
void drawCloud(int x, int y, uint16_t color) {
  gfx->fillCircle(x, y, 14, color);
  gfx->fillCircle(x + 16, y - 6, 18, color);
  gfx->fillCircle(x + 34, y, 14, color);
  gfx->fillRect(x - 2, y, 46, 12, color);
}

void drawGameScene() {
  int hh = sceneHour();
  bool night = hh < 6 || hh >= 20;
  bool dawn = hh >= 6 && hh < 8;
  bool day = hh >= 8 && hh < 18;
  uint16_t top, bot;
  if (night)       { top = C565(0x0c, 0x12, 0x24); bot = C565(0x1e, 0x26, 0x46); }
  else if (dawn)   { top = C565(0xd1, 0x6a, 0x86); bot = C565(0xf3, 0xb8, 0x7c); }
  else if (day)    { top = C565(0x8f, 0xc8, 0xea); bot = C565(0xdc, 0xee, 0xe6); }
  else             { top = C565(0xc7, 0x5a, 0x4a); bot = C565(0xf0, 0xae, 0x64); }
  int hor = 376;
  for (int y = 0; y < hor; y += 8)
    gfx->fillRect(0, y, 466, 8, lerp565(top, bot, y, hor));

  uint32_t now = millis();
  if (night && LOOK_B) {
    drawStarsB(now, top, bot, HORIZON - 10);
    drawMoonB(360 + gSunDX, 84 + gSunDY, lerp565(top, bot, 84 + gSunDY, hor));
  } else if (night) {
    for (auto &st : STARS) gfx->fillRect(st[0], st[1], 4, 4, UI_WHITE);
    // luna en cuarto creciente: circulo palido con una "mordida" del color
    // del cielo
    uint16_t moonCol = C565(0xe8, 0xe6, 0xd8);
    gfx->fillCircle(360, 84, 22, moonCol);
    gfx->fillCircle(370, 76, 20, top);
  } else if (day) {
    // sol alto con halo suave
    uint16_t sunCore = C565(0xff, 0xe1, 0x66);
    if (LOOK_B) {
      drawSunB(350 + gSunDX, 86 + gSunDY, 24, sunCore, lerp565(top, bot, 86 + gSunDY, hor), now);
    } else {
      gfx->fillCircle(350, 86, 34, lerp565(sunCore, top, 1, 2));
      gfx->fillCircle(350, 86, 24, sunCore);
    }
    // nubes a la deriva en tres alturas, cada una a su ritmo
    for (int i = 0; i < 3; i++) {
      int cx = (int)((now / 40 + (uint32_t)i * 210) % 620) - 90;
      int cy = 110 + i * 40;
      if (LOOK_B) drawCloudB(cx, cy, i == 1 ? 85 : 105, UI_WHITE, lerp565(top, bot, 150, hor));
      else drawCloud(cx, cy, UI_WHITE);
    }
  } else {  // amanecer o atardecer: sol bajo, cerca del horizonte
    int sx = dawn ? 90 : 376;
    uint16_t sunCore = C565(0xff, 0xd3, 0x8a);
    if (LOOK_B) {
      drawSunB(sx, 320, 28, sunCore, lerp565(top, bot, 320, hor), now);
    } else {
      gfx->fillCircle(sx, 320, 40, lerp565(sunCore, bot, 1, 2));
      gfx->fillCircle(sx, 320, 28, sunCore);
    }
    if (LOOK_B) drawCloudB(dawn ? 250 : 130, 190, 100, lerp565(UI_WHITE, top, 1, 3), lerp565(top, bot, 190, hor));
    else drawCloud(dawn ? 250 : 130, 190, lerp565(UI_WHITE, top, 1, 3));
  }

  uint8_t bio = pet.isEgg() ? 0 : biomeFor(pet.speciesId);
  uint16_t soil = LOOK_B ? BIOME_SOIL_B[bio < BIO_COUNT_B ? bio : 0] : BIOME_SOIL[bio < 6 ? bio : 0];
  if (night) soil = lerp565(soil, C565(0x16, 0x1c, 0x30), 9, 16);
  gfx->fillRect(0, hor, 466, 466 - hor, soil);
}

void renderGame() {
  // sin fillScreen(NEGRO): drawGameScene cubre los 466x466 completos. Si el
  // DMA del flush anterior aun lee el buffer, vera contenido valido (no negro
  // a medio pintar), que era el parpadeo a 25 fps.
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  // Ergebnisbildschirm: in Optik B steht der Text hoeher (mittig zwischen oberem Rand und Horizont)
  int up = LOOK_B ? BALL_RESULT_UP : 0;

  if (gameOverUntil) {
    if (LOOK_B) drawBallSceneB();  // Optik B: Landschaft, 120 px tiefer
    else drawGameScene();
    if (millis() > gameOverUntil) {
      gameOpen = false;
      return;
    }
    char buf[22];
    snprintf(buf, sizeof(buf), T(S_SCORE_FMT), gameScore);
    gfx->setTextColor(ink);
    gfx->setTextSize(4);
    gfx->setCursor(CX - strlen(buf) * 12, 160 - up);
    gfx->print(buf);
    gfx->setTextSize(2);
    const char *msg = gameScore >= 10 ? T(S_GREAT_JOY) : T(S_PLUS_JOY);
    printUmlaut(msg, CX - umlautLen(msg) * 6, 214 - up, 2, ink);
    char spd[14];
    if (pet.trSpe >= 100) strncpy(spd, T(S_SPD_MAX), sizeof(spd) - 1);
    else snprintf(spd, sizeof(spd), T(S_SPD_GAIN_FMT), gameSpdGain);
    spd[sizeof(spd) - 1] = 0;
    gfx->setCursor(CX - strlen(spd) * 6, 238 - up);
    gfx->print(spd);
    if (gameNewHi && gameScore > 0) {
      gfx->setTextColor(LOOK_B ? ink : RGB565_BLACK);  // Optik B: auch nachts lesbar
      gfx->setCursor(CX - strlen(T(S_NEW_RECORD)) * 6, 270 - up);
      gfx->print(T(S_NEW_RECORD));
    } else {
      char r[18];
      snprintf(r, sizeof(r), T(S_RECORD_FMT), pet.gameHi);
      gfx->setTextColor(ink);
      gfx->setCursor(CX - strlen(r) * 6, 270 - up);
      gfx->print(r);
    }
    // el corazon (score alto) solo dura 1.5s, mucho menos que esta pantalla
    // de resultado (4s): sin pintarlo aqui, nunca llegaba a verse (mismo
    // problema que ya se corrigio en el resultado del saco de entrenamiento)
    if (pet.showHeart()) drawMap(SPR_HEART, 32, CX - 32, 300 - up, 2, false);
    gfx->flush();
    return;
  }

  if (LOOK_B) drawBallSceneB();  // Optik B: Landschaft, 120 px tiefer
  else drawGameScene();
  stepGame();

  // marcador (un solo intento: sin indicador de vidas, sin record)
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", gameScore);
  gfx->setTextColor(ink);
  gfx->setTextSize(4);
  gfx->setCursor(CX - strlen(buf) * 12, 30);
  gfx->print(buf);

  if (pmd.loaded) {
    static float lastPetX = 233;
    uint8_t act = (gamePetX > lastPetX + 1) ? PMD_WALKR : (gamePetX < lastPetX - 1) ? PMD_WALKL : PMD_IDLE;
    lastPetX = gamePetX;
    if (!pmd.has(act)) act = PMD_IDLE;
    drawPmdAct(act, (int)gamePetX, 394, millis(), true, false, 3, false);
  } else if (mon.loaded) {
    int s = (mon.h * 2 > 130) ? 1 : 2;
    int w = mon.w * s, h = mon.h * s;
    uint16_t fm = mon.frameMs ? mon.frameMs : 100;
    uint16_t fi = (millis() / fm) % mon.frames;
    const uint8_t *fr = mon.data + (uint32_t)fi * mon.w * mon.h;
    int px = (int)gamePetX - w / 2, py = 394 - h;
    for (int r = 0; r < mon.h; r++)
      for (int c = 0; c < mon.w; c++) {
        uint8_t idx = fr[r * mon.w + c];
        if (idx == 0xFF) continue;
        gfx->fillRect(px + c * s, py + r * s, s, s, mon.pal[idx]);
      }
  }

  // anillo de impacto que se expande y desvanece (feedback suave del golpe)
  uint32_t ht = millis() - hitTime;
  if (hitTime && ht < 260) {
    int rad = 22 + (int)(ht / 6);
    gfx->drawCircle((int)hitX, (int)hitY, rad, C565(0xff, 0xe7, 0x9f));
    gfx->drawCircle((int)hitX, (int)hitY, rad - 2, C565(0xff, 0xd9, 0x8a));
  }

  // la pokeball
  drawMap(SPR_ICON_PLAY, 16, (int)ballX - 24, (int)ballY - 24, 3, false);

  gfx->flush();
}

// ---------- combate (battle): 5 rondas automaticas contra un rival ----------

#define BATTLE_HP(base, lvl) ((uint16_t)(base) + (uint16_t)(lvl) * 2)

// dano = ataque/defensa * multiplicador de tipo (en octavos); minimo 3, para
// que un golpe nunca se sienta como "no paso nada"
uint16_t battleDamage(uint16_t atk, uint16_t def, uint8_t atkType, uint8_t defType) {
  uint32_t mult8 = TYPE_CHART[atkType][defType];
  uint32_t base = (uint32_t)atk * 22 / (def ? def : 1);
  uint32_t dmg = base * mult8 / 8;
  if (dmg < 3) dmg = 3;
  if (dmg > 9999) dmg = 9999;
  return (uint16_t)dmg;
}

// el sobrepeso (>=67, igual que en la ficha) resta 40% a Defensa y Velocidad
// solo dentro del combate; en la ficha de kampfwerte se sigue mostrando el
// valor real sin este malus
uint16_t battleEffDef() {
  uint16_t d = pet.defStat();
  return (pet.weight >= OVERWEIGHT_FROM) ? (uint16_t)(d * 3 / 5) : d;
}
uint16_t battleEffSpe() {
  uint16_t s = pet.speStat();
  return (pet.weight >= OVERWEIGHT_FROM) ? (uint16_t)(s * 3 / 5) : s;
}

// Pokemon-Suche per Bluetooth-Pille (Statusseite 2). Reagiert in den
// Sperrfaellen wie Training und Kampf (Meldung + Ablehnungston); die Suche
// selbst laeuft nur zwischen 6 und 20 Uhr (ein schlafendes Pokemon blockt vorher).
void startPokeSearch() {
  if (pet.isEgg() || pet.napping() || pet.blocked()) return;
  if (choiceKind) {
    sleepMsgId = S_NOT_YET; sleepMsgUntil = millis() + 2500;
    sfxPlay(SFX_DENY);
    return;
  }
  if (pet.sleeping) {
    sleepMsgId = S_IS_SLEEPING; sleepMsgUntil = millis() + 2500;
    sfxPlay(SFX_DENY);
    return;
  }
  int h = sceneHour();
  if (h >= 20) {  // ab 20 Uhr: Schlafenszeit (wie Training/Kampf)
    sleepMsgId = S_TOO_LATE; sleepMsgUntil = millis() + 2500;
    sfxPlay(SFX_DENY);
    return;
  }
  if (h < 6) {  // nachts vor 6 Uhr: noch zu frueh
    sleepMsgId = S_NOT_YET; sleepMsgUntil = millis() + 2500;
    sfxPlay(SFX_DENY);
    return;
  }
  pokeSearchOpen = true;
  pokeSearchPhase = PS_SEARCHING;
  pokeSearchUntil = millis() + 10000;
  meetBleStart();
  sfxPlay(SFX_HEART);  // Bestaetigungston
}

void startBattle() {
  if (pet.isEgg() || pet.napping() || pet.blocked()) return;
  if (choiceKind) {
    sleepMsgId = S_NOT_YET; sleepMsgUntil = millis() + 2500;
    sfxPlay(SFX_DENY);
    return;
  }
  if (pet.sleeping) {
    sleepMsgId = S_IS_SLEEPING; sleepMsgUntil = millis() + 2500;
    sfxPlay(SFX_DENY);
    return;
  }
  if (sceneHour() >= 20) {  // nach 20 Uhr: bald Schlafenszeit, kein Kampf mehr
    sleepMsgId = S_TOO_LATE; sleepMsgUntil = millis() + 2500;
    sfxPlay(SFX_DENY);
    return;
  }
  int16_t candidates[DEX_COUNT];
  int n = 0;
  for (int16_t sp = 1; sp <= DEX_COUNT; sp++) {
    if (sp == pet.speciesId) continue;       // nunca contra el propio bicho
    if (!pet.isRegistered(sp)) continue;      // solo especies ya aufgedeckt
    candidates[n++] = sp;
  }
  if (n == 0) {  // sin nadie descubierto todavia (recien empezado el juego)
    sleepMsgId = S_BATTLE_NO_OPP1; sleepMsgUntil = millis() + 3500;
    sfxPlay(SFX_DENY);
    return;
  }
  battleOppDex = candidates[random(n)];
  pet.registerBattle();
  if (pet.dexBattleTotal[battleOppDex] < 65535) pet.dexBattleTotal[battleOppDex]++;
  if (pet.speciesId >= 1 && pet.speciesId <= DEX_COUNT && pet.dexBattleTotal[pet.speciesId] < 65535)
    pet.dexBattleTotal[pet.speciesId]++;
  // nivel rival: entre -3 y +3 del nuestro, pero con mas peso hacia arriba
  // a proposito -- un rival de nivel mas alto puede darse mas a menudo
  static const int8_t lvlDeltas[10] = { -3, -2, -1, 0, 1, 1, 2, 2, 3, 3 };  // 60% positivo
  int lv = (int)pet.level() + lvlDeltas[random(10)];
  if (lv < 1) lv = 1;
  battleOppLevel = (uint8_t)lv;
  const DexEntry &od = DEX_TBL[battleOppDex];
  // stats temporales del rival: solo para este combate, no se guardan.
  // Gen al 100% y sin entrenamiento (un rival salvaje generico)
  battleOppAtk = (uint16_t)od.bAtk + lv;
  battleOppDef = (uint16_t)od.bDef + lv;
  battleOppSpe = (uint16_t)od.bSpe + lv;
  battleOppMaxHp = BATTLE_HP(od.bHp, lv);
  battleOppHp = battleOppMaxHp;
  battlePlayerMaxHp = BATTLE_HP(DEX_TBL[pet.speciesId].bHp, pet.level());
  battlePlayerHp = battlePlayerMaxHp;
  battleRound = 0;
  battleAttacksInRound = 0;
  battleOver = false;
  battleWon = false;
  battleBlockedHits = 0;
  battlePhase = 0;
  // el mas rapido ataca primero
  battleTurn = (battleOppSpe > battleEffSpe()) ? 1 : 0;
  battlePhaseUntil = millis() + 700;
  battlePmd.load(battleOppDex, false);
  cardOpen = false;
  battleOpen = true;
}

void stepBattle() {
  if (millis() < battlePhaseUntil) return;
  uint32_t now = millis();

  if (battlePhase == 0) {  // pausa antes de lanzar
    battlePhase = 1;
    battlePhaseUntil = now + 550;  // tiempo de vuelo del proyectil
  } else if (battlePhase == 1) {  // el proyectil llega: aplica el golpe (o falla)
    bool weAttack = (battleTurn == 0);
    // con sobrepeso el propio golpe falla mucho mas facil: 50% con
    // sobrepeso normal (67-99), 80% si el peso esta exactamente en el
    // tope (100) -- sustituye al 10% base, solo afecta al ataque PROPIO
    int missChance = 10;
    if (weAttack) {
      if (pet.weight == 100) missChance = 80;
      else if (pet.weight >= OVERWEIGHT_FROM) missChance = 50;
    }
    bool missed = random(100) < missChance;
    if (missed) {
      battleLastHit = 0;
      if (!weAttack) battleBlockedHits++;  // el rival fallo contra nosotros: lo abrimos
      sfxPlay(SFX_MISS);
    } else {
      uint8_t atkType = DEX_TBL[weAttack ? pet.speciesId : battleOppDex].type;
      uint8_t defType = DEX_TBL[weAttack ? battleOppDex : pet.speciesId].type;
      uint16_t atk = weAttack ? pet.atkStat() : battleOppAtk;
      uint16_t def = weAttack ? battleOppDef : battleEffDef();
      uint16_t dmg = battleDamage(atk, def, atkType, defType);
      // con sobrepeso, 10% de posibilidades de que el golpe del rival sea
      // inesperadamente fuerte (+50% de dano) -- para meterle tension
      bool heavyHit = !weAttack && pet.weight >= OVERWEIGHT_FROM && random(100) < 50;
      if (heavyHit) dmg = (uint16_t)(dmg * 3 / 2);
      uint16_t targetMax = weAttack ? battleOppMaxHp : battlePlayerMaxHp;
      if (weAttack) battleOppHp = (dmg >= battleOppHp) ? 0 : battleOppHp - dmg;
      else battlePlayerHp = (dmg >= battlePlayerHp) ? 0 : battlePlayerHp - dmg;
      // fuerte = al menos 1/4 de la vida maxima del objetivo de un solo golpe
      bool strong = targetMax && ((uint32_t)dmg * 4 >= targetMax);
      battleLastHit = strong ? 2 : 1;
      sfxPlay(strong ? SFX_HIT_STRONG : SFX_HIT_WEAK);
    }
    battlePhase = 2;
    battlePhaseUntil = now + 900;  // tiempo para ver la reaccion triste/feliz
  } else if (battlePhase == 2) {  // fin del turno: victoria, siguiente turno o ronda
    if (battlePlayerHp == 0 || battleOppHp == 0) {
      battleOver = true;
      battleWon = (battleOppHp == 0);
    } else {
      battleAttacksInRound++;
      if (battleAttacksInRound >= 2) {  // los dos ya atacaron: cierra la ronda
        battleAttacksInRound = 0;
        battleRound++;
        if (battleRound >= 5) {
          battleOver = true;
          // seit 6.64: ohne K.O. gewinnt, wer dem anderen mehr Schaden zugefuegt hat (bei Gleichstand wir)
          battleWon = ((uint32_t)(battleOppMaxHp - battleOppHp) >= (uint32_t)(battlePlayerMaxHp - battlePlayerHp));
        }
      }
    }
    if (battleOver) {
      battleDefGain = pet.battleResult(battleWon, battleOppDex, battleBlockedHits);
      sfxPlay(battleWon ? SFX_BATTLE_WIN : SFX_BATTLE_LOSE);
      battlePhaseUntil = now + 3000;
      battlePhase = 3;
    } else {
      battleTurn = 1 - battleTurn;
      battlePhase = 0;
      battlePhaseUntil = now + 500;
    }
  } else {  // fase 3: resultado mostrado, volver sola al hueco bicho
    battleOpen = false;
    battlePmd.unload();
  }
}

// Seit 6.64: gemeinsame Skala fuer beide Balken. Der Balken mit den groesseren Max-HP ist voll breit (w), der
// andere entsprechend schmaler (mindestens BATTLE_BAR_MIN px, damit er auch bei extremen Unterschieden sichtbar
// bleibt); die Fuellung ist der Anteil der eigenen Max-HP, also ist das leere Stueck ~ der erlittene Schaden.
// Die Farbe bleibt relativ zur eigenen Max-HP (gruen ab 50 %, orange ab 25 %, sonst rot).
#define BATTLE_BAR_MIN 30
void drawBattleHpBar(int x, int y, int w, uint16_t hp, uint16_t maxHp) {
  uint32_t bigMax = battlePlayerMaxHp > battleOppMaxHp ? battlePlayerMaxHp : battleOppMaxHp;
  int tw = bigMax ? (int)((uint32_t)w * maxHp / bigMax) : w;
  if (tw < BATTLE_BAR_MIN) tw = BATTLE_BAR_MIN;
  if (tw > w) tw = w;
  int tx = x + (w - tw) / 2;                                   // mittig unter dem Pokemon
  gfx->fillRoundRect(tx, y, tw, 14, 4, UI_TRACK);
  int fw = maxHp ? (int)((uint32_t)(tw - 4) * hp / maxHp) : 0;
  uint16_t col = (hp * 2 >= maxHp) ? UI_BAR_OK : (hp * 4 >= maxHp) ? UI_BAR_WARN : UI_BAR_BAD;
  if (fw > 0) gfx->fillRoundRect(tx + 2, y + 2, fw, 10, 3, col);
}

// Angriffsgeschosse je Typ (seit 6.65). cx/cy = Mitte, d = +1 fliegt nach rechts, -1 nach links.
// Alles aus einfachen Formen gezeichnet; 'now' laesst Flammen flackern und Blaetter/Steine/Sterne drehen.
static void shotStar(int cx, int cy, int rO, int rI, float an, uint16_t lo, uint16_t hi, int pts) {
  for (int i = 0; i < pts * 2; i++) {
    float a0 = an + i * 3.14159f / pts, a1 = an + (i + 1) * 3.14159f / pts;
    int r0 = (i & 1) ? rI : rO, r1 = (i & 1) ? rO : rI;
    gfx->fillTriangle(cx, cy, cx + (int)(cosf(a0) * r0), cy + (int)(sinf(a0) * r0), cx + (int)(cosf(a1) * r1), cy + (int)(sinf(a1) * r1), (i & 2) ? hi : lo);
  }
}
void drawBattleShot(uint8_t type, int cx, int cy, int d, uint32_t now) {
  int fl = (int)((now / 60) % 3);
  float an = now / 110.0f;
  auto X = [&](int off) { return cx + d * off; };                 // off > 0 vorne, off < 0 dahinter
  const uint16_t WHT = C565(0xff, 0xff, 0xff);
  switch (type) {
    case TYPE_FIRE: {                                              // Feuerball mit flackerndem Schweif
      uint16_t red = C565(0xe8, 0x38, 0x10), org = C565(0xff, 0x8a, 0x20), yel = C565(0xff, 0xd8, 0x50), wht = C565(0xff, 0xf4, 0xc8);
      gfx->fillTriangle(X(-4), cy - 14, X(-4), cy + 14, X(-54 - fl * 4), cy + (fl - 1) * 3, red);
      gfx->fillTriangle(X(-4), cy - 10, X(-4), cy + 10, X(-42 - fl * 3), cy - (fl - 1) * 3, org);
      gfx->fillTriangle(X(-4), cy - 6, X(-4), cy + 6, X(-30 - fl * 2), cy, yel);
      gfx->fillCircle(cx, cy, 15, red); gfx->fillCircle(cx, cy, 11, org); gfx->fillCircle(X(2), cy, 7, yel); gfx->fillCircle(X(3), cy, 3, wht);
      for (int k = 0; k < 4; k++) gfx->fillRect(X(-26 - k * 14), cy - 14 + (int)((now / 40 + k * 17) % 28), 3, 3, (k & 1) ? yel : org);
    } break;
    case TYPE_WATER: {                                             // Wasserblase mit Tropfen
      uint16_t d1 = C565(0x2a, 0x70, 0xd8), d2 = C565(0x4a, 0x9c, 0xf0), d3 = C565(0xa8, 0xdc, 0xff);
      gfx->fillTriangle(X(-8), cy - 10, X(-8), cy + 10, X(-36), cy, d1);
      for (int k = 0; k < 3; k++) gfx->fillCircle(X(-26 - k * 14), cy + ((k & 1) ? -7 : 7), 6 - k, k ? d2 : d1);
      gfx->fillCircle(cx, cy, 14, d1); gfx->fillCircle(cx, cy, 11, d2);
      gfx->fillCircle(cx - 4, cy - 5, 4, d3); gfx->fillCircle(cx + 4, cy + 5, 2, d3);
    } break;
    case TYPE_GRASS: {                                             // Rasierblatt: drei drehende Blaetter
      uint16_t g1 = C565(0x2e, 0x8c, 0x34), g2 = C565(0x68, 0xd0, 0x48), g3 = C565(0xb8, 0xf0, 0x90);
      for (int k = 0; k < 3; k++) {
        int lx = X(-k * 20), ly = cy + (int)(sinf(an * 0.7f + k * 2.0f) * 12);
        float a = an * 1.6f + k * 2.1f, ca = cosf(a), sa = sinf(a);
        int tx1 = lx + (int)(ca * 15), ty1 = ly + (int)(sa * 15), tx2 = lx - (int)(ca * 15), ty2 = ly - (int)(sa * 15);
        int sx1 = lx + (int)(-sa * 7), sy1 = ly + (int)(ca * 7), sx2 = lx - (int)(-sa * 7), sy2 = ly - (int)(ca * 7);
        gfx->fillTriangle(tx1, ty1, tx2, ty2, sx1, sy1, g1);
        gfx->fillTriangle(tx1, ty1, tx2, ty2, sx2, sy2, g2);
        gfx->drawLine(tx1, ty1, tx2, ty2, g3);
      }
    } break;
    case TYPE_ELECTRIC: {                                          // Blitzschlag: Zickzack hinter einer Funkenkugel
      uint16_t y1 = C565(0xff, 0xe8, 0x40), y2 = C565(0xff, 0xfb, 0xc8), y3 = C565(0xf0, 0xb0, 0x20);
      int px0 = X(8), py0 = cy;
      for (int i = 0; i < 5; i++) {
        int nx = X(8 - (i + 1) * 15), ny = cy + (((i + (int)(now / 55)) & 1) ? 13 : -13);
        for (int o = -1; o <= 1; o++) gfx->drawLine(px0, py0 + o, nx, ny + o, y1);
        gfx->drawLine(px0, py0, nx, ny, y2);
        px0 = nx; py0 = ny;
      }
      gfx->fillCircle(cx, cy, 11, y3); gfx->fillCircle(cx, cy, 8, y1); gfx->fillCircle(cx, cy, 4, y2);
      for (int k = 0; k < 4; k++) { float a = an * 2 + k * 1.57f; gfx->drawLine(cx + (int)(cosf(a) * 12), cy + (int)(sinf(a) * 12), cx + (int)(cosf(a) * 20), cy + (int)(sinf(a) * 20), y2); }
    } break;
    case TYPE_ICE: {                                               // Eispfeile: drei Pfeile
      uint16_t i1 = C565(0x4c, 0x9c, 0xd8), i2 = C565(0xc8, 0xf0, 0xff), i3 = C565(0xf4, 0xfe, 0xff), i0 = C565(0x2c, 0x68, 0xb0);   // i0: dunkler Rand, auch vor Schnee gut sichtbar
      static const int8_t AO[3][2] = { { 8, -18 }, { 0, 0 }, { 8, 18 } };
      for (int k = 0; k < 3; k++) {
        int ax = X(AO[k][0]), ay = cy + AO[k][1];
        gfx->fillTriangle(X(AO[k][0] + 24), ay, X(AO[k][0] - 1), ay - 12, X(AO[k][0] - 1), ay + 12, i0);   // Rand der Spitze
        for (int o = -2; o <= 2; o++) gfx->drawLine(X(AO[k][0] - 26), ay + o, ax, ay + o, o == -2 || o == 2 ? i0 : i1);   // Schaft
        gfx->fillTriangle(X(AO[k][0] + 20), ay, X(AO[k][0] + 2), ay - 9, X(AO[k][0] + 2), ay + 9, i2); // Spitze
        gfx->fillTriangle(X(AO[k][0] + 20), ay, X(AO[k][0] + 2), ay - 9, X(AO[k][0] + 6), ay, i3);      // Glanz
        gfx->fillTriangle(X(AO[k][0] - 26), ay, X(AO[k][0] - 36), ay - 7, X(AO[k][0] - 30), ay, i2);    // Befiederung
        gfx->fillTriangle(X(AO[k][0] - 26), ay, X(AO[k][0] - 36), ay + 7, X(AO[k][0] - 30), ay, i2);
      }
      for (int k = 0; k < 2; k++) { int sx = X(-44 - k * 16), sy = cy - 6 + k * 14 + (int)((now / 80 + k) % 3); gfx->drawLine(sx - 3, sy, sx + 3, sy, i3); gfx->drawLine(sx, sy - 3, sx, sy + 3, i3); }
    } break;
    case TYPE_FIGHTING: {                                          // Faust mit Tempolinien
      uint16_t f1 = C565(0xa8, 0x4c, 0x18), f2 = C565(0xe0, 0x80, 0x30), f3 = C565(0xf8, 0xb8, 0x68);
      for (int k = 0; k < 3; k++) gfx->fillRect(d > 0 ? X(-52) : X(-18), cy + (k - 1) * 10 - 1, 34, 3, k == 1 ? WHT : C565(0xf0, 0xe0, 0xd0));
      gfx->fillRoundRect(cx - 16, cy - 13, 32, 26, 7, f1);
      gfx->fillRoundRect(cx - 14, cy - 11, 28, 22, 6, f2);
      gfx->fillRoundRect(cx - 12, cy - 9, 24, 7, 3, f3);
      for (int k = 0; k < 3; k++) gfx->drawLine(X(4 + k * 4), cy - 10, X(4 + k * 4), cy + 10, f1);   // Fingerknoechel
      gfx->fillRoundRect(X(-6) - 5, cy + 6, 10, 7, 3, f1);                                           // Daumen
    } break;
    case TYPE_POISON: {                                            // Giftschleim
      uint16_t p1 = C565(0x70, 0x28, 0x8c), p2 = C565(0xa8, 0x48, 0xd0), p3 = C565(0xe0, 0xa8, 0xf8);
      gfx->fillCircle(X(-22), cy + 9, 5, p1); gfx->fillCircle(X(-34), cy + 13 + fl, 4, p1); gfx->fillCircle(X(-44), cy + 10, 3, p2);
      gfx->fillCircle(cx, cy, 14, p1); gfx->fillCircle(cx - 2, cy - 2, 11, p2); gfx->fillCircle(cx - 5, cy - 5, 3, p3);
      for (int k = 0; k < 3; k++) gfx->fillCircle(cx + (int)(cosf(an + k * 2.1f) * 18), cy - 16 + (int)(sinf(an * 1.3f + k * 2.1f) * 6) - k * 2, 3 + (k & 1), p3);
    } break;
    case TYPE_GROUND: {                                            // Schlammbombe mit Staub
      uint16_t m1 = C565(0x6a, 0x42, 0x20), m2 = C565(0x8a, 0x5c, 0x2e), m3 = C565(0xb4, 0x84, 0x4c), dust = C565(0xc8, 0xb4, 0x90);
      for (int k = 0; k < 3; k++) gfx->fillCircle(X(-24 - k * 14), cy + 10 - k * 4, 8 - k * 2, dust);
      gfx->fillCircle(cx, cy, 14, m1); gfx->fillCircle(cx - 3, cy - 2, 11, m2);
      gfx->fillCircle(cx - 5, cy + 3, 5, m1); gfx->fillCircle(cx + 5, cy - 4, 4, m3); gfx->fillCircle(cx + 3, cy + 6, 3, m3);
    } break;
    case TYPE_PSYCHIC: {                                           // Psywelle: wachsende Ringe
      uint16_t q1 = C565(0xf8, 0x58, 0xa8), q2 = C565(0xff, 0xa0, 0xd8), q3 = C565(0xb0, 0x30, 0x80);
      for (int k = 0; k < 3; k++) {
        int r = 6 + (int)((now / 30 + k * 10) % 30);
        uint16_t c = r < 18 ? q2 : (r < 28 ? q1 : q3);
        gfx->drawCircle(cx, cy, r, c); gfx->drawCircle(cx, cy, r + 1, c);
      }
      gfx->fillCircle(cx, cy, 7, q1); gfx->fillCircle(cx, cy, 4, q2); gfx->fillCircle(cx, cy, 2, WHT);
    } break;
    case TYPE_BUG: {                                               // Nadelschwarm
      uint16_t b1 = C565(0x98, 0xb8, 0x18), b2 = C565(0xd8, 0xe8, 0x50), blk = C565(0x28, 0x30, 0x10);
      for (int i = 0; i < 6; i++) {
        int bx = X(-i * 9), by = cy + (int)(sinf(an * 1.5f + i * 1.3f) * 15);
        gfx->fillTriangle(X(-i * 9 + 14), by, X(-i * 9 - 2), by - 3, X(-i * 9 - 2), by + 3, b2);      // Stachel
        gfx->fillCircle(bx - d * 3, by, 4, (i & 1) ? b1 : blk);
        gfx->drawLine(bx - d * 3, by - 3, bx - d * 6, by - 8, WHT);                                  // Fluegel
      }
    } break;
    case TYPE_ROCK: {                                              // Steinwurf
      uint16_t r1 = C565(0x78, 0x68, 0x50), r2 = C565(0xa0, 0x8c, 0x6c), r3 = C565(0xc8, 0xb4, 0x94), dust = C565(0xb0, 0xa4, 0x90);
      for (int k = 0; k < 3; k++) gfx->fillCircle(X(-22 - k * 12), cy + (k - 1) * 6, 5 - k, dust);
      static const uint8_t RR[7] = { 15, 12, 16, 11, 15, 12, 14 };
      int vx[7], vy[7];
      for (int i = 0; i < 7; i++) { float a = an * 1.2f + i * 0.8976f; vx[i] = cx + (int)(cosf(a) * RR[i]); vy[i] = cy + (int)(sinf(a) * RR[i]); }
      for (int i = 0; i < 7; i++) { int j = (i + 1) % 7; gfx->fillTriangle(cx, cy, vx[i], vy[i], vx[j], vy[j], (vy[i] + vy[j] < 2 * cy - 4) ? r3 : ((vx[i] + vx[j] > 2 * cx) ? r2 : r1)); }
    } break;
    case TYPE_GHOST: {                                             // Schattenball mit Geisterschweif
      uint16_t g1 = C565(0x38, 0x24, 0x60), g2 = C565(0x58, 0x38, 0x88), g3 = C565(0x84, 0x60, 0xb8);
      gfx->fillTriangle(X(-6), cy - 13, X(-6), cy + 13, X(-44 - fl * 3), cy + (fl - 1) * 7, g1);
      gfx->fillTriangle(X(-6), cy - 8, X(-6), cy + 8, X(-32), cy - (fl - 1) * 5, g2);
      gfx->fillCircle(cx, cy, 15, g2); gfx->fillCircle(cx, cy, 11, g3);
      gfx->fillCircle(X(4) - 0, cy - 4, 4, WHT); gfx->fillCircle(X(-5), cy - 4, 4, WHT);
      gfx->fillCircle(X(5), cy - 4, 2, C565(0xd0, 0x20, 0x40)); gfx->fillCircle(X(-4), cy - 4, 2, C565(0xd0, 0x20, 0x40));
      gfx->fillRoundRect(cx - 5, cy + 3, 10, 5, 2, g1);
    } break;
    case TYPE_DRAGON: {                                            // Drachenwut: blaues Feuer mit Zacken
      uint16_t a1 = C565(0x40, 0x38, 0xc8), a2 = C565(0x60, 0x90, 0xf8), a3 = C565(0xa0, 0xe8, 0xff);
      gfx->fillTriangle(X(-4), cy - 14, X(-4), cy + 14, X(-52 - fl * 4), cy + (fl - 1) * 3, a1);
      gfx->fillTriangle(X(-4), cy - 10, X(-4), cy + 10, X(-40 - fl * 3), cy - (fl - 1) * 3, a2);
      gfx->fillTriangle(X(-4), cy - 6, X(-4), cy + 6, X(-28 - fl * 2), cy, a3);
      gfx->fillTriangle(X(24), cy, X(8), cy - 13, X(8), cy + 13, a1);                                // Zacken vorn
      gfx->fillTriangle(X(8), cy - 18, X(-4), cy - 6, X(14), cy - 6, a2);
      gfx->fillTriangle(X(8), cy + 18, X(-4), cy + 6, X(14), cy + 6, a2);
      gfx->fillCircle(cx, cy, 14, a1); gfx->fillCircle(cx, cy, 10, a2); gfx->fillCircle(X(2), cy, 6, a3); gfx->fillCircle(X(3), cy, 3, WHT);
    } break;
    case TYPE_FLYING: {                                            // Windsichel
      uint16_t w1 = C565(0xe8, 0xf4, 0xff), w2 = C565(0xa8, 0xd0, 0xf0);
      for (int k = 0; k < 3; k++) gfx->drawLine(X(-26 - k * 8), cy + (k - 1) * 12, X(-54 - k * 8 - fl * 3), cy + (k - 1) * 12, k == 1 ? WHT : w2);
      const int N = 8;
      int ox[N + 1], oy[N + 1], ix[N + 1], iy[N + 1];
      for (int i = 0; i <= N; i++) {
        float a = (-75 + i * 150 / N) * 3.14159f / 180.0f;
        ox[i] = cx + d * (int)(cosf(a) * 22) - d * 6; oy[i] = cy + (int)(sinf(a) * 22);
        ix[i] = cx + d * (int)(cosf(a) * 17) - d * 16; iy[i] = cy + (int)(sinf(a) * 17);
      }
      uint16_t w3 = C565(0x5c, 0x90, 0xd0);                        // dunkler Rand, damit die Sichel auch vor hellem Himmel zu sehen ist
      for (int pass = 0; pass < 2; pass++) {
        int sh = pass == 0 ? -2 * d : 0;
        for (int i = 0; i < N; i++) {
          uint16_t c = pass == 0 ? w3 : (i < N / 2 ? w2 : w1);
          gfx->fillTriangle(ox[i] + sh, oy[i], ox[i + 1] + sh, oy[i + 1], ix[i] + sh, iy[i], c);
          gfx->fillTriangle(ox[i + 1] + sh, oy[i + 1], ix[i + 1] + sh, iy[i + 1], ix[i] + sh, iy[i], c);
        }
      }
    } break;
    case TYPE_DARK: {                                              // Finsterkugel mit Reisszaehnen
      uint16_t k1 = C565(0x14, 0x0c, 0x24), k2 = C565(0x32, 0x20, 0x54), k3 = C565(0x90, 0x50, 0xd0);
      gfx->fillTriangle(X(-6), cy - 12, X(-6), cy + 12, X(-40 - fl * 3), cy, k2);
      gfx->fillCircle(cx, cy, 17, k3); gfx->fillCircle(cx, cy, 15, k1); gfx->fillCircle(X(3), cy, 9, k2);
      gfx->fillTriangle(X(14), cy - 8, X(6), cy - 14, X(6), cy - 4, WHT);
      gfx->fillTriangle(X(14), cy + 8, X(6), cy + 14, X(6), cy + 4, WHT);
      gfx->fillCircle(X(-2), cy - 3, 2, C565(0xff, 0x40, 0x60)); gfx->fillCircle(X(-2), cy + 3, 2, C565(0xff, 0x40, 0x60));
    } break;
    case TYPE_STEEL: {                                             // Stahlstern
      uint16_t s1 = C565(0x78, 0x80, 0x90), s2 = C565(0xb4, 0xbc, 0xcc), s3 = C565(0xe8, 0xee, 0xf8);
      shotStar(cx, cy, 22, 7, an * 1.4f, s1, s2, 4);
      gfx->fillCircle(cx, cy, 6, s2); gfx->fillCircle(cx - 2, cy - 2, 3, WHT);
      for (int k = 0; k < 3; k++) gfx->drawLine(X(-24 - k * 6), cy + (k - 1) * 9, X(-40 - k * 8), cy + (k - 1) * 9, s3);
    } break;
    default: {                                                     // Normal: weisse Energiekugel mit Strahlen
      uint16_t n1 = C565(0xa0, 0xa8, 0xb8), n2 = C565(0xe8, 0xe8, 0xf0);
      gfx->fillCircle(cx, cy, 14, n1); gfx->fillCircle(cx, cy, 11, n2); gfx->fillCircle(cx, cy, 6, WHT);
      for (int k = 0; k < 4; k++) { float a = an + k * 1.5708f; gfx->drawLine(cx + (int)(cosf(a) * 14), cy + (int)(sinf(a) * 14), cx + (int)(cosf(a) * 22), cy + (int)(sinf(a) * 22), n2); }
    } break;
  }
}

// Aufprallfunke in Typfarbe (seit 6.65): kurzer Strahlenkranz am Ziel, p = 0..100 (Fortschritt), strong = harter Treffer.
void drawBattleImpact(uint8_t type, int cx, int cy, int p, bool strong) {
  static const uint16_t COL[17][2] = {
    { C565(0xe8, 0xe8, 0xf0), C565(0xff, 0xff, 0xff) }, { C565(0xff, 0x8a, 0x20), C565(0xff, 0xd8, 0x50) },
    { C565(0x4a, 0x9c, 0xf0), C565(0xa8, 0xdc, 0xff) }, { C565(0x68, 0xd0, 0x48), C565(0xb8, 0xf0, 0x90) },
    { C565(0xff, 0xe8, 0x40), C565(0xff, 0xfb, 0xc8) }, { C565(0x4c, 0x9c, 0xd8), C565(0xe4, 0xf8, 0xff) },
    { C565(0xe0, 0x80, 0x30), C565(0xf8, 0xb8, 0x68) }, { C565(0xa8, 0x48, 0xd0), C565(0xe0, 0xa8, 0xf8) },
    { C565(0x8a, 0x5c, 0x2e), C565(0xc8, 0xb4, 0x90) }, { C565(0xf8, 0x58, 0xa8), C565(0xff, 0xa0, 0xd8) },
    { C565(0x98, 0xb8, 0x18), C565(0xd8, 0xe8, 0x50) }, { C565(0xa0, 0x8c, 0x6c), C565(0xc8, 0xb4, 0x94) },
    { C565(0x78, 0x58, 0xb0), C565(0xb0, 0x90, 0xe0) }, { C565(0x60, 0x90, 0xf8), C565(0xa0, 0xe8, 0xff) },
    { C565(0xa8, 0xd0, 0xf0), C565(0xff, 0xff, 0xff) }, { C565(0x90, 0x50, 0xd0), C565(0x32, 0x20, 0x54) },
    { C565(0xb4, 0xbc, 0xcc), C565(0xe8, 0xee, 0xf8) } };
  if (type > 16) type = 0;
  uint16_t c1 = COL[type][0], c2 = COL[type][1];
  int rad = 8 + p * (strong ? 44 : 30) / 100;
  int flash = (100 - p) * (strong ? 26 : 18) / 100;
  if (flash > 2) gfx->fillCircle(cx, cy, flash, c2);
  gfx->drawCircle(cx, cy, rad * 7 / 10, c1); gfx->drawCircle(cx, cy, rad * 7 / 10 + 1, c1);
  const int N = strong ? 10 : 8;
  for (int k = 0; k < N; k++) {
    float a = k * 6.28318f / N + 0.3f;
    int r0 = rad / 2, x0 = cx + (int)(cosf(a) * r0), y0 = cy + (int)(sinf(a) * r0);
    int x1 = cx + (int)(cosf(a) * rad), y1 = cy + (int)(sinf(a) * rad);
    if (type == TYPE_ELECTRIC) { x1 += (k & 1) ? 4 : -4; y1 += (k & 1) ? -4 : 4; }     // zackige Strahlen
    gfx->drawLine(x0, y0, x1, y1, c1); gfx->drawLine(x0 + 1, y0, x1 + 1, y1, c1);
    if (type == TYPE_WATER || type == TYPE_POISON || type == TYPE_GROUND || type == TYPE_BUG) gfx->fillCircle(x1, y1, 3, c2);   // Tropfen / Brocken
    else if (type == TYPE_ICE) { gfx->drawLine(x1 - 3, y1, x1 + 3, y1, c2); gfx->drawLine(x1, y1 - 3, x1, y1 + 3, c2); }          // Eiskristalle
    else gfx->fillRect(x1 - 1, y1 - 1, 3, 3, c2);
  }
}

void renderBattle() {
  uint32_t now = millis();
  if (LOOK_B) drawBattleSceneB();  // Optik B: Landschaft des Pokemon, tiefer gesetzt
  else drawGameScene();            // Optik A: fondo del habitat, ya adaptado a la hora del dia
  stepBattle();

  int px = 140, ox = 326, groundY = 340;
  bool weAttack = (battleTurn == 0);

  // pose de cada bicho segun la fase: quien ataca se anima, a quien le toca
  // encajar el golpe muestra PMD_HURT justo al impacto (fase 2), el otro
  // queda en pose feliz/pose normal
  uint8_t playerAct = PMD_IDLE, oppAct = PMD_IDLE;
  if (battlePhase == 1) {  // proyectil en vuelo: el atacante ataca
    if (weAttack) playerAct = pmd.has(PMD_ATTACK) ? PMD_ATTACK : PMD_IDLE;
    else oppAct = battlePmd.has(PMD_ATTACK) ? PMD_ATTACK : PMD_IDLE;
  } else if (battlePhase == 2) {  // impacto: quien lo recibe se ve triste, quien
                                   // golpeo se ve contento (si el golpe no fallo)
    if (battleLastHit > 0) {
      if (weAttack) {
        oppAct = battlePmd.has(PMD_HURT) ? PMD_HURT : PMD_IDLE;
        playerAct = pmd.has(PMD_POSE) ? PMD_POSE : PMD_IDLE;
      } else {
        playerAct = pmd.has(PMD_HURT) ? PMD_HURT : PMD_IDLE;
        oppAct = battlePmd.has(PMD_POSE) ? PMD_POSE : PMD_IDLE;
      }
    }
  }
  if (battleOver) { playerAct = PMD_IDLE; oppAct = PMD_IDLE; }

  if (pmd.loaded && battlePlayerHp > 0) drawPmdAct(playerAct, px, groundY, now, true, false, 4, false);
  if (battlePmd.loaded && battleOppHp > 0) drawPmdActM(battlePmd, oppAct, ox, groundY, now, true, false, 4, true);

  // proyectil "bola de energia" viajando de quien ataca a quien defiende
  if (battlePhase == 1) {
    float t = 1.0f - (float)(battlePhaseUntil - now) / 550.0f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float sx = weAttack ? px : ox, ex = weAttack ? ox : px;
    sx += (ex > sx ? 1 : -1) * BATTLE_SHOT_START;   // seit 6.65: Start weiter zur Sprite-Mitte hin (Richtung Gegner versetzt)
    battleBallX = sx + (ex - sx) * t;
    battleBallY = groundY - 90;
    drawBattleShot(DEX_TBL[weAttack ? pet.speciesId : battleOppDex].type, (int)battleBallX, (int)battleBallY, ex > sx ? 1 : -1, now);   // seit 6.65: Geschoss je Typ
  }

  if (battlePhase == 2 && battleLastHit > 0) {   // seit 6.65: Aufprallfunke in Typfarbe am getroffenen Pokemon (erste 450 ms)
    uint32_t el = battlePhaseUntil > now ? 900 - (battlePhaseUntil - now) : 900;
    if (el < 450) drawBattleImpact(DEX_TBL[weAttack ? pet.speciesId : battleOppDex].type, weAttack ? ox : px, groundY - 90, (int)(el * 100 / 450), battleLastHit == 2);
  }
  drawBattleHpBar(px - 60, groundY + 20, 120, battlePlayerHp, battlePlayerMaxHp);
  drawBattleHpBar(ox - 60, groundY + 20, 120, battleOppHp, battleOppMaxHp);

  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  char ourLine[26], oppLine[26];
  snprintf(ourLine, sizeof(ourLine), "%s Lv.%u", dexName(pet.speciesId), pet.level());
  snprintf(oppLine, sizeof(oppLine), "%s Lv.%u", dexName(battleOppDex), battleOppLevel);
  gfx->setCursor(CX - (int)strlen(ourLine) * 6, 22);
  gfx->print(ourLine);
  gfx->setCursor(CX - 12, 44);
  gfx->print("VS");
  gfx->setCursor(CX - (int)strlen(oppLine) * 6, 66);
  gfx->print(oppLine);

  // tipo de cada uno, justo encima de su sprite -- entre los nombres de
  // arriba y los bichos, para ver de un vistazo quien tiene ventaja.
  // Desaparece en cuanto se conoce el resultado: se solapaba con el
  // mensaje de victoria/derrota
  if (!battleOver) {
    uint8_t ourType = DEX_TBL[pet.speciesId].type;
    uint8_t oppType = DEX_TBL[battleOppDex].type;
    const char *ourTypeName = typeName(ourType);
    const char *oppTypeName = typeName(oppType);
    printUmlautOutlined(ourTypeName, px - umlautLen(ourTypeName) * 9, 124, 3, TYPE_COLOR[ourType]);
    printUmlautOutlined(oppTypeName, ox - umlautLen(oppTypeName) * 9, 124, 3, TYPE_COLOR[oppType]);
  }

  if (battleOver) {
    const char *msg = battleWon ? T(S_BATTLE_WIN) : T(S_BATTLE_LOSE);
    gfx->setTextSize(3);
    gfx->setCursor(CX - strlen(msg) * 9, 115);
    gfx->print(msg);
    char def[14];
    if (pet.trDef >= 100) strncpy(def, T(S_DEF_MAX), sizeof(def) - 1);
    else snprintf(def, sizeof(def), T(S_DEF_GAIN_FMT), battleDefGain);
    def[sizeof(def) - 1] = 0;
    gfx->setTextSize(2);
    gfx->setCursor(CX - (int)strlen(def) * 6, 148);
    gfx->print(def);
    // el corazon (solo tras una victoria) dura 1.5s, menos que estos 3s de
    // pantalla de resultado: sin pintarlo aqui, nunca llegaba a verse
    // (mismo problema ya corregido en saco y ballspiel)
    if (battleWon) {
      if (pet.showHeart()) {
        drawMap(SPR_HEART, 32, CX - 32, 175, 2, false);
      } else if (pet.winStreak >= 10) {
        const char *champ = T(S_CHAMPION_TXT);
        gfx->setTextColor(UI_INK);
        gfx->setTextSize(3);
        gfx->setCursor(CX - (int)strlen(champ) * 9, 175);
        gfx->print(champ);
      } else {
        char streak[26];
        snprintf(streak, sizeof(streak), T(S_WIN_STREAK_FMT), pet.winStreak);
        gfx->setTextColor(UI_INK);
        gfx->setTextSize(2);
        gfx->setCursor(CX - (int)strlen(streak) * 6, 175);
        gfx->print(streak);
      }
    }
  }

  gfx->flush();
}

// ---------- ficha del bicho (deslizar vertical) ----------

void drawCardStat(int y, const char *label, uint16_t val, uint16_t maxBar, uint16_t color) {
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(150 - (int)strlen(label) * 12 - 12, y);
  gfx->print(label);
  char num[8];
  snprintf(num, sizeof(num), "%u", val);
  gfx->setCursor(330, y);
  gfx->print(num);
  int bw = 160;
  int fw = (int)val * bw / maxBar;
  if (fw > bw) fw = bw;
  gfx->fillRoundRect(150, y + 2, bw, 11, 3, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(150, y + 2, fw, 11, 3, color);
}

// fila de peso: en vez de un numero muestra "delgado/normal/grueso" y el
// balance colorea en tercios (verde/amarillo/rojo); en 0 deja un sliver
// minimo visible para que la barra nunca se vea totalmente vacia
void drawWeightStat(int y) {
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(150 - (int)strlen(T(S_STAT_WGT)) * 12 - 12, y);
  gfx->print(T(S_STAT_WGT));
  StrId wlbl = pet.weight <= 33 ? S_WGT_THIN : (pet.weight <= 66 ? S_WGT_NORMAL : (pet.weight < 100 ? S_WGT_THICK : S_WGT_FAT));
  uint16_t color = pet.weight <= 33 ? UI_BAR_OK : (pet.weight <= 66 ? UI_BAR_WARN : UI_BAR_BAD);
  printUmlaut(T(wlbl), 330, y, 2, UI_INK);
  int bw = 160;
  int fw = (int)pet.weight * bw / 100;
  if (fw > bw) fw = bw;
  if (fw < 18) fw = 18;  // sliver minimo, incluso en peso 0 (x3 mas largo que antes)
  gfx->fillRoundRect(150, y + 2, bw, 11, 3, UI_TRACK);
  gfx->fillRoundRect(150, y + 2, fw, 11, 3, color);
}

// ---------- ajuste de hora en pantalla (deslizar abajo) ----------
// El usuario pone su hora LOCAL a ojo; el firmware la usa tal cual, asi que
// no hay que gestionar zona horaria. Preserva el dia (no rompe racha/edad).

bool clockOrigAudio = true;   // valores al abrir el menu, para poder deshacer
Lang clockOrigLang = LANG_DE;  // el cambio si se cancela sin pulsar OK
uint8_t clockOrigLook = 0;

// Optik A/B laden bzw. speichern (gleiches Muster wie Ton und Sprache)
void loadLook() {
  Preferences p;
  p.begin("tamapoke", true);
  uint8_t v = p.getUChar("look", 0);
  p.end();
  gLook = (v == 1) ? 1 : 0;
}

void setLook(uint8_t v) {
  gLook = (v == 1) ? 1 : 0;
  Preferences p;
  p.begin("tamapoke", false);
  p.putUChar("look", gLook);
  p.end();
}

void openClock() {
  uint32_t e = pet.lastSeenEpoch ? pet.lastSeenEpoch : rtcEpoch();
  clockH = (e / 3600) % 24;
  clockM = (e / 60) % 60;
  clockDeltaMin = 0;
  clockOpen = true;
  clockPeekOpen = false;
  clockOrigAudio = audioEnabled();
  clockOrigLang = gLang;
  clockOrigLook = gLook;
}

// deshace un cambio de sonido/idioma hecho en el menu de la hora sin
// confirmar con OK -- se llama en cualquier salida que no sea applyClock()
void revertClockAudioLang() {
  if (audioEnabled() != clockOrigAudio) audioSetEnabled(clockOrigAudio);
  if (gLang != clockOrigLang) setLang(clockOrigLang);
  if (gLook != clockOrigLook) setLook(clockOrigLook);
}

void openWeekdaySet() {
  weekdayEdit = pet.weekday;
  weekdaySetOpen = true;
  clockPeekOpen = false;
}

// ajuste manual del dia de la semana: flechas para elegir, OK para confirmar
void renderWeekdaySet() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  const char *wd = weekdayName(weekdayEdit);
  gfx->setCursor(CX - (int)strlen(wd) * 9, 207);
  gfx->print(wd);

  drawClockBtn(77, 190, "<");
  drawClockBtn(331, 190, ">");

  gfx->fillRoundRect(133, 300, 200, 48, 14, UI_BAR_OK);
  gfx->setTextColor(UI_BG_DAY);
  gfx->setTextSize(3);
  gfx->setCursor(CX - 18, 312);
  gfx->print("OK");

  gfx->flush();
}

void weekdaySetTap(int16_t x, int16_t y) {
  if (y >= 165 && y < 273) {
    if (x >= 52 && x < 155) weekdayEdit = (weekdayEdit + 6) % 7;
    else if (x >= 311 && x < 414) weekdayEdit = (weekdayEdit + 1) % 7;
    return;
  }
  if (y >= 300 && y <= 348 && x >= 133 && x <= 333) {
    pet.setWeekday(weekdayEdit);
    weekdaySetOpen = false;
  }
}

// nombre del dia de la semana (0=domingo..6=sabado)
// -------- truco de umlaut: la fuente bitmap no tiene a/o/u con dieresis ni
// eszett, asi que se dibuja la letra base + dos puntos encima a mano. En el
// texto, un backtick (`) justo ANTES de una letra marca que esa letra lleva
// los puntos (p.ej. "L`oschen" imprime "Loschen" con puntos sobre la o) --
// el backtick en si no se imprime ni cuenta para el ancho/centrado
int umlautLen(const char *s) {
  int n = 0;
  for (int i = 0; s[i]; i++) {
    if (s[i] == '`') continue;
    n++;
  }
  return n;
}
void printUmlaut(const char *s, int x, int y, uint8_t size, uint16_t color) {
  int cw = 6 * size;
  int cx = x;
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  for (int i = 0; s[i]; i++) {
    char c = s[i];
    bool umlaut = false;
    if (c == '`' && s[i + 1]) { umlaut = true; i++; c = s[i]; }
    gfx->setCursor(cx, y);
    gfx->print(c);
    if (umlaut) {
      int d = (size >= 3) ? 2 : 1;
      int dotY = y - d * 3 + 3;
      gfx->fillCircle(cx + cw / 2 - d * 2 - 2, dotY, d, color);  // punto izquierdo: 1px mas a la izquierda
      gfx->fillCircle(cx + cw / 2 + d * 2 - 1, dotY, d, color);  // punto derecho: sin cambio
    }
    cx += cw;
  }
}

// como printUmlaut() pero con un contorno blanco alrededor -- para texto
// legible encima de fondos que cambian de color (dia/noche)
void printUmlautOutlined(const char *s, int x, int y, uint8_t size, uint16_t color) {
  static const int8_t off[8][2] = { { -2, -2 }, { 0, -2 }, { 2, -2 }, { -2, 0 },
                                     { 2, 0 }, { -2, 2 }, { 0, 2 }, { 2, 2 } };
  for (int i = 0; i < 8; i++)
    printUmlaut(s, x + off[i][0], y + off[i][1], size, UI_WHITE);
  printUmlaut(s, x, y, size, color);
}

// imprime un texto de hasta 3 lineas separadas por '\n', cada una centrada
// en X y con soporte de umlaut; y0 es la primera linea, lineH la distancia
// entre lineas
void printLinesCentered(const char *s, int y0, int lineH, uint8_t size, uint16_t color, int maxLines = 3) {
  char buf[100];
  strncpy(buf, s, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  char *line = buf;
  int y = y0;
  for (int i = 0; i < maxLines && line; i++) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    printUmlaut(line, CX - umlautLen(line) * (size * 3), y, size, color);
    y += lineH;
    line = nl ? nl + 1 : nullptr;
  }
}

// estrella de 5 puntas rellena, mediante un abanico de 10 triangulos desde
// el centro (funciona porque una estrella de 5 puntas es "estrellada"
// respecto a su propio centro)
void fillStar(int cx, int cy, int outerR, int innerR, uint16_t color) {
  float px[10], py[10];
  for (int i = 0; i < 10; i++) {
    float ang = -(float)PI / 2 + i * (float)PI / 5;
    float r = (i % 2 == 0) ? outerR : innerR;
    px[i] = cx + cosf(ang) * r;
    py[i] = cy + sinf(ang) * r;
  }
  for (int i = 0; i < 10; i++) {
    int j = (i + 1) % 10;
    gfx->fillTriangle(cx, cy, (int)px[i], (int)py[i], (int)px[j], (int)py[j], color);
  }
}

const char *weekdayName(uint8_t wd) {
  static const StrId names[7] = { S_WD_SUN, S_WD_MON, S_WD_TUE, S_WD_WED, S_WD_THU, S_WD_FRI, S_WD_SAT };
  return T(names[wd % 7]);
}

// vista previa antes de los ajustes completos: mismo fondo+bicho en
// movimiento que la pantalla principal, pero solo con la hora y el dia de
// la semana encima (sin nombre, sin barras, sin estadisticas). Mantener
// pulsado la mitad de arriba pasa a los ajustes de hora; la mitad de abajo,
// a los ajustes del dia de la semana. Deslizar en cualquier direccion
// vuelve al juego (ver onSwipeV)
void renderClockPeek() {
  int h = sceneHour();
  bool night = pet.sleeping || h < 6 || h >= 20;
  gClockScene = true;
  drawScene(pet.isEgg() ? 0 : biomeFor(pet.speciesId), millis(), night);
  gClockScene = false;
  if (!pet.isEgg() && !pet.blocked()) drawPet();

  uint32_t liveNow = rtcEpoch();
  if (!liveNow) liveNow = pet.lastSeenEpoch;
  int32_t secOfDay = (int32_t)(((liveNow % 86400) + 86400) % 86400);
  char t[8];
  snprintf(t, sizeof(t), "%02d:%02d", (int)(secOfDay / 3600), (int)((secOfDay / 60) % 60));
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  gfx->setTextColor(ink);
  gfx->setTextSize(7);
  gfx->setCursor(CX - 105, 108);
  gfx->print(t);

  const char *wd = weekdayName(pet.weekday);
  gfx->setTextSize(5);
  gfx->setCursor(CX - (int)strlen(wd) * 15, 338);
  gfx->print(wd);

  gfx->flush();
}

void applyClock() {
  // se aplica el desplazamiento sobre la hora real ACTUAL (recien leida),
  // no sobre una foto de cuando se abrio el menu: asi el tiempo que paso
  // mientras se estaba en el menu no se pierde
  uint32_t liveNow = rtcEpoch();
  if (!liveNow) liveNow = pet.lastSeenEpoch;
  uint32_t e = (uint32_t)((int64_t)liveNow + (int64_t)clockDeltaMin * 60);
  e = (e / 60) * 60;  // Sekunden immer auf 00, egal was die Live-Uhrzeit gerade zeigte
  uint32_t oldEpochForGuard = pet.lastSeenEpoch;  // vor setClock() einfangen, da danach ueberschrieben
  rtcSetEpoch(e);
  pet.setClockManual(e);  // Schlaf-/Wachwechsel durch die Uhr vergibt kein "Perfect Day"
  pet.guardClockJump((uint32_t)(clockDeltaMin < 0 ? -clockDeltaMin : clockDeltaMin), oldEpochForGuard);  // evita fabricar dias/rachas/peso-verde con saltos grandes
  sfxPlay(SFX_HEART);
  clockOpen = false;
  clockDeleteOpen = false;
  clockSavedOpen = true;
  clockSavedUntil = millis() + 3000;
}

// aviso "gespeichert" tras pulsar OK en el menu de la hora: mismo fondo que
// ese menu, texto negro, dura lo mismo que el corazon antes de volver solo
// al juego
void renderClockSaved() {
  if (millis() >= clockSavedUntil) { clockSavedOpen = false; return; }
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(RGB565_BLACK);

  gfx->setTextSize(2);
  const char *l1 = T(S_CLOCK_SAVED);
  gfx->setCursor(CX - (int)strlen(l1) * 6, 60);
  gfx->print(l1);

  gfx->setTextSize(4);
  const char *l2 = T(S_APP_NAME);
  gfx->setCursor(CX - (int)strlen(l2) * 12, 106);
  gfx->print(l2);

  gfx->setTextSize(2);
  const char *l3 = T(S_VERSION_LBL);
  gfx->setCursor(CX - (int)strlen(l3) * 6, 170);
  gfx->print(l3);

  gfx->setTextSize(4);
  const char *l4 = T(S_VERSION_NAME);
  gfx->setCursor(CX - (int)strlen(l4) * 12, 206);
  gfx->print(l4);

  gfx->setTextSize(2);
  const char *l5 = T(S_COUNTRY);
  gfx->setCursor(CX - (int)strlen(l5) * 6, 270);
  gfx->print(l5);

  drawMap(SPR_HEART, 32, CX - 32, 310, 2, false);
  gfx->flush();
}

void drawClockBtn(int x, int y, const char *l) {
  gfx->fillRoundRect(x, y, 58, 58, 12, UI_WHITE);
  gfx->drawRoundRect(x, y, 58, 58, 12, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(4);
  gfx->setCursor(x + 17, y + 15);
  gfx->print(l);
}

// pildoras de idioma centradas en y; rellena la activa
#define LANG_PILL_Y 280
#define LANG_PILL_H 30
#define LANG_PILL_X 336          // pildora de idioma (cicla los 6 al tocar)
#define LANG_PILL_W 96
#define LOOK_PILL_X 171          // Optik-Pille (Mitte), breit genug fuer "Ansicht A"
#define LOOK_PILL_W 124
static const char *const LANG_CODES[LANG_COUNT] = { "EN", "DE", "EN/DE", "DE/EN" };

void renderClock() {
  // se recalcula cada frame a partir de la hora real actual + el ajuste
  // hecho con los botones: asi la pantalla avanza sola, se puede usar como
  // reloj normal, y +/- sigue funcionando exactamente igual que antes
  uint32_t liveNow = rtcEpoch();
  if (!liveNow) liveNow = pet.lastSeenEpoch;
  int64_t adj = (int64_t)liveNow + (int64_t)clockDeltaMin * 60;
  int32_t secOfDay = (int32_t)(((adj % 86400) + 86400) % 86400);
  clockH = secOfDay / 3600;
  clockM = (secOfDay / 60) % 60;

  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  printUmlaut(T(S_SET_TIME), CX - umlautLen(T(S_SET_TIME)) * 9, 44, 3, UI_INK);

  char t[8];
  snprintf(t, sizeof(t), "%02d:%02d", clockH, clockM);
  gfx->setTextSize(7);
  gfx->setCursor(CX - 105, 108);
  gfx->print(t);

  drawClockBtn(104, 190, "-");  // hora -
  drawClockBtn(170, 190, "+");  // hora +
  drawClockBtn(252, 190, "-");  // min -
  drawClockBtn(318, 190, "+");  // min +
  gfx->setTextSize(2);
  gfx->setTextColor(UI_INK);
  gfx->setCursor(120, 256);
  gfx->print(T(S_HOUR));
  gfx->setCursor(276, 256);
  gfx->print(T(S_MIN));

  // interruptor de sonido (izquierda de la fila de idioma)
  bool snd = audioEnabled();
  const char *sl = snd ? T(S_SND_ON) : T(S_SND_OFF);
  gfx->fillRoundRect(34, LANG_PILL_Y, 96, LANG_PILL_H, 8, snd ? UI_BAR_OK : UI_WHITE);
  gfx->drawRoundRect(34, LANG_PILL_Y, 96, LANG_PILL_H, 8, UI_INK);
  gfx->setTextColor(snd ? UI_BG_DAY : UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(34 + (96 - (int)strlen(sl) * 12) / 2, LANG_PILL_Y + 8);
  gfx->print(sl);

  // Optik A/B (Mitte der Reihe, zwischen Ton und Sprache)
  {
    bool deUi = (gLang == LANG_DE || gLang == LANG_DE_EN);
    const char *ll = deUi ? (LOOK_B ? "Ansicht B" : "Ansicht A") : (LOOK_B ? "Look B" : "Look A");
    gfx->fillRoundRect(LOOK_PILL_X, LANG_PILL_Y, LOOK_PILL_W, LANG_PILL_H, 8, UI_WHITE);
    gfx->drawRoundRect(LOOK_PILL_X, LANG_PILL_Y, LOOK_PILL_W, LANG_PILL_H, 8, UI_INK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(LOOK_PILL_X + (LOOK_PILL_W - (int)strlen(ll) * 12) / 2, LANG_PILL_Y + 8);
    gfx->print(ll);
  }

  // selector de idioma: una pildora que cicla los 6 idiomas al tocar
  gfx->fillRoundRect(LANG_PILL_X, LANG_PILL_Y, LANG_PILL_W, LANG_PILL_H, 8, UI_WHITE);
  gfx->drawRoundRect(LANG_PILL_X, LANG_PILL_Y, LANG_PILL_W, LANG_PILL_H, 8, UI_INK);
  char lp[10];
  snprintf(lp, sizeof(lp), "%s >", LANG_CODES[gLang]);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(LANG_PILL_X + (LANG_PILL_W - (int)strlen(lp) * 12) / 2, LANG_PILL_Y + 8);
  gfx->print(lp);

  // borrar todo: mantener pulsado unos segundos para abrir la confirmacion
  gfx->fillRoundRect(133, 318, 200, 40, 10, UI_BAR_BAD);
  gfx->setTextColor(UI_BG_DAY);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(T(S_CLOCK_DELETE_L1)) * 6, 323);
  gfx->print(T(S_CLOCK_DELETE_L1));
  printUmlaut(T(S_CLOCK_DELETE_L2), CX - umlautLen(T(S_CLOCK_DELETE_L2)) * 6, 341, 2, UI_BG_DAY);

  gfx->fillRoundRect(133, 366, 200, 48, 14, UI_BAR_OK);
  gfx->setTextColor(UI_BG_DAY);
  gfx->setTextSize(3);
  gfx->setCursor(CX - 18, 378);
  gfx->print("OK");

  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_CLOCK_CANCEL)) * 6, 428);
  gfx->print(T(S_CLOCK_CANCEL));

  // version del firmware (abajo del todo)
  char ver[20];
  snprintf(ver, sizeof(ver), "TamaPoke v%s", FW_VERSION);
  gfx->setTextColor(RGB565_BLACK);
  gfx->setTextSize(1);
  gfx->setCursor(CX - (int)strlen(ver) * 3, 452);
  gfx->print(ver);
  gfx->flush();
}

// pantalla de confirmacion de borrado: numeros 1-6 en circulo, hay que
// tocarlos en orden ascendente o descendente para confirmar. Cualquier
// fallo o los 10s cumplidos devuelven a la pantalla principal sin borrar
void renderClockDelete() {
  uint32_t now = millis();
  if (clockDeleteSuccess) {
    if (now >= clockDeleteSuccessUntil) {
      pet.factoryReset();
      delay(100);
      ESP.restart();
    }
    gfx->fillScreen(RGB565_BLACK);
    gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(3);
    printUmlaut(T(S_DELETE_DONE1), CX - umlautLen(T(S_DELETE_DONE1)) * 9, 190, 3, UI_INK);
    gfx->setCursor(CX - strlen(T(S_DELETE_DONE2)) * 9, 230);
    gfx->print(T(S_DELETE_DONE2));
    gfx->flush();
    return;
  }
  if (now >= clockDeleteUntil) {  // se acabo el tiempo: vuelve sin borrar nada
    revertClockAudioLang();
    clockDeleteOpen = false;
    clockOpen = false;
    return;
  }

  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  const char *w1 = T(S_DELETE_CONFIRM1), *w2 = T(S_DELETE_CONFIRM2), *w3 = T(S_DELETE_CONFIRM3);
  gfx->setCursor(CX - (int)strlen(w1) * 6, 190);
  gfx->print(w1);
  gfx->setCursor(CX - (int)strlen(w2) * 6, 208);
  gfx->print(w2);
  printUmlaut(w3, CX - umlautLen(w3) * 6, 226, 2, UI_INK);

  for (int i = 0; i < 6; i++) {
    float a = -(float)PI / 2 + i * (2.0f * (float)PI / 6);
    int bx = CX + (int)(cosf(a) * 150) - 25, by = CY + (int)(sinf(a) * 150) - 25;
    bool marked = clockDeleteMarked[i];
    gfx->fillRoundRect(bx, by, 50, 50, 12, marked ? UI_BAR_BAD : UI_TRACK);
    char num[2];
    snprintf(num, sizeof(num), "%d", i + 1);
    gfx->setTextColor(UI_BG_DAY);
    gfx->setTextSize(3);
    gfx->setCursor(bx + 17, by + 13);
    gfx->print(num);
  }

  // cuenta atras, debajo de las tres lineas de aviso
  char secs[8];
  snprintf(secs, sizeof(secs), "%u", (unsigned)((clockDeleteUntil - now) / 1000 + 1));
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - (int)strlen(secs) * 9, 272);
  gfx->print(secs);

  gfx->flush();
}

void clockTap(int16_t x, int16_t y) {
  if (y >= 175 && y <= 252) {  // fila de botones +/- (zonas ampliadas, sin huecos entre botones)
    if (x >= 90 && x < 166) { clockH = (clockH + 23) % 24; clockDeltaMin -= 60; }
    else if (x >= 166 && x < 240) { clockH = (clockH + 1) % 24; clockDeltaMin += 60; }
    else if (x >= 240 && x < 314) { clockM = (clockM + 59) % 60; clockDeltaMin -= 1; }
    else if (x >= 314 && x < 390) { clockM = (clockM + 1) % 60; clockDeltaMin += 1; }
    return;
  }
  if (y >= 268 && y <= 322) {  // fila de sonido/idioma (zona ampliada, aprovecha el hueco grande entre las dos pildoras)
    if (x >= 20 && x < 168) {                  // interruptor de sonido
      audioSetEnabled(!audioEnabled());
      sfxPlay(SFX_HEART);  // solo se oye si quedo encendido (la tarea de audio filtra por gOn)
      return;
    }
    if (x >= 168 && x < 310) {  // Optik A/B umschalten
      setLook(gLook ? 0 : 1);
      sfxPlay(SFX_HEART);
      return;
    }
    if (x >= 310 && x < 446) {  // cicla idioma
      setLang((Lang)((gLang + 1) % LANG_COUNT));
      return;
    }
  }
  if (y >= 362 && y <= 425 && x >= 118 && x <= 348) { applyClock(); return; }
}

// llama + numero de racha arriba a la izquierda
void drawStreakBadge() {
  if (pet.streak < 1) return;
  int x = 26, y = 16;
  gfx->fillTriangle(x + 8, y, x + 1, y + 17, x + 15, y + 17, UI_BAR_BAD);
  gfx->fillTriangle(x + 8, y + 7, x + 4, y + 17, x + 12, y + 17, UI_BAR_WARN);
  char s[6];
  snprintf(s, sizeof(s), "%u", pet.streak);
  gfx->setTextColor(inkColor());
  gfx->setTextSize(2);
  gfx->setCursor(x + 22, y + 2);
  gfx->print(s);
}

// separa los miles con un espacio: 300000 -> "300 000"
void formatThousands(char *buf, size_t bufsize, uint32_t n) {
  char tmp[16];
  snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)n);
  int len = (int)strlen(tmp), pos = 0;
  for (int i = 0; i < len && pos < (int)bufsize - 1; i++) {
    if (i > 0 && (len - i) % 3 == 0) buf[pos++] = ' ';
    buf[pos++] = tmp[i];
  }
  buf[pos] = 0;
}

// banner temporal: medalla, hito de pasos o record de racha. Se consume la
// cola de pet (achvKind/achvValue) de uno en uno, con el doble de duracion
// que antes, para que cada logro se vea por separado con claridad

void drawCelebration() {
  if (pet.achvCount == 0) { achvShowUntil = 0; return; }
  uint32_t now = millis();
  if (achvShowUntil == 0) {
    achvShowUntil = 1;  // marca que ya se inicio (ya no es un plazo de caducidad)
    achvShowStart = now;
    if (!pet.sleeping) sfxPlay(SFX_MEDAL);
  }
  // sin caducidad automatica: solo se cierra al tocar (ver handleTouch, con
  // una espera minima de 2s para que no se salte por un toque accidental)

  uint8_t kind = pet.achvKind[0];
  uint32_t value = pet.achvValue[0];
  const char *l1 = nullptr;
  char buf[24], numbuf[16];
  if (kind == 0) {  // medalla
    l1 = T(S_MEDAL_BANNER);
    const char *nm = "";
    for (int i = 0; i < MED_COUNT; i++)
      if (value & (1u << i)) { nm = medalDesc(i); break; }
    strncpy(buf, nm, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
  } else if (kind == 1) {  // hito de pasos
    l1 = T(S_MILESTONE_BANNER);
    formatThousands(numbuf, sizeof(numbuf), value);
    snprintf(buf, sizeof(buf), T(S_STEP_MILESTONE_FMT), numbuf);
  } else if (kind == 2) {  // record de racha
    l1 = T(S_STREAK_RECORD_BANNER);
    snprintf(buf, sizeof(buf), T(S_STREAK_RECORD_FMT), (unsigned)value);
  } else {  // kind == 3: nueva etapa del pokedex
    l1 = T(S_TIER_UP_BANNER);
    const char *rn = (value >= 1 && value <= 10) ? T((StrId)(S_RANK_0 + (value - 1))) : T(S_RANK_10);
    strncpy(buf, rn, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
  }
  const char *l2 = buf;

  gfx->fillRoundRect(73, 150, 320, 96, 16, UI_BAR_WARN);
  gfx->drawRoundRect(73, 150, 320, 96, 16, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(l1) * 9, 176);
  gfx->print(l1);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(l2) * 6, 212);
  gfx->print(l2);
}

// medallas en la ficha: badge con etiqueta, color si conseguida
void drawMedalBadge(int x, int y, int i) {
  bool got = pet.hasMedal(1 << i);
  gfx->fillRoundRect(x, y, 100, 24, 6, got ? UI_BAR_OK : UI_TRACK);
  if (!got) gfx->drawRoundRect(x, y, 100, 24, 6, UI_TRACK);
  gfx->setTextColor(got ? UI_BG_DAY : 0x9492);
  gfx->setTextSize(2);
  gfx->setCursor(x + (100 - (int)strlen(medalLabel(i)) * 12) / 2, y + 5);
  gfx->print(medalLabel(i));
}

// pagina 0: perfil (retrato grande, identidad, racha, vinculo, baya)
void renderCardProfile() {
  const DexEntry &d = DEX_TBL[pet.speciesId];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  char head[26];
  snprintf(head, sizeof(head), T(S_NAME_FMT), pet.shiny ? "*" : "", nm, pet.level());
  gfx->setTextColor(d.accent);
  // auto-encoge: a tamano 3 los nombres largos no caben en la franja estrecha de
  // arriba de la pantalla redonda, asi que se cortaban por el borde
  int hlen = umlautLen(head);
  int hts = (hlen <= 11) ? 3 : 2;
  gfx->setTextSize(hts);
  printUmlaut(head, CX - hlen * (hts == 3 ? 9 : 6), hts == 3 ? 34 : 40, hts, d.accent);
  if (pet.nick[0]) {  // especie real bajo el apodo
    const char *sp = dexName(pet.speciesId);
    gfx->setTextColor(UI_TRACK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - (strlen(sp) + 2) * 6, 64);
    gfx->printf("(%s)", sp);
  }

  // retrato grande animado
  if (pmd.loaded) drawPmdAct(PMD_IDLE, CX, 206, millis(), true, false, 4, false);

  // racha con llama: grupo (llama + texto) centrado como unidad
  char rl[30];
  snprintf(rl, sizeof(rl), T(S_STREAK_FMT), pet.streak, pet.bestStreak);
  int rlW = (int)strlen(rl) * 12;
  int streakGroupW = 24 + rlW;  // 15px llama + 9px hueco + texto
  int sx = CX - streakGroupW / 2, sy = 224;
  gfx->fillTriangle(sx + 8, sy, sx + 1, sy + 18, sx + 15, sy + 18, UI_BAR_BAD);
  gfx->fillTriangle(sx + 8, sy + 7, sx + 4, sy + 18, sx + 12, sy + 18, UI_BAR_WARN);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(sx + 24, sy + 2);
  gfx->print(rl);

  // vinculo: grupo (etiqueta + barra + numero) centrado como unidad -- no
  // usa drawCardStat() a proposito, para no mover ATK/DEF/SPE de su
  // posicion fija compartida
  {
    const char *lbl = T(S_VIN);
    char num[8];
    snprintf(num, sizeof(num), "%u", pet.bond);
    int lblW = (int)strlen(lbl) * 12, barW = 160, numW = (int)strlen(num) * 12;
    int totalW = lblW + 12 + barW + 20 + numW;
    int gx = CX - totalW / 2, gy = 258;
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(gx, gy);
    gfx->print(lbl);
    int barX = gx + lblW + 12;
    int fw = (int)pet.bond * barW / 100;
    if (fw > barW) fw = barW;
    gfx->fillRoundRect(barX, gy + 2, barW, 11, 3, UI_TRACK);
    if (fw > 2) gfx->fillRoundRect(barX, gy + 2, fw, 11, 3, C565(0xd4, 0x52, 0x7e));
    gfx->setCursor(barX + barW + 20, gy);
    gfx->print(num);
  }

  const char *berry = !pet.berryKnown ? T(S_BERRY_UNK)
                      : pet.lovesBerry(0) ? T(S_BERRY_RED)
                      : pet.lovesBerry(1) ? T(S_BERRY_BLUE)
                                          : T(S_BERRY_GREEN);
  char info[40];
  unsigned long ageDays = pet.ageMinutes / 1440;
  snprintf(info, sizeof(info), T(ageDays == 1 ? S_INFO_FMT_1 : S_INFO_FMT), berry, ageDays);
  printUmlaut(info, CX - umlautLen(info) * 6, 296, 2, UI_INK);
}

#define CARD_BTN_W 124    // Training / Kampf
#define CARD_PILL_W 54    // Bluetooth-Pille (Pokemon-Suche)
#define CARD_BTN_GAP 12
// Bluetooth-Zeichen fuer die Pille (Alpha 0..255, 14x26, aus der Vorlage des Nutzers)
#define BT_ICON_W 14
#define BT_ICON_H 26
static const uint8_t BT_ICON[BT_ICON_W * BT_ICON_H] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x0D, 0xF3, 0x9A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xE3, 0xFF, 0x9B, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xE5, 0xFD, 0xFF, 0xA0, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xE5, 0xFF, 0xFF, 0xFF, 0xA6, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xE6, 0xFD, 0xAE, 0xFC, 0xFF, 0xA9, 0x00, 0x00,
  0x26, 0xB2, 0x31, 0x00, 0x00, 0x0F, 0xE4, 0xFF, 0x2F, 0x68, 0xFF, 0xFF, 0xAB, 0x00,
  0xC9, 0xFF, 0xE4, 0x32, 0x00, 0x13, 0xE4, 0xFF, 0x44, 0x00, 0x75, 0xFC, 0xFF, 0xBC,
  0x32, 0xD8, 0xFF, 0xE7, 0x33, 0x00, 0xE6, 0xFF, 0x3C, 0x09, 0xB0, 0xFF, 0xF6, 0x67,
  0x00, 0x24, 0xDB, 0xFF, 0xE7, 0x46, 0xDB, 0xFF, 0x51, 0xAF, 0xFF, 0xFA, 0x50, 0x00,
  0x00, 0x00, 0x22, 0xD8, 0xFF, 0xF0, 0xF2, 0xFC, 0xE8, 0xFF, 0xF5, 0x4E, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x1F, 0xD7, 0xFF, 0xFE, 0xFE, 0xFF, 0xF8, 0x4D, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x19, 0xCE, 0xFD, 0xFC, 0xED, 0x41, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x17, 0xCD, 0xFE, 0xFC, 0xEC, 0x3D, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x1F, 0xD5, 0xFF, 0xFF, 0xFE, 0xFF, 0xF6, 0x49, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x22, 0xD8, 0xFF, 0xF1, 0xF3, 0xFC, 0xE9, 0xFF, 0xF5, 0x4E, 0x00, 0x00,
  0x00, 0x23, 0xDB, 0xFF, 0xEA, 0x49, 0xDA, 0xFE, 0x54, 0xB3, 0xFF, 0xF9, 0x50, 0x00,
  0x2F, 0xD6, 0xFF, 0xE9, 0x36, 0x00, 0xE6, 0xFF, 0x3D, 0x0A, 0xB4, 0xFF, 0xF6, 0x66,
  0xC8, 0xFF, 0xE6, 0x36, 0x00, 0x13, 0xE4, 0xFF, 0x44, 0x00, 0x73, 0xFC, 0xFF, 0xBD,
  0x29, 0xB7, 0x33, 0x00, 0x00, 0x0F, 0xE4, 0xFF, 0x2E, 0x64, 0xFF, 0xFF, 0xAC, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xE6, 0xFD, 0xAA, 0xFB, 0xFF, 0xAB, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xE5, 0xFF, 0xFF, 0xFF, 0xA8, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xE5, 0xFD, 0xFF, 0xA2, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xE3, 0xFF, 0x9E, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x0D, 0xF2, 0x9D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// pagina 1: combate (4 barras + boton de entrenar)
void renderCardStats() {
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_BATTLE)) * 9, 48);
  gfx->print(T(S_BATTLE));

  drawCardStat(118, T(S_STAT_ATK), pet.atkStat(), 260, UI_BAR_BAD);
  drawCardStat(160, T(S_STAT_DEF), pet.defStat(), 260, 0x4C98);
  drawCardStat(202, T(S_STAT_SPE), pet.speStat(), 260, UI_BAR_WARN);
  drawWeightStat(244);

  // Tastenzeile (seit 6.28 dreiteilig): Training | Bluetooth-Pille (Pokemon-
  // Suche) | Kampf. Je 124 px breit, Pille 54 px, Luecken 12 px, 40 px hoch.
  const int bw = CARD_BTN_W, pw = CARD_PILL_W, gp = CARD_BTN_GAP;
  const int x0 = CX - (bw * 2 + pw + gp * 2) / 2;  // 70
  gfx->fillRoundRect(x0, 300, bw, 40, 12, UI_BAR_BAD);
  gfx->setTextColor(UI_BG_DAY);
  gfx->setTextSize(2);
  gfx->setCursor(x0 + (bw - (int)strlen(T(S_TRAIN_STR)) * 12) / 2, 311);
  gfx->print(T(S_TRAIN_STR));
  int px = x0 + bw + gp;
  const uint16_t btBlue = C565(0x0a, 0x64, 0xb4);
  gfx->fillRoundRect(px, 300, pw, 40, 20, btBlue);
  int ix = px + (pw - BT_ICON_W) / 2, iy = 300 + (40 - BT_ICON_H) / 2;
  for (int yy = 0; yy < BT_ICON_H; yy++)
    for (int xx = 0; xx < BT_ICON_W; xx++) {
      uint8_t a = BT_ICON[yy * BT_ICON_W + xx];
      if (a) gfx->drawPixel(ix + xx, iy + yy, a > 247 ? UI_WHITE : lerp565(btBlue, UI_WHITE, a, 255));
    }
  int kx = px + pw + gp;
  gfx->fillRoundRect(kx, 300, bw, 40, 12, 0x4C98);
  gfx->setTextColor(UI_BG_DAY);
  gfx->setCursor(kx + (bw - (int)strlen(T(S_BATTLE)) * 12) / 2, 311);
  gfx->print(T(S_BATTLE));
}

// pagina 2: medallas con etiqueta descriptiva
void renderCardMedals() {
  int got = 0;
  for (int i = 0; i < MED_COUNT; i++)
    if (pet.hasMedal(1 << i)) got++;
  char head[20];
  snprintf(head, sizeof(head), T(S_MEDALS_FMT), got, MED_COUNT);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(head) * 9, 48);
  gfx->print(head);

  // reordenacion PURAMENTE visual (a peticion): la posicion en la rejilla ya
  // no coincide con el indice de bit. displayOrder[posicion] = bit real; la
  // logica de cada medalla (que bit es cual, propagacion, etc.) no cambia
  static const uint8_t displayOrder[MED_COUNT] = { 0, 1, 3, 5, 7, 2, 4, 6 };
  for (int pos = 0; pos < MED_COUNT; pos++) {
    int i = displayOrder[pos];
    int x = 32 + (pos % 2) * 206, y = 128 + (pos / 2) * 54;
    bool g = pet.hasMedal(1 << i);
    gfx->fillRoundRect(x, y, 196, 44, 10, g ? UI_BAR_OK : UI_TRACK);
    if (g) {  // marca de conseguida
      gfx->fillCircle(x + 22, y + 22, 11, UI_BG_DAY);
      gfx->setTextColor(UI_BAR_OK);
      gfx->setTextSize(2);
      gfx->setCursor(x + 16, y + 13);
      gfx->print("v");
    }
    gfx->setTextColor(g ? UI_BG_DAY : 0x8410);
    gfx->setTextSize(2);
    gfx->setCursor(x + 44, y + 14);
    const char *label = medalDesc(i);
    gfx->print(label);
    // "Endform"/"Final Form": tachado si esta especie TODAVIA no es forma
    // final (para dejar claro que no se puede conseguir en este momento)
    if ((1 << i) == MED_FINAL && !g && DEX_TBL[pet.speciesId].evolvesTo != 0) {
      int lw = (int)strlen(label) * 12;
      gfx->drawFastHLine(x + 44, y + 14 + 7, lw, 0x8410);
    }
    // "Toller Tag": tachado, solange el ciclo actual no esta limpio, O
    // solange TODAVIA no hubo ningun despertar (huevo recien nacido: ese
    // tiempo no cuenta para la medalla, se refleja tambien visualmente)
    if ((1 << i) == MED_STREAK7 && !g && (!pet.perfectDayClean || pet.perfectDayCycleStart == 0)) {
      int lw = (int)strlen(label) * 12;
      gfx->drawFastHLine(x + 44, y + 14 + 7, lw, 0x8410);
    }
  }
}

// pagina 3: progreso (nivel, evolucion, descuidos) — saca a la luz mecanicas
// que antes eran invisibles (cuanto falta para subir/evolucionar y por que)
void renderCardProgress() {
  const DexEntry &d = DEX_TBL[pet.speciesId];
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_PROGRESS)) * 9, 44);
  gfx->print(T(S_PROGRESS));

  // nivel grande
  char lv[10];
  snprintf(lv, sizeof(lv), T(S_LVL_FMT), pet.level());
  gfx->setTextSize(5);
  gfx->setCursor(CX - strlen(lv) * 15, 86);
  gfx->print(lv);

  // barra de progreso al siguiente nivel (duracion variable, ver level())
  uint32_t into, total;
  pet.levelProgress(into, total);
  int bx = 93, bw = 280, by = 158, bh = 22;
  gfx->fillRoundRect(bx, by, bw, bh, 6, UI_TRACK);
  int fw = (int)((bw - 4) * into / total);
  if (fw > 0) gfx->fillRoundRect(bx + 2, by + 2, fw, bh - 4, 5, UI_BAR_OK);
  char nx[26];
  snprintf(nx, sizeof(nx), T(S_NEXT_LVL_FMT), (unsigned)(total - into), pet.level() + 1);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(nx) * 6, by + 32);
  gfx->print(nx);

  // estado de evolucion
  gfx->setTextColor(UI_INK);
  gfx->setCursor(CX - strlen(T(S_EVO_LABEL)) * 6, 230);
  gfx->print(T(S_EVO_LABEL));
  char evoBuf[28];
  const char *evo = nullptr;
  uint16_t evoCol = UI_INK;
  bool evoIsButton = false;
  if (d.evolvesTo == 0) {
    evo = T(S_FINAL_FORM);
  } else {
    int needed = d.evolveLevel + pet.careMistakes;
    if (pet.level() >= needed) {
      if (pet.lowestStat() >= 40) { evoIsButton = true; }
      else { evo = T(S_EVO_BLOCKED); evoCol = UI_BAR_BAD; }
    } else {
      snprintf(evoBuf, sizeof(evoBuf), T(S_EVO_IN_FMT), needed - pet.level());
      evo = evoBuf;
    }
  }
  if (evoIsButton) {
    // boton real y tocable, con el mismo estilo que Training/Kampf -- para
    // que se note claramente que se puede pulsar. Un poco mas abajo que el
    // resto de textos, para no pegarse a la etiqueta "EVOLUTION" de arriba
    const char *btnLabel = T(S_EVO_READY);
    gfx->fillRoundRect(96, 254, 274, 40, 12, UI_BAR_OK);
    gfx->setTextColor(UI_BG_DAY);
    gfx->setTextSize(2);
    gfx->setCursor(CX - (int)strlen(btnLabel) * 6, 265);
    gfx->print(btnLabel);
  } else {
    gfx->setTextColor(evoCol);
    gfx->setCursor(CX - strlen(evo) * 6, 256);
    gfx->print(evo);
  }

  // descuidos (retrasan la evolucion)
  char ms[24];
  snprintf(ms, sizeof(ms), T(S_MISTAKES_FMT), pet.careMistakes);
  gfx->setTextColor(pet.careMistakes > 0 ? UI_BAR_BAD : UI_INK);
  gfx->setCursor(CX - strlen(ms) * 6, 312);
  gfx->print(ms);
}

// pagina 4: ausflug/trip -- contador de pasos, base para futuros encuentros
// eleccion de que rellena la baya magica: 2x2 botones
void renderBerryChoice() {
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_TRIP_EAT_BERRY)) * 9, 70);
  gfx->print(T(S_TRIP_EAT_BERRY));

  const char *labels[4] = { T(S_FULL_HUNGER), T(S_FULL_JOY), T(S_FULL_ENERGY), T(S_FULL_HYGIENE) };
  uint16_t cols[4] = { UI_BAR_WARN, C565(0xff, 0xb0, 0x3c), UI_BAR_OK, 0x4C98 };
  int bx[4] = { 83, 253, 83, 253 };
  int by[4] = { 166, 166, 266, 266 };   // seit 6.58 16 px tiefer: Abstand Titel->Buttons = Buttons->Zurueck (je 72 px; vorher 56/88)
  gfx->setTextSize(2);
  for (int i = 0; i < 4; i++) {
    gfx->fillRoundRect(bx[i], by[i], 130, 60, 12, cols[i]);
    gfx->setTextColor(UI_BG_DAY);
    gfx->setCursor(bx[i] + (130 - (int)strlen(labels[i]) * 12) / 2, by[i] + 22);
    gfx->print(labels[i]);
  }
}

// aviso tras comer una baya magica: se cierra solo tras un rato
// separa "msg" en hasta maxLines lineas de como mucho maxChars caracteres,
// cortando por palabras completas (para que quepa en el circulo sin partir
// palabras a la mitad)
int wordWrap(const char *msg, char lines[][32], int maxLines, int maxChars) {
  int n = 0;
  const char *p = msg;
  while (*p && n < maxLines) {
    int len = 0;
    const char *lastSpace = nullptr;
    const char *q = p;
    while (*q && len < maxChars) {
      if (*q == ' ') lastSpace = q;
      q++;
      len++;
    }
    if (*q == 0) {  // cabe el resto entero en esta linea
      strncpy(lines[n], p, 31);
      lines[n][31] = 0;
      n++;
      break;
    }
    const char *cut = lastSpace ? lastSpace : q;
    int l = (int)(cut - p);
    if (l > 31) l = 31;
    strncpy(lines[n], p, l);
    lines[n][l] = 0;
    n++;
    p = lastSpace ? lastSpace + 1 : q;
  }
  return n;
}

void renderBerryResult() {
  if (millis() >= berryResultUntil) { berryResultOpen = false; return; }
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  printUmlaut(T(S_YUMMY), CX - umlautLen(T(S_YUMMY)) * 9, 130, 3, UI_INK);

  static const StrId msgs[4] = { S_BERRY_MSG_HUNGER, S_BERRY_MSG_JOY, S_BERRY_MSG_ENERGY, S_BERRY_MSG_HYGIENE };
  const char *msg = T(msgs[berryResultWhich]);
  char lines[3][32];
  int n = wordWrap(msg, lines, 3, 20);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  int startY = 233 - (n - 1) * 17 - 12;
  for (int i = 0; i < n; i++) {
    printUmlaut(lines[i], CX - umlautLen(lines[i]) * 9, startY + i * 34, 3, UI_INK);
  }

  // el corazon dura mientras se ve esta pantalla entera (3s), no solo el
  // HEART_MS corto de costumbre -- de lo contrario nunca llegaria a verse
  drawMap(SPR_HEART, 32, CX - 32, 320, 2, false);
}

void renderCardTrip() {
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(4);
  gfx->setCursor(CX - strlen(T(S_TRIP)) * 12, 50);
  gfx->print(T(S_TRIP));

  // "hoy" se ve al dia si ya paso la medianoche desde el ultimo ausflug
  // guardado, aunque no se haya vuelto a llamar a addSteps() todavia
  uint32_t todaySteps = (pet.lastSeenEpoch / 86400 == pet.stepsDay) ? pet.stepsToday : 0;

  char today[32], tot[32], berryLabel[24], berryNum[6], todayNum[16], totNum[16];
  formatThousands(todayNum, sizeof(todayNum), todaySteps);
  formatThousands(totNum, sizeof(totNum), pet.totalSteps);
  snprintf(today, sizeof(today), "%s %s", T(S_TRIP_TODAY), todayNum);
  snprintf(tot, sizeof(tot), "%s %s", T(S_TRIP_TOTAL), totNum);
  snprintf(berryLabel, sizeof(berryLabel), "%s ", T(S_TRIP_BERRY_COUNT));
  snprintf(berryNum, sizeof(berryNum), "%u", pet.magicBerries);
  // las tres lineas, espaciado uniforme, justo encima del boton de la baya
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - (int)strlen(today) * 9, 100);
  gfx->print(today);
  gfx->setCursor(CX - (int)strlen(tot) * 9, 130);
  gfx->print(tot);
  int berryChars = (int)strlen(berryLabel) + (int)strlen(berryNum);
  int berryX = CX - berryChars * 9;  // centrado (9 = mitad del ancho real de 18px/caracter en tamano 3)
  gfx->setTextColor(UI_INK);
  gfx->setCursor(berryX, 160);
  gfx->print(berryLabel);
  gfx->setTextColor(pet.magicBerries >= MAGIC_BERRY_MAX ? UI_BAR_BAD : UI_INK);
  gfx->setCursor(berryX + (int)strlen(berryLabel) * 18, 160);  // avance real, no la mitad
  gfx->print(berryNum);
  gfx->setTextColor(UI_INK);

  if (!imuOk()) {
    gfx->setTextColor(UI_BAR_BAD);
    gfx->setTextSize(2);
    gfx->setCursor(CX - (int)strlen(T(S_TRIP_NO_SENSOR)) * 6, 188);
    gfx->print(T(S_TRIP_NO_SENSOR));
  }

  // comer una baya magica: justo debajo de las tres lineas de arriba
  bool hasBerry = pet.magicBerries > 0;
  gfx->fillRoundRect(96, 200, 274, 40, 12, hasBerry ? UI_BAR_OK : UI_TRACK);
  gfx->setTextColor(UI_BG_DAY);
  {
    const char *s = T(S_TRIP_EAT_BERRY);
    bool fits = (int)strlen(s) * 18 <= 260;
    gfx->setTextSize(fits ? 3 : 2);
    int hw = (int)strlen(s) * (fits ? 9 : 6);
    gfx->setCursor(CX - hw, fits ? 211 : 214);
    gfx->print(s);
  }

  // boton de inicio: el elemento principal de esta pagina, el doble de alto
  // que el resto, con mas separacion respecto al area de "volver" de abajo
  gfx->fillRoundRect(96, 250, 274, 80, 16, 0x4C98);
  gfx->setTextColor(UI_BG_DAY);
  {
    const char *s = T(S_TRIP_GO);
    bool fits = (int)strlen(s) * 18 <= 260;
    gfx->setTextSize(fits ? 3 : 2);
    int hw = (int)strlen(s) * (fits ? 9 : 6);
    gfx->setCursor(CX - hw, fits ? 278 : 282);  // centrado vertical real (caja: 250-330)
    gfx->print(s);
  }
}

// ---------- ausflug/trip: modo contador de pasos ----------

// mismas sperren que startTrip(), pero en vez de arrancar directo abre la
// eleccion "junto / solo"
void openTripChoice() {
  if (pet.isEgg() || pet.napping() || pet.blocked()) return;
  if (choiceKind) {
    sleepMsgId = S_NOT_YET; sleepMsgUntil = millis() + 2500;
    sfxPlay(SFX_DENY);
    return;
  }
  if (pet.sleeping) {
    sleepMsgId = S_IS_SLEEPING; sleepMsgUntil = millis() + 2500;
    sfxPlay(SFX_DENY);
    return;
  }
  int h = sceneHour();
  if (h >= 20) {
    sleepMsgId = S_TOO_LATE; sleepMsgUntil = millis() + 2500;
    sfxPlay(SFX_DENY);
    return;
  }
  if (h < 6) {
    sleepMsgId = S_NOT_YET; sleepMsgUntil = millis() + 2500;
    sfxPlay(SFX_DENY);
    return;
  }
  tripChoiceOpen = true;
}

// pantalla dividida "junto / solo": mitad de arriba = ausflug de siempre,
// mitad de abajo = nuevo "Pokemon-Ausflug" (mandar al bicho solo)
// aviso tras detectar en el arranque que el aparato se apago de golpe
// durante un ausflug de pasos; se cierra solo con un toque en cualquier sitio
void renderTripWarning() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  printLinesCentered(T(S_TRIP_POWEROFF_WARN), 140, 40, 3, UI_INK, 4);
  gfx->flush();
}

void renderTripChoice() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->drawFastHLine(CX - 200, CY, 400, RGB565_BLACK);
  gfx->drawFastHLine(CX - 200, CY + 1, 400, RGB565_BLACK);
  gfx->setTextColor(UI_INK);
  printLinesCentered(T(S_TRIP_TOGETHER), 100, 34, 3, UI_INK);
  printLinesCentered(T(S_TRIP_ALONE), 268, 34, 3, UI_INK);
  gfx->flush();
}

void tripChoiceTap(int16_t x, int16_t y) {
  if (y < CY) { tripChoiceOpen = false; startTrip(); }
  else { tripChoiceOpen = false; startTripSolo(); }
}

void startTrip() {
  if (pet.isEgg() || pet.napping() || pet.blocked()) return;
  if (choiceKind) {
    sleepMsgId = S_NOT_YET; sleepMsgUntil = millis() + 2500;
    sfxPlay(SFX_DENY);
    return;
  }
  if (pet.sleeping) {
    sleepMsgId = S_IS_SLEEPING; sleepMsgUntil = millis() + 2500;
    sfxPlay(SFX_DENY);
    return;
  }
  int h = sceneHour();
  if (h >= 20) {  // nach 20 Uhr: bald Schlafenszeit, kein Ausflug mehr
    sleepMsgId = S_TOO_LATE; sleepMsgUntil = millis() + 2500;
    sfxPlay(SFX_DENY);
    return;
  }
  if (h < 6) {
    sleepMsgId = S_NOT_YET; sleepMsgUntil = millis() + 2500;
    sfxPlay(SFX_DENY);
    return;
  }
  tripOpen = true;
  pet.tripActive = true;
  pet.tripActiveFlag = true;
  pet.requestSave();
  pet.stepsThisInterval = false;
  tripSteps = 0;
  tripNextCheckpoint = 1000;
  tripEncCount = 0;
  tripBerryLost = false;
  tripBerriesGained = 0;
  tripEndConfirm = false;
  imuEnable();
}

// "Pokemon-Ausflug": el bicho se va solo, sin sensor de pasos, con un
// temporizador de 60-90 min elegido al azar. Maximo 5 veces por dia natural
void startTripSolo() {
  uint32_t td = pet.lastSeenEpoch ? pet.lastSeenEpoch / 86400 : 0;
  if (td != pet.tripSoloDay) { pet.tripSoloDay = td; pet.tripSoloCount = 0; }
  if (pet.tripSoloCount >= 5) {
    sleepMsgId = S_SOLO_LIMIT; sleepMsgUntil = millis() + 2500;
    sfxPlay(SFX_DENY);
    return;
  }
  pet.tripSoloCount++;
  uint32_t liveNow = rtcEpoch();
  if (!liveNow) liveNow = pet.lastSeenEpoch;
  pet.tripSoloEndEpoch = liveNow + (uint32_t)(30 + random(31)) * 60;  // 30-60 min al azar
  pet.requestSave();
}

// pantalla del "Pokemon-Ausflug": mismo fondo que el ausflug normal, pero
// con cuenta atras en vez de pasos, y sin sensor de pasos activo
void renderTripSolo() {
  if (tripSoloConfirm && millis() > tripSoloConfirmUntil) tripSoloConfirm = false;
  if (LOOK_B) {  // Optik B: Landschaft des Pokemon, Sonne/Mond tiefer und rechts
    gSunDX = TRIP_SUN_DX; gSunDY = TRIP_SUN_DY; gSunUp = TRIP_SOLO_SUN_UP;
    drawTripSceneB(pet.isEgg() ? 0 : biomeFor(pet.speciesId));
    gSunDX = gSunDY = 0; gSunUp = 0;
  } else drawGameScene();
  if (batPercent() >= 0 && batPercent() < 40 && !batCharging()) drawBattery();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;

  // igual que en el ausflug normal: arriba se ve la hora actual, sin titulo
  uint32_t liveNow = rtcEpoch();
  if (!liveNow) liveNow = pet.lastSeenEpoch;
  int32_t secOfDay = (int32_t)(((liveNow % 86400) + 86400) % 86400);
  char clk[8];
  snprintf(clk, sizeof(clk), "%02d:%02d", (int)(secOfDay / 3600), (int)((secOfDay / 60) % 60));
  gfx->setTextColor(ink);
  gfx->setTextSize(7);
  gfx->setCursor(CX - 105, 55);
  gfx->print(clk);

  // cuenta atras en el sitio de los pasos del ausflug normal; en base al
  // reloj REAL (no lastSeenEpoch, que solo se actualiza una vez por
  // minuto) para que baje segundo a segundo de verdad. Por encima de 60s
  // se muestran minutos, a partir de 60s (inclusive) segundos -- con
  // singular/plural correcto en ambos casos
  uint32_t remain = (pet.tripSoloEndEpoch > liveNow) ? (pet.tripSoloEndEpoch - liveNow) : 0;
  char cd[24];
  if (remain == 0) {
    strncpy(cd, T(S_TRIP_SOLO_END), sizeof(cd) - 1);
    cd[sizeof(cd) - 1] = 0;
  } else {
    bool secondsMode = remain <= 60;
    const char *lbl = T(secondsMode ? S_BACK_SOON : S_TIME_UNTIL_RETURN);
    gfx->setTextColor(ink);
    gfx->setTextSize(2);
    printUmlaut(lbl, CX - umlautLen(lbl) * 6, 155, 2, ink);
    if (secondsMode) {
      snprintf(cd, sizeof(cd), T(remain == 1 ? S_SECOND_FMT : S_SECONDS_FMT), (int)remain);
    } else {
      unsigned mins = remain / 60;
      snprintf(cd, sizeof(cd), T(mins == 1 ? S_MINUTE_FMT : S_MINUTES_FMT), (int)mins);
    }
  }
  gfx->setTextColor(ink);
  gfx->setTextSize(6);
  gfx->setCursor(CX - (int)strlen(cd) * 18, 190);
  gfx->print(cd);

  gfx->fillRoundRect(83, 305, 300, 85, 16, UI_BAR_BAD);
  gfx->setTextColor(UI_BG_DAY);
  if (tripSoloConfirm) {
    gfx->setTextSize(3);
    const char *r = T(S_TRIP_REALLY);
    gfx->setCursor(CX - (int)strlen(r) * 9, 335);
    gfx->print(r);
  } else {
    StrId lblId = remain == 0 ? S_POKEMON_BACK : S_CALL_BACK;
    printLinesCentered(T(lblId), 323, 30, 3, UI_BG_DAY);
  }

  gfx->flush();
}

void tripSoloTap(int16_t x, int16_t y) {
  if (!(x >= 83 && x <= 383 && y >= 305 && y <= 390)) return;
  uint32_t liveNow = rtcEpoch();
  if (!liveNow) liveNow = pet.lastSeenEpoch;
  uint32_t remain = (pet.tripSoloEndEpoch > liveNow) ? (pet.tripSoloEndEpoch - liveNow) : 0;
  sfxPlay(SFX_HEART);
  if (remain == 0) {  // ya termino el timer: se recoge directo, sin preguntar
    tripSoloConfirm = false;
    resolveTripSolo();
    return;
  }
  // timer aun corriendo: hace falta confirmar (evita recuperar sin querer)
  if (tripSoloConfirm) {
    tripSoloConfirm = false;
    pet.joy = (pet.joy > 30) ? pet.joy - 30 : 0;
    pet.loseBond(5);
    pet.tripSoloEndEpoch = 0;
    pet.requestSave();
    sfxPlay(SFX_BATTLE_LOSE);
    tripSoloSadOpen = true;
    tripSoloSadUntil = millis() + 3000;
  } else {
    tripSoloConfirm = true;
    tripSoloConfirmUntil = millis() + 3000;
  }
}


// se llama al recuperar el Pokemon DESPUES de que el temporizador ya
// termino: sortea uno de los 10 posibles eventos (10% cada uno) y aplica
// sus efectos
void resolveTripSolo() {
  int ev = random(10);  // 0-9
  int16_t sp = 0;
  bool wasNew = false;
  tripSoloResultBerryLost = false;
  switch (ev) {
    case 0:  // "nettes Pokemon / ein Geschenk / zwei Zauberbeeren"
      sp = 1 + random(DEX_COUNT);
      pet.joy = (pet.joy + 20 > 100) ? 100 : pet.joy + 20;
      for (int i = 0; i < 2; i++) {
        if (pet.magicBerries < MAGIC_BERRY_MAX) pet.magicBerries++;
        else tripSoloResultBerryLost = true;
      }
      pet.everSoloEvent1 = true;
      break;
    case 1:  // "tolle Rauferei / freundliche Worte / so viel Spass"
      sp = 1 + random(DEX_COUNT);
      pet.joy = (pet.joy + 30 > 100) ? 100 : pet.joy + 30;
      pet.hygiene = (pet.hygiene > 30) ? pet.hygiene - 30 : 0;
      break;
    case 2:  // "ein Picknick / ein voller Magen / Zufriedenheit"
      sp = 1 + random(DEX_COUNT);
      pet.fullness = (pet.fullness + 20 > 100) ? 100 : pet.fullness + 20;
      pet.joy = (pet.joy + 20 > 100) ? 100 : pet.joy + 20;
      pet.energy = (pet.energy + 10 > 100) ? 100 : pet.energy + 10;
      pet.weight = (pet.weight + 30 > 100) ? 100 : pet.weight + 30;
      break;
    case 3:  // "gekaempft / verloren / deprimiert"
      sp = 1 + random(DEX_COUNT);
      pet.joy = (pet.joy > 40) ? pet.joy - 40 : 0;
      pet.hygiene = (pet.hygiene > 30) ? pet.hygiene - 30 : 0;
      break;
    case 4:  // "weit gelaufen / viel entdeckt / voll zufrieden"
      pet.fullness = (pet.fullness > 40) ? pet.fullness - 40 : 0;
      pet.joy = (pet.joy + 30 > 100) ? 100 : pet.joy + 30;
      pet.energy = (pet.energy > 40) ? pet.energy - 40 : 0;
      pet.weight = (pet.weight > 20) ? pet.weight - 20 : 0;
      pet.everSoloEvent5 = true;
      break;
    case 5:  // "Busch mit Beeren / volle Backen / dicker Bauch"
      pet.fullness = (pet.fullness + 30 > 100) ? 100 : pet.fullness + 30;
      pet.joy = (pet.joy + 30 > 100) ? 100 : pet.joy + 30;
      pet.energy = (pet.energy + 20 > 100) ? 100 : pet.energy + 20;
      pet.weight = (pet.weight + 30 > 100) ? 100 : pet.weight + 30;
      break;
    case 6:  // "verlaufen / herumgeirrt / voellig erschoepft"
      pet.fullness = (pet.fullness > 10) ? pet.fullness - 10 : 0;
      pet.joy = (pet.joy > 30) ? pet.joy - 30 : 0;
      pet.energy = (pet.energy > 40) ? pet.energy - 40 : 0;
      pet.hygiene = (pet.hygiene > 40) ? pet.hygiene - 40 : 0;
      pet.weight = (pet.weight > 20) ? pet.weight - 20 : 0;
      break;
    case 7:  // "Schlammbad / matsch matsch / so spassig"
      pet.joy = (pet.joy + 30 > 100) ? 100 : pet.joy + 30;
      pet.hygiene = (pet.hygiene > 40) ? pet.hygiene - 40 : 0;
      break;
    case 8:  // "ausgiebig gechillt / total entspannt / voll ausgeruht"
      pet.joy = 100;
      pet.energy = 100;
      break;
    case 9:  // "verletzt / frustriert / ungluecklich"
    default:
      pet.joy = 30;
      break;
  }
  if (sp > 0) {
    wasNew = !pet.isRegistered(sp);
    if (wasNew) pet.registerSeen(sp);
    if (pet.dexEncounters[sp] < 65535) pet.dexEncounters[sp]++;
  }
  pet.tripSoloEndEpoch = 0;
  pet.requestSave();

  tripSoloResultEvent = (uint8_t)ev;
  tripSoloResultSpecies = sp;
  tripSoloResultNew = wasNew;
  tripSoloResultStage = (sp > 0) ? 0 : 1;  // sin encuentro, se salta el "You met X!"
  tripSoloResultOpen = true;
}

// aviso "triste" al recuperar el Pokemon antes de tiempo: se cierra solo
void renderTripSoloSad() {
  if (millis() >= tripSoloSadUntil) { tripSoloSadOpen = false; return; }
  if (LOOK_B) {  // Optik B: Landschaft des Pokemon, Sonne/Mond tiefer und rechts
    gSunDX = TRIP_SUN_DX; gSunDY = TRIP_SUN_DY;
    drawTripSceneB(pet.isEgg() ? 0 : biomeFor(pet.speciesId));
    gSunDX = gSunDY = 0;
  } else drawGameScene();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  if (LOOK_B) printLinesCentered(T(S_SOLO_SAD), 187, 34, 3, ink);
  else printLinesCentered(T(S_SOLO_SAD), 205, 40, 3, ink);
  gfx->flush();
}

// resultado del "Pokemon-Ausflug" tras recuperarlo a tiempo: 0="You met X!"
// + NEU (solo si hubo encuentro), 1=texto del evento en 3 lineas, 2=baya
// perdida (solo si aplica). Un toque avanza de etapa; al final vuelve sola
void renderTripSoloResult() {
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  int16_t sp = tripSoloResultSpecies;

  if (tripSoloResultStage == 0 && sp > 0) {
    const DexEntry &d = DEX_TBL[sp];
    if (LOOK_B) drawTripSceneB(biomeFor(sp));  // Optik B: Landschaft 120 px tiefer, Text auf dem Himmel
    else drawScene(biomeFor(sp), millis(), night);
    if (!tripPmd.loaded) tripPmd.load(sp, false);
    if (tripPmd.loaded) {
      if (LOOK_B) drawPmdActM(tripPmd, PMD_IDLE, CX, TRIP_PET_Y, millis(), true, false, 3, false);
      else drawPmdActM(tripPmd, PMD_IDLE, CX, 266, millis(), true, false, 4, false);
    }
    char msg[40];
    snprintf(msg, sizeof(msg), T(S_TRIP_MET_FMT), dexName(sp));
    gfx->setTextColor(LOOK_B ? ink : UI_INK);
    gfx->setTextSize(3);
    // Optik B: Textblock (1-2 Zeilen) bleibt mittig, rutscht bei weniger Zeilen nach unten
    int ty = LOOK_B ? 134 + (tripSoloResultNew ? 17 : 34) : 280;
    gfx->setCursor(CX - (int)strlen(msg) * 9, ty);
    gfx->print(msg);
    if (tripSoloResultNew) {
      gfx->setCursor(CX - (int)strlen(T(S_TRIP_NEW)) * 9, LOOK_B ? ty + 34 : 314);
      gfx->print(T(S_TRIP_NEW));
    }
  } else if (tripSoloResultStage == 1) {
    tripPmd.unload();
    if (LOOK_B) drawTripSceneB(pet.isEgg() ? 0 : biomeFor(pet.speciesId));
    else drawGameScene();
    static const StrId evtIds[10] = { S_SOLO_EVT1, S_SOLO_EVT2, S_SOLO_EVT3, S_SOLO_EVT4, S_SOLO_EVT5,
                                       S_SOLO_EVT6, S_SOLO_EVT7, S_SOLO_EVT8, S_SOLO_EVT9, S_SOLO_EVT10 };
    // Optik B: Text auf dem Himmel (mit Bonuszeile weiter oben)
    printLinesCentered(T(evtIds[tripSoloResultEvent]), LOOK_B ? (tripSoloResultEvent == 0 ? 110 : 145) : 190, 34, 3, ink);
    if (tripSoloResultEvent == 0) {
      const char *bonus = T(S_SOLO_EVT1_BONUS);
      gfx->setTextColor(ink);
      gfx->setTextSize(3);
      gfx->setCursor(CX - (int)strlen(bonus) * 9, LOOK_B ? 254 : 334);
      gfx->print(bonus);
    }
  } else if (tripSoloResultStage == 2) {
    if (LOOK_B) drawTripSceneB(pet.isEgg() ? 0 : biomeFor(pet.speciesId));
    else drawGameScene();
    const char *l1 = T(S_BERRY_LOST1), *l2 = T(S_BERRY_LOST2);
    gfx->setTextColor(ink);
    gfx->setTextSize(3);
    gfx->setCursor(CX - (int)strlen(l1) * 9, LOOK_B ? 187 : 220);
    gfx->print(l1);
    gfx->setCursor(CX - (int)strlen(l2) * 9, LOOK_B ? 221 : 254);
    gfx->print(l2);
  }

  // Landschaft dieser Seite: Seite 1 zeigt das getroffene Pokemon, sonst das eigene
  uint8_t sceneBio = (tripSoloResultStage == 0 && sp > 0) ? biomeFor(sp) : (pet.isEgg() ? 0 : biomeFor(pet.speciesId));
  gfx->setTextColor(groundTextCol(sceneBio, ink));
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(T(S_TRIP_NEXT)) * 6, LOOK_B ? TRIP_WEITER_Y : 390);
  gfx->print(T(S_TRIP_NEXT));

  gfx->flush();
}

void tripSoloResultTap() {
  if (tripSoloResultStage == 0) {
    tripPmd.unload();
    tripSoloResultStage = 1;
  } else if (tripSoloResultStage == 1) {
    if (tripSoloResultBerryLost) tripSoloResultStage = 2;
    else tripSoloResultOpen = false;
  } else {
    tripSoloResultOpen = false;
  }
}

// del dex al instante (no hace falta esperar al fin del ausflug)
// crea un encuentro (especie + baya + registro); se llama tras ya haber
// decidido que SI hay encuentro, tanto en checkpoints completos como en
// el resto proporcional al final del ausflug
static void tripRollEncounter(int checkpointNum) {
  if (tripEncCount >= TRIP_MAX_ENC) return;
  int16_t sp = 1 + random(DEX_COUNT);
  // la chance de baya sube con la duracion del ausflug EN CURSO: 40% en el
  // primer checkpoint, +10% por cada uno mas, tope en 100% (a partir del 7.)
  int berryChance = 40 + 10 * (checkpointNum - 1);
  if (berryChance > 100) berryChance = 100;
  bool berry = random(100) < berryChance;
  bool wasNew = !pet.isRegistered(sp);
  if (wasNew) pet.registerSeen(sp);  // se queda aufgedeckt para siempre
  tripEncSpecies[tripEncCount] = sp;
  tripEncBerry[tripEncCount] = berry;
  tripEncNew[tripEncCount] = wasNew;
  tripEncCount++;
  if (pet.dexEncounters[sp] < 65535) pet.dexEncounters[sp]++;
  if (berry) {
    tripBerriesGained++;  // cuenta para "Rendezvous" aunque se pierda por bolsas llenas
    if (pet.magicBerries < MAGIC_BERRY_MAX) pet.magicBerries++;
    else tripBerryLost = true;  // las bolsas ya estaban llenas: se pierde
  }
}

void tripCheckEncounters() {
  while (tripSteps >= tripNextCheckpoint && tripNextCheckpoint <= 1000000UL) {
    tripNextCheckpoint += 1000;
    pet.addTripBond();  // +1 vinculo cada 1000 pasos, aunque no haya encuentro
    if (random(100) < 50) tripRollEncounter((int)(tripNextCheckpoint / 1000) - 1);
  }
}

// fin del ausflug (boton cancelar o medianoche/hora de dormir forzada):
// guarda los pasos y, si hubo encuentros, abre la pantalla de resultados
// carga (o descarga) el sprite del encuentro actualmente mostrado
void loadTripResultSprite() {
  tripPmd.unload();
  if (tripResultIndex < tripEncCount) {
    tripPmd.load(tripEncSpecies[tripResultIndex], false);  // sin shiny en encuentros salvajes
    // fanfarria si es nueva especie y/o trajo una baya -- una sola vez, aunque
    // se cumplan las dos cosas a la vez
    if (tripEncNew[tripResultIndex] || tripEncBerry[tripResultIndex]) sfxPlay(SFX_BATTLE_WIN);
  } else if (tripEncCount == 0) {
    sfxPlay(SFX_BATTLE_LOSE);  // "niemanden getroffen": el mismo tono triste que perder un combate
  }
}

void finishTrip() {
  tripOpen = false;
  pet.tripActive = false;
  pet.tripActiveFlag = false;
  imuDisable();
  // resto proporcional: el tramo de 1000 pasos ya empezado pero no
  // completado tambien tiene su chance, escalada linealmente (p.ej. 500
  // pasos = 25%, 750 pasos = 37.5% redondeado hacia arriba a 38%)
  uint32_t partial = tripSteps - (tripNextCheckpoint - 1000);
  if (partial > 0 && partial < 1000 && tripEncCount < TRIP_MAX_ENC) {
    int chance = (int)((partial + 19) / 20);  // partial/20 redondeado hacia arriba
    if (random(100) < chance) tripRollEncounter((int)(tripNextCheckpoint / 1000));
  }
  if (tripSteps >= 3000) pet.bigTripDone = true;
  if (tripEncCount == 0) { pet.joy = (pet.joy > 30) ? pet.joy - 30 : 0; pet.loseBond(5); }  // niemanden getroffen: enttaeuscht
  tripResultUntil = millis() + 3000;  // solo relevante si tripEncCount==0: se cierra sola
  if (tripBerriesGained >= 2) pet.everGot2BerriesOneTrip = true;  // "Rendezvous"
  pet.addSteps(tripSteps);
  pet.requestSave();  // stellt sicher, dass tripActiveFlag=false IMMER
                        // persistiert wird, auch wenn addSteps() bei exakt
                        // 0 Schritten selbst nicht speichert
  tripResultOpen = true;
  tripResultIsMeet = false;  // echter Ausflug: normales Tap-zum-Weiter-Verhalten
  tripResultIndex = 0;
  loadTripResultSprite();
}

void renderTrip() {
  if (tripEndConfirm && millis() > tripEndConfirmUntil) tripEndConfirm = false;  // laeuft kontinuierlich, auch ohne neuen Touch

  if (LOOK_B) {  // Optik B: Landschaft des Pokemon (120 px tiefer), Sonne/Mond tiefer und rechts (Uhrzeit)
    gSunDX = TRIP_SUN_DX; gSunDY = TRIP_SUN_DY;
    drawTripSceneB(pet.isEgg() ? 0 : biomeFor(pet.speciesId));
    gSunDX = gSunDY = 0;
  } else drawGameScene();  // mismo fondo con la hora del dia que el minijuego/saco
  // bateria: solo se ve aqui en amarillo/rojo (aviso durante un ausflug
  // largo), no en verde ni cargando -- eso ya se ve en la pantalla principal
  if (batPercent() >= 0 && batPercent() < 40 && !batCharging()) drawBattery();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;

  // durante el ausflug, arriba se ve la hora actual en vez de "TRIP"
  uint32_t liveNow = rtcEpoch();
  if (!liveNow) liveNow = pet.lastSeenEpoch;
  int32_t secOfDay = (int32_t)(((liveNow % 86400) + 86400) % 86400);
  char clk[8];
  snprintf(clk, sizeof(clk), "%02d:%02d", (int)(secOfDay / 3600), (int)((secOfDay / 60) % 60));
  gfx->setTextColor(ink);
  gfx->setTextSize(7);
  gfx->setCursor(CX - 105, 55);
  gfx->print(clk);

  char steps[16];
  snprintf(steps, sizeof(steps), "%lu", (unsigned long)tripSteps);
  gfx->setTextSize(6);
  gfx->setCursor(CX - (int)strlen(steps) * 18, 190);
  gfx->print(steps);

  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(T(S_TRIP_STEPS)) * 6, 270);
  gfx->print(T(S_TRIP_STEPS));

  if (!imuOk()) {
    gfx->setTextColor(UI_BAR_BAD);
    gfx->setCursor(CX - (int)strlen(T(S_TRIP_NO_SENSOR)) * 6, 300);
    gfx->print(T(S_TRIP_NO_SENSOR));
  }

  gfx->fillRoundRect(83, 330, 300, 75, 16, UI_BAR_BAD);
  gfx->setTextColor(UI_BG_DAY);
  gfx->setTextSize(3);
  const char *endLabel = tripEndConfirm ? T(S_TRIP_REALLY) : T(S_TRIP_CANCEL);
  gfx->setCursor(CX - (int)strlen(endLabel) * 9, 358);
  gfx->print(endLabel);

  gfx->flush();
}

// resultados del ausflug: uno por pantalla, fondo segun el tipo del
// pokemon encontrado, se avanza tocando en cualquier sitio ("weiter")
void renderTripResult() {
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  if (tripEncCount == 0 && millis() >= tripResultUntil) { tripResultOpen = false; return; }
  if (tripResultIndex == tripEncCount && tripBerryLost) {
    // pagina extra al final: baya perdida (bolsas llenas). Mismo fondo que
    // el resto, solo cambia el texto
    uint8_t bioP = pet.isEgg() ? 0 : biomeFor(pet.speciesId);
    if (LOOK_B) drawTripSceneB(bioP);  // Optik B: Landschaft 120 px tiefer, Text auf dem Himmel
    else drawScene(bioP, millis(), night);
    uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
    gfx->setTextColor(ink);
    gfx->setTextSize(3);
    const char *l1 = T(S_BERRY_LOST1), *l2 = T(S_BERRY_LOST2);
    gfx->setCursor(CX - (int)strlen(l1) * 9, LOOK_B ? 187 : 280);
    gfx->print(l1);
    gfx->setCursor(CX - (int)strlen(l2) * 9, LOOK_B ? 221 : 314);
    gfx->print(l2);
  } else if (tripEncCount == 0) {
    if (LOOK_B) drawTripSceneB(pet.isEgg() ? 0 : biomeFor(pet.speciesId));
    else drawGameScene();
    gfx->setTextColor(night ? UI_INK_NIGHT : UI_INK);
    gfx->setTextSize(3);
    const char *msg = T(S_TRIP_NOBODY);
    gfx->setCursor(CX - strlen(msg) * 9, LOOK_B ? 204 : CY);
    gfx->print(msg);
  } else {
    int16_t sp = tripEncSpecies[tripResultIndex];
    const DexEntry &d = DEX_TBL[sp];
    if (LOOK_B) drawTripSceneB(biomeFor(sp));  // Optik B: Landschaft des Pokemon, 120 px tiefer
    else drawScene(biomeFor(sp), millis(), night);
    uint16_t ink = night ? UI_INK_NIGHT : UI_INK;

    char counter[12];
    snprintf(counter, sizeof(counter), "%u/%u", tripResultIndex + 1, tripEncCount);
    gfx->setTextColor(ink);
    gfx->setTextSize(2);
    gfx->setCursor(CX - (int)strlen(counter) * 6, 34);
    gfx->print(counter);

    if (tripPmd.loaded) {
      if (LOOK_B) drawPmdActM(tripPmd, PMD_IDLE, CX, TRIP_PET_Y, millis(), true, false, 3, false);
      else drawPmdActM(tripPmd, PMD_IDLE, CX, 266, millis(), true, false, 4, false);
    }

    char msg[40];
    snprintf(msg, sizeof(msg), T(S_TRIP_MET_FMT), dexName(sp));
    gfx->setTextColor(LOOK_B ? ink : UI_INK);
    gfx->setTextSize(3);
    // Optik B: Textblock (1-3 Zeilen) bleibt mittig, rutscht bei weniger Zeilen nach unten
    int nLines = 1 + (tripEncNew[tripResultIndex] ? 1 : 0) + (tripEncBerry[tripResultIndex] ? 1 : 0);
    int ty = LOOK_B ? 134 + (3 - nLines) * 17 : 280;
    gfx->setCursor(CX - (int)strlen(msg) * 9, ty);
    gfx->print(msg);

    int ny = LOOK_B ? ty + 34 : 314;
    if (tripEncNew[tripResultIndex]) {
      gfx->setTextColor(LOOK_B ? ink : UI_INK);
      gfx->setTextSize(3);
      gfx->setCursor(CX - (int)strlen(T(S_TRIP_NEW)) * 9, ny);
      gfx->print(T(S_TRIP_NEW));
      ny += 34;
    }
    if (tripEncBerry[tripResultIndex]) {
      const char *bt = (pet.visitBerryDelta < 0) ? T(S_TRIP_LOST_BERRY_TO) : T(S_TRIP_GOT_BERRY);
      gfx->setTextColor(LOOK_B ? ink : UI_INK);
      gfx->setTextSize(3);
      gfx->setCursor(CX - (int)strlen(bt) * 9, ny);
      gfx->print(bt);
    }
  }

  if (tripEncCount != 0 && !tripResultIsMeet) {
    uint8_t sceneBio = (tripResultIndex < tripEncCount) ? biomeFor(tripEncSpecies[tripResultIndex])
                                                         : (pet.isEgg() ? 0 : biomeFor(pet.speciesId));  // Beere-verloren-Seite: eigene Landschaft
    gfx->setTextColor(groundTextCol(sceneBio, night ? UI_INK_NIGHT : UI_INK));
    gfx->setTextSize(2);
    gfx->setCursor(CX - (int)strlen(T(S_TRIP_NEXT)) * 6, LOOK_B ? TRIP_WEITER_Y : 390);
    gfx->print(T(S_TRIP_NEXT));
  } else if (tripResultIsMeet) {
    // Treffen: kein "weiter", stattdessen ein automatisch ablaufender
    // 5s-Countdown ("5","4","3"...), keine Touch-Interaktion noetig
    uint32_t remain = (millis() >= tripResultMeetUntil) ? 0 : (tripResultMeetUntil - millis());
    int secs = (int)(remain / 1000) + 1;
    if (secs > 5) secs = 5;
    if (secs < 1) secs = 1;
    char cbuf[4];
    snprintf(cbuf, sizeof(cbuf), "%d", secs);
    gfx->setTextColor(groundTextCol(tripResultIndex < tripEncCount ? biomeFor(tripEncSpecies[tripResultIndex]) : (pet.isEgg() ? 0 : biomeFor(pet.speciesId)),
                                    night ? UI_INK_NIGHT : UI_INK));
    if (LOOK_B) {  // Optik B: Pokemon steht tiefer, Countdown etwas kleiner und darunter
      gfx->setTextSize(4);
      gfx->setCursor(CX - (int)strlen(cbuf) * 12, 410);
    } else {
      gfx->setTextSize(5);
      gfx->setCursor(CX - (int)strlen(cbuf) * 15, 386);
    }
    gfx->print(cbuf);
    if (millis() >= tripResultMeetUntil) {
      tripResultOpen = false;
      tripResultIsMeet = false;
      tripPmd.unload();
    }
  }

  gfx->flush();
}

void renderCard() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  if (cardPage == 0) renderCardProfile();
  else if (cardPage == 1) renderCardStats();
  else if (cardPage == 2) renderCardMedals();
  else if (cardPage == 3) renderCardProgress();
  else if (cardPage == 4 && berryResultOpen) renderBerryResult();
  else if (cardPage == 4 && tripBerryChoiceOpen) renderBerryChoice();
  else renderCardTrip();

  // indicador de 5 paginas + ayuda -- oculto mientras se ve el resultado
  // de la baya magica (para dejar sitio a "Lecker!"/el corazon)
  if (!(cardPage == 4 && berryResultOpen)) {
    // seit 6.58 keine Seitenpunkte auf der Beeren-Auswahl: dort wird nicht geblaettert
    if (!(cardPage == 4 && tripBerryChoiceOpen))
      for (int i = 0; i < 5; i++) {
        if (i == cardPage) gfx->fillCircle(181 + i * 26, 374, 5, UI_INK);
        else gfx->drawCircle(181 + i * 26, 374, 4, UI_INK);
      }
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    printUmlaut(T(S_DETAIL_BACK), CX - umlautLen(T(S_DETAIL_BACK)) * 6, 398, 2, UI_INK);
  }
  gfx->flush();
}

// ---------- teclado para renombrar ----------

static const char KB_KEYS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";  // 26 Buchstaben; letzte Zeile: Y Z <-- <-- OK OK
#define KB_COLS 6
#define KB_X 40
#define KB_Y 97
#define KB_W 64
#define KB_H 52

void openKeyboard() {
  kbOpen = true;
  kbLastWasUmlaut = false;
  strncpy(nameBuf, pet.nick, sizeof(nameBuf) - 1);
  nameBuf[sizeof(nameBuf) - 1] = 0;
  nameLen = strlen(nameBuf);
}

void renderKeyboard() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_NAME)) * 6, 15);
  gfx->print(T(S_NAME));
  // buffer actual (mas estrecho que antes: 260 en vez de 300, para que quepa
  // sin recortarse tan cerca del borde superior del circulo)
  gfx->fillRoundRect(CX - 130, 45, 260, 40, 8, UI_WHITE);
  gfx->drawRoundRect(CX - 130, 45, 260, 40, 8, UI_INK);
  gfx->setTextSize(3);
  if (nameLen) printUmlaut(nameBuf, CX - 118, 55, 3, UI_INK);
  else { gfx->setCursor(CX - 118, 55); gfx->print("_"); }

  for (int i = 0; i < 26; i++) {
    int x = KB_X + (i % KB_COLS) * KB_W, y = KB_Y + (i / KB_COLS) * KB_H;
    gfx->fillRoundRect(x, y, KB_W - 6, KB_H - 6, 6, UI_WHITE);
    gfx->drawRoundRect(x, y, KB_W - 6, KB_H - 6, 6, UI_INK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(x + KB_W / 2 - 9, y + KB_H / 2 - 10);
    gfx->print(KB_KEYS[i]);
  }
  // letzte Zeile: Loeschen und OK jeweils doppelt breit (nehmen den
  // frueheren Platz von "." und "-" bzw. von der Loeschen-Taste ein)
  int rowY = KB_Y + 4 * KB_H;
  int delX = KB_X + 2 * KB_W, delW = 2 * KB_W - 6;
  int okX = KB_X + 4 * KB_W, okW = 2 * KB_W - 6;
  gfx->fillRoundRect(delX, rowY, delW, KB_H - 6, 6, UI_BAR_WARN);
  gfx->drawRoundRect(delX, rowY, delW, KB_H - 6, 6, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(delX + delW / 2 - 18, rowY + KB_H / 2 - 10);
  gfx->print("<--");
  gfx->fillRoundRect(okX, rowY, okW, KB_H - 6, 6, UI_BAR_WARN);
  gfx->drawRoundRect(okX, rowY, okW, KB_H - 6, 6, UI_INK);
  gfx->setCursor(okX + okW / 2 - 12, rowY + KB_H / 2 - 10);
  gfx->print("OK");
  printUmlaut(T(S_DETAIL_BACK), CX - umlautLen(T(S_DETAIL_BACK)) * 6, 398, 2, UI_INK);
  gfx->flush();
}

void keyboardTap(int16_t x, int16_t y) {
  if (y > 390) {  // tocar el aviso "tippen fuer zurueck" = cerrar SIN guardar
    kbOpen = false;
    return;
  }
  int col = (x - KB_X) / KB_W, row = (y - KB_Y) / KB_H;
  if (col < 0 || col >= KB_COLS || row < 0 || row >= 5) return;
  bool isDel = (row == 4 && (col == 2 || col == 3));  // doppelt breite Loeschen-Taste
  bool isOk = (row == 4 && (col == 4 || col == 5));   // doppelt breite OK-Taste
  int i = row * KB_COLS + col;  // nur bei Buchstaben (i<26) als Index gueltig
  if (isDel) {
    // si el ultimo caracter es un umlaut (marcado con backtick) Y se acaba
    // de crear por la conversion automatica (aun no se escribio nada mas
    // despues), el primer backspace lo deshace a las dos letras (UE/AE/OE)
    // en vez de borrarlo entero; un segundo backspace ya borra normal
    if (nameLen >= 2 && nameBuf[nameLen - 2] == '`' && kbLastWasUmlaut) {
      nameBuf[nameLen - 2] = nameBuf[nameLen - 1];  // U, A o O de vuelta
      nameBuf[nameLen - 1] = 'E';
      kbLastWasUmlaut = false;
    } else if (nameLen >= 2 && nameBuf[nameLen - 2] == '`') {
      nameLen -= 2;  // borra el umlaut completo (2 bytes) de una vez
      nameBuf[nameLen] = 0;
      kbLastWasUmlaut = false;
    } else if (nameLen) {
      nameBuf[--nameLen] = 0;
      kbLastWasUmlaut = false;
    }
  } else if (isOk) {
    pet.rename(nameBuf);
    kbOpen = false;
  } else if (i < 26 && umlautLen(nameBuf) < 11 && nameLen < sizeof(nameBuf) - 2) {
    char c = KB_KEYS[i];
    bool converted = false;
    // UE/AE/OE -> Ue/Ae/Oe (solo si el idioma actual usa nombres/interfaz
    // en aleman): el backtick antes de la letra es la misma codificacion
    // de umlaut que usa printUmlaut() en el resto del juego
    if (gLang != LANG_EN && c == 'E' && nameLen >= 1) {
      char prev = nameBuf[nameLen - 1];
      if (prev == 'U' || prev == 'A' || prev == 'O') {
        nameBuf[nameLen - 1] = '`';
        if (nameLen < sizeof(nameBuf) - 1) {
          nameBuf[nameLen] = prev;
          nameLen++;
          nameBuf[nameLen] = 0;
        }
        converted = true;
      }
    }
    if (!converted) {
      nameBuf[nameLen++] = c;
      nameBuf[nameLen] = 0;
    }
    kbLastWasUmlaut = converted;
  }
}

// ---------- galeria pokedex ----------

#define GAL_X 73
#define GAL_Y 72  // seit 6.27 12 Pixel hoeher: unten Platz fuer Punkte + "tippen fuer zurueck"
#define GAL_CELL 80

// dibuja una miniatura centrada en su celda; sil=true la pinta en tinta
void drawThumb(const uint8_t *b, int x, int y, int s, bool sil) {
  uint8_t w = b[0], h = b[1], n = b[2];
  const uint8_t *pal = b + 3;
  const uint8_t *d = pal + n * 2;
  int ox = x + (GAL_CELL - w * s) / 2;
  int oy = y + (GAL_CELL - h * s) / 2;
  for (int r = 0; r < h; r++) {
    for (int c = 0; c < w; c++) {
      uint8_t idx = d[r * w + c];
      if (idx == 0xFF) continue;
      uint16_t col = sil ? INK_K : (uint16_t)(pal[idx * 2] | (pal[idx * 2 + 1] << 8));
      gfx->fillRect(ox + c * s, oy + r * s, s, s, col);
    }
  }
}

// ---- Pokedex-Raster (Optik A und B), Zellen mit allen 8 Medaillen (Gold) ----
// Statt der gelben Scheibe: Lichtreflex ueber dem Pokemon + Glitzersterne.
// Das Raster wird dafuer ~12x pro Sekunde neu gezeichnet (nur wenn die Seite
// mindestens eine Goldzelle hat, siehe renderGallery).
#define GAL_FX_GOLD C565(0xf0, 0xb0, 0x18)
#define GAL_FX_MS 80   // Mindestabstand zwischen zwei Animationsbildern

uint32_t galHash32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}

// Position des Lichtbands (x+y relativ zur Zellmitte); -9999 = gerade kein Band.
// Seit 6.54: Jeder Durchlauf dauert 1 s; Takt je Pokemon 2,2..2,8 s plus Zufallsanteil 0..0,2 s je
// Durchlauf, also 2,0..3,0 s zwischen zwei Baendern desselben Pokemon. Takt, Versatz und Zufall
// haengen von galFxSeed ab, der bei jedem neuen Aufbau der Rasterseite (Wischen, Oeffnen, Zurueck
// aus der Detailansicht) neu gewuerfelt wird: Die Reihenfolge der Baender ist nie gleich.
// Zustandslos (nur Hash), braucht keinen Speicher je Pokemon.
uint32_t galFxSeed = 0;
int galShinePos(int dex, uint32_t t) {
  uint32_t hd = galHash32((uint32_t)dex * 2654435761u ^ galFxSeed);   // je Pokemon und Seitenbesuch
  uint32_t P = 2200 + hd % 601u;                                      // eigener Takt: 2,2 .. 2,8 s
  uint32_t tt = t + galHash32(hd + 1u) % P;                           // eigener Versatz
  uint32_t k = tt / P, r = tt % P;                                    // Durchlauf k, Zeit darin
  uint32_t s = galHash32(hd + 17u + k * 104729u) % 201u;              // Zufallsanteil je Lauf: 0 .. 0,2 s
  if (r < s || r >= s + 1000) return -9999;
  return -70 + (int)(140L * (r - s) / 1000);
}

// wie drawThumb, aber mit diagonalem Lichtband (sp = Bandmitte, -9999 = aus)
void drawThumbShine(const uint8_t *b, int x, int y, int s, bool sil, int cx, int cy, int sp) {
  uint8_t w = b[0], h = b[1], n = b[2];
  const uint8_t *pal = b + 3;
  const uint8_t *d = pal + n * 2;
  int ox = x + (GAL_CELL - w * s) / 2;
  int oy = y + (GAL_CELL - h * s) / 2;
  for (int r = 0; r < h; r++) {
    for (int c = 0; c < w; c++) {
      uint8_t idx = d[r * w + c];
      if (idx == 0xFF) continue;
      uint16_t col = sil ? INK_K : (uint16_t)(pal[idx * 2] | (pal[idx * 2 + 1] << 8));
      if (sp != -9999) {
        int dd = (ox + c * s - cx) + (oy + r * s - cy) - sp;
        if (dd < 0) dd = -dd;
        if (dd < 7) col = lerp565(col, 0xFFFF, 4, 5);
        else if (dd < 20) col = lerp565(col, 0xFFFF, 2, 5);
      }
      gfx->fillRect(ox + c * s, oy + r * s, s, s, col);
    }
  }
}

// vierzackiger Funke, Armlaenge L
void galSparkle(int x, int y, int L) {
  if (L <= 0) return;
  gfx->fillRect(x - L, y, 2 * L + 1, 1, GAL_FX_GOLD);
  gfx->fillRect(x, y - L, 1, 2 * L + 1, GAL_FX_GOLD);
  if (L >= 2) {
    gfx->fillRect(x - 1, y - 1, 3, 3, GAL_FX_GOLD);
    gfx->fillRect(x - L / 2, y, L, 1, 0xFFFF);
    gfx->fillRect(x, y - L / 2, 1, L, 0xFFFF);
  }
  if (L >= 4) gfx->fillRect(x - 1, y - 1, 3, 3, 0xFFFF);
}

// 4 Funken je Zelle mit eigener Dauer (1,0 / 1,5 / 1,5 / 2,0 s), nach jedem Zyklus neue Stelle
void galSparkles(int cx, int cy, int dex, uint32_t t) {
  static const uint16_t PER[4] = { 2000, 1500, 1000, 1500 };
  for (int i = 0; i < 4; i++) {
    uint32_t P = PER[i];
    uint32_t tt = t + galHash32(dex * 31 + i) % P;
    uint32_t k = tt / P;
    float u = (float)(tt % P) / P;                    // 0..1 des Zyklus
    if (u > 0.62f) continue;                          // Pause
    int L = (int)(9.0f * sinf(3.14159f * u / 0.62f) + 0.5f);
    uint32_t h = galHash32(dex * 977 + i * 131 + k * 7919);
    float ang = (h & 1023) * (6.2832f / 1024.0f);
    int rad = 24 + (h >> 10) % 20;                    // 24..43 px vom Zellmittelpunkt
    galSparkle(cx + (int)(cosf(ang) * rad), cy + (int)(sinf(ang) * rad), L);
  }
}

// ---- Pokedex-Detailansicht: bewegte Pokebaelle (nur freigeschaltete Medaillen) ----
// Freigeschaltete Baelle bewegen sich in zufaelliger Reihenfolge mit drei Bewegungen (siehe unten),
// als saesse ein lebendiges Pokemon darin (Neigung um den Fusspunkt). Seit 6.50 zufaellig statt fester Rhythmus.
// Die Schatten (noch nicht freigeschaltet) bleiben unbewegt, aber heller/unauffaelliger.
#define MEDAL_SHADOW_COL lerp565(UI_BG_DAY, C565(0xB8, 0xB8, 0xD0), 2, 5)
#define MEDAL_CYCLE_MS 2800        // Richtwert: alle Baelle zusammen starten im Mittel alle MEDAL_CYCLE_MS / Anzahl ms einen
#define MEDAL_MIN_START_GAP 1500   // ein Ball startet fruehestens 1,5 s nach seinem letzten Start (Pause bleibt kurz)

// Drei Bewegungen (zufaellig, nie zweimal dieselbe hintereinander bei demselben Ball):
//  0 = Hopser + Wackeln (1,1 s), 1 = zweimal hoch huepfen + Wackeln (1,4 s), 2 = nur hin und her wackeln (0,9 s)
static const uint16_t MEDAL_DUR[3] = { 1100, 1380, 900 };

// Zufaellige Reihenfolge: Ein kleiner Planer vergibt im Abstand MEDAL_CYCLE_MS / Anzahl (+-120 ms) jeweils
// einen Start an einen zufaellig gewaehlten Ball (nicht der zuletzt gestartete, mindestens 1,5 s nach seinem
// letzten Start) und waehlt zufaellig die Bewegung. So gibt es keinen festen Rhythmus mehr, und es bewegt sich
// fast immer mindestens ein Ball.
static int16_t  mdDex = -1;
static uint8_t  mdEarned = 0;
static uint32_t mdLastCall = 0, mdNextEv = 0, mdRng = 1;
static uint32_t mdStart[MED_COUNT];
static uint8_t  mdVar[MED_COUNT];
static int8_t   mdPrev = -1;
static uint32_t mdRand() { mdRng ^= mdRng << 13; mdRng ^= mdRng >> 17; mdRng ^= mdRng << 5; return mdRng; }

// Einmal je Bild vor dem Zeichnen der Baelle aufrufen (dex = Pokemon, earned = Bitmaske der Medaillen)
void medalBallUpdate(int dex, uint8_t earned, uint32_t t) {
  if (dex != mdDex || earned != mdEarned || (uint32_t)(t - mdLastCall) > 400) {   // neu betreten / anderes Pokemon
    mdDex = (int16_t)dex; mdEarned = earned;
    mdRng = galHash32(t ^ ((uint32_t)dex * 2654435761u)) | 1u;
    for (int i = 0; i < MED_COUNT; i++) { mdStart[i] = t - 100000u; mdVar[i] = (uint8_t)(mdRand() % 3); }
    mdPrev = -1;
    mdNextEv = t + 150 + mdRand() % 350;
  }
  mdLastCall = t;
  int n = 0;
  for (int i = 0; i < MED_COUNT; i++) if (earned & (1 << i)) n++;
  if (n == 0) return;
  for (int guard = 0; guard < 8 && (int32_t)(t - mdNextEv) >= 0; guard++) {
    uint32_t ev = mdNextEv;
    int cand[MED_COUNT], nc = 0, oldest = -1; uint32_t oldAge = 0;
    for (int i = 0; i < MED_COUNT; i++) {
      if (!(earned & (1 << i)) || (i == mdPrev && n > 1)) continue;
      uint32_t age = ev - mdStart[i];
      if (age >= MEDAL_MIN_START_GAP) cand[nc++] = i;
      if (oldest < 0 || age > oldAge) { oldest = i; oldAge = age; }
    }
    int pick = nc ? cand[mdRand() % nc] : oldest;
    if (pick < 0) break;
    mdStart[pick] = ev;
    mdVar[pick] = (uint8_t)((mdVar[pick] + 1 + mdRand() % 2) % 3);   // immer eine andere Bewegung als zuletzt
    mdPrev = (int8_t)pick;
    int32_t base = (n == 1) ? 2000 : (n == 2) ? 1100 : MEDAL_CYCLE_MS / n;   // wenige Baelle: etwas dichter, damit die Pausen kurz bleiben
    int32_t step = base + (int32_t)(mdRand() % 241u) - 120;
    if (step < 200) step = 200;
    mdNextEv = ev + (uint32_t)step;
  }
}

// Haltung des Balls m zum Zeitpunkt t: ang = Neigung in Grad (um den Fusspunkt), lift = Anhebung in px
void medalBallPose(int m, uint32_t t, float &ang, int &lift) {
  ang = 0; lift = 0;
  if (m < 0 || m >= MED_COUNT || !(mdEarned & (1 << m))) return;
  int32_t tt = (int32_t)(t - mdStart[m]);
  if (tt < 0 || tt >= (int32_t)MEDAL_DUR[mdVar[m]]) return;
  switch (mdVar[m]) {
    case 0:                                           // Hopser, dann zweimal Wackeln (klingt ab)
      if (tt < 300) {
        lift = (int)(9.0f * sinf(3.14159f * tt / 300.0f) + 0.5f);
      } else {
        float w = (float)(tt - 300);
        float env = 1.0f - w / 800.0f;
        float sn = sinf(6.2832f * w / 400.0f);
        ang = 16.0f * sn * env;
        lift = (int)(2.0f * fabsf(sn) * env + 0.5f);
      }
      break;
    case 1:                                           // zweimal hoch huepfen, danach Wackeln (klingt ab)
      if (tt < 680) {
        int u = tt % 340;
        lift = (int)(11.0f * sinf(3.14159f * u / 340.0f) + 0.5f);
      } else {
        float w = (float)(tt - 680);
        float env = 1.0f - w / 700.0f;
        float sn = sinf(6.2832f * w / 400.0f);
        ang = 16.0f * sn * env;
        lift = (int)(2.0f * fabsf(sn) * env + 0.5f);
      }
      break;
    default: {                                        // nur hin und her wackeln, klingt ab
      float env = 1.0f - tt / 900.0f;
      ang = 18.0f * sinf(6.2832f * tt / 360.0f) * env;
      break;
    }
  }
}

// Pokeball (SPR_ICON_PLAY, 16x16 bei Faktor 2 = 32x32), um den Fusspunkt geneigt und angehoben.
// bx/by = linke obere Ecke der Kachel
void drawMedalBall(int bx, int by, float angDeg, int lift) {
  if (angDeg > -0.6f && angDeg < 0.6f) { drawMap(SPR_ICON_PLAY, 16, bx, by - lift, 2, false); return; }
  float a = angDeg * 3.14159f / 180.0f, ca = cosf(a), sa = sinf(a);
  const float px = 16.0f, py = 28.0f;               // Drehpunkt: Unterkante des Balls
  for (int dy = -4; dy < 34; dy++) {
    for (int dx = -8; dx < 40; dx++) {
      float rx = dx + 0.5f - px, ry = dy + 0.5f - py;
      float sx = ca * rx + sa * ry + px, sy = -sa * rx + ca * ry + py;
      if (sx < 0 || sy < 0 || sx >= 32 || sy >= 32) continue;
      char ch = SPR_ICON_PLAY[(int)sy / 2][(int)sx / 2];
      if (ch == '.') continue;
      gfx->drawPixel(bx + dx, by + dy - lift, spriteColor(ch));
    }
  }
}

// vierzackiger Funke fuer die Detailansicht: wie galSparkle, aber bei grossen Armen 2 px dick (besser sichtbar)
void galSparkleBig(int x, int y, int L) {
  if (L < 8) { galSparkle(x, y, L); return; }
  gfx->fillRect(x - L, y, 2 * L + 2, 2, GAL_FX_GOLD);
  gfx->fillRect(x, y - L, 2, 2 * L + 2, GAL_FX_GOLD);
  gfx->fillRect(x - 2, y - 2, 6, 6, GAL_FX_GOLD);
  gfx->fillRect(x - L / 2, y, L, 2, 0xFFFF);
  gfx->fillRect(x, y - L / 2, 2, L, 0xFFFF);
  gfx->fillRect(x - 1, y - 1, 4, 4, 0xFFFF);
}

// Glitzersterne um das Pokemon bei allen 8 Medaillen: gleiche Funken wie im Raster (galSparkle),
// nur ohne Lichtreflex. Radius und Armlaenge der Groesse der Detailansicht angepasst.
void galSparklesBig(int cx, int cy, int dex, uint32_t t) {
  static const uint16_t PER[4] = { 2000, 1500, 1000, 1500 };
  for (int i = 0; i < 4; i++) {
    uint32_t P = PER[i];
    uint32_t tt = t + galHash32(dex * 31 + i) % P;
    uint32_t k = tt / P;
    float u = (float)(tt % P) / P;
    if (u > 0.62f) continue;
    int L = (int)(13.0f * sinf(3.14159f * u / 0.62f) + 0.5f);
    uint32_t h = galHash32(dex * 977 + i * 131 + k * 7919);
    float ang = (h & 1023) * (6.2832f / 1024.0f);
    int rad = 52 + (h >> 10) % 46;                  // 52..97 px vom Mittelpunkt
    galSparkleBig(cx + (int)(cosf(ang) * rad), cy + (int)(sinf(ang) * rad), L);
  }
}


// eleccion de hueco de favorito (1/2/3): pulsacion larga sobre la imagen en
// el detalle del pokedex la abre; toca un hueco para reemplazarlo
// calcula la etapa del pokedex (1-10, o "final") segun el % aufgedeckt
void pokedexTier(int &stage, bool &isFinal, const char **name) {
  uint16_t reg = pet.registeredCount();
  if (reg >= DEX_COUNT) { isFinal = true; stage = 11; *name = T(S_RANK_10); return; }
  isFinal = false;
  int pct = reg * 100 / DEX_COUNT;
  stage = (pct == 0) ? 1 : ((pct - 1) / 10) + 1;
  if (stage > 10) stage = 10;
  *name = T((StrId)(S_RANK_0 + (stage - 1)));
}

bool pokedexHintShown = false;
uint32_t pokedexHintUntil = 0;

void renderPokedexOverview() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - (int)strlen(T(S_POKEDEX_HDR)) * 9, 40);
  gfx->print(T(S_POKEDEX_HDR));

  int stage; bool isFinal; const char *rankName;
  pokedexTier(stage, isFinal, &rankName);
  gfx->setTextSize(2);
  if (isFinal) {
    gfx->setCursor(CX - (int)strlen(T(S_STAGE_FINAL)) * 6, 82);
    gfx->print(T(S_STAGE_FINAL));
  } else {
    char buf[16];
    snprintf(buf, sizeof(buf), T(S_STAGE_FMT), stage);
    gfx->setCursor(CX - (int)strlen(buf) * 6, 82);
    gfx->print(buf);
  }
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(rankName) * 6, 106);
  gfx->print(rankName);

  // cantidad de estrellas segun la etapa (1-11, 11 = forma final): 1&2->0,
  // 3&4->1, 5&6->2, 7&8->3, 9&10->4, 11->5. El grupo visible siempre
  // centrado, igual que antes con las 5
  int starCount = (stage - 1) / 2;
  if (starCount > 5) starCount = 5;
  int starGap = 28;
  int starX0 = CX - starGap * (starCount - 1) / 2;
  for (int i = 0; i < starCount; i++) {
    fillStar(starX0 + i * starGap, 145, 11, 5, C565(0xFF, 0xC8, 0x00));
  }

  char countBuf[16];
  snprintf(countBuf, sizeof(countBuf), "%u/%u", pet.registeredCount(), (unsigned)DEX_COUNT);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - (int)strlen(countBuf) * 9, 400);
  gfx->print(countBuf);

  if (pokedexHintShown && millis() < pokedexHintUntil) {
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - (int)strlen(T(S_FAV_HINT1)) * 6, 200);
    gfx->print(T(S_FAV_HINT1));
    printUmlaut(T(S_FAV_HINT2), CX - umlautLen(T(S_FAV_HINT2)) * 6, 224, 2, UI_INK);
    gfx->setCursor(CX - (int)strlen(T(S_FAV_HINT3)) * 6, 248);
    gfx->print(T(S_FAV_HINT3));
    gfx->flush();
    return;
  }

  // tres favoritos: el del medio un poco mas abajo, siguiendo la curva de
  // la pantalla redonda
  int xs[3] = { CX - 115, CX, CX + 115 };
  int ys[3] = { 300, 330, 300 };
  for (int i = 0; i < 3; i++) {
    if (favPmd[i].loaded) drawPmdActM(favPmd[i], PMD_IDLE, xs[i], ys[i], millis(), true, false, 3, false);
  }

  gfx->flush();
}

void pokedexOverviewTap(int16_t x, int16_t y) {
  int ys[3] = { 300, 330, 300 };
  int xs[3] = { CX - 115, CX, CX + 115 };
  for (int i = 0; i < 3; i++) {
    int dx = x - xs[i], dy = y - ys[i] + 40;
    if (dx * dx + dy * dy <= 70 * 70) {
      pokedexHintShown = true;
      pokedexHintUntil = millis() + 3500;
      return;
    }
  }
  if (pokedexHintShown) { pokedexHintShown = false; return; }  // el toque solo cierra el aviso
  // cualquier otro toque: vuelve al juego (ya no pasa a la rejilla directamente)
  galleryOpen = false;
  pokedexOverviewOpen = false;
  for (int i = 0; i < 3; i++) favPmd[i].unload();
}

void renderFavConfirm() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_BAR_OK);
  gfx->setTextSize(2);
  printUmlaut(T(S_FAV_CONFIRMED), CX - umlautLen(T(S_FAV_CONFIRMED)) * 6, 220, 2, UI_BAR_OK);
  gfx->flush();
}

void renderFavSlot() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  printUmlaut(T(S_FAV_TITLE), CX - umlautLen(T(S_FAV_TITLE)) * 6, 90, 2, UI_INK);
  for (int i = 0; i < 3; i++) {
    int by = 150 + i * 66;
    gfx->fillRoundRect(83, by, 300, 52, 12, UI_WHITE);
    gfx->drawRoundRect(83, by, 300, 52, 12, UI_INK);
    char line[28];
    snprintf(line, sizeof(line), "%d  %s", i + 1, dexName(pet.favorites[i]));
    gfx->setTextColor(UI_INK);
    gfx->setCursor(CX - (int)strlen(line) * 6, by + 18);
    gfx->print(line);
  }
  gfx->setTextColor(UI_INK);
  printUmlaut(T(S_DETAIL_BACK), CX - umlautLen(T(S_DETAIL_BACK)) * 6, 398, 2, UI_INK);
  gfx->flush();
}

void favSlotTap(int16_t x, int16_t y) {
  for (int i = 0; i < 3; i++) {
    int by = 150 + i * 66;
    if (x >= 83 && x <= 383 && y >= by && y <= by + 52) {
      pet.setFavorite(i, favSlotChooseDex);
      favSlotChoiceOpen = false;
      favConfirmShown = true;
      favConfirmUntil = millis() + 2000;
      sfxPlay(SFX_MEDAL);
      return;
    }
  }
  if (y > 390) {  // tocar el aviso "tap to go back" = cerrar sin elegir
    favSlotChoiceOpen = false;
  }
}

// Seiten des Pokedex: siehe GAL_REGION oben (jede Region beginnt auf einer
// neuen Seite, auch wenn die letzte Seite der vorigen Region halb leer bleibt)

// dex de la celda r,c en la pagina dada, o 0 si esta casilla no corresponde
// a ninguna especie (pagina a medias al final de una region)
static int16_t galleryDexAt(int page, int r, int c) {
  int pageInRegion;
  int rg = galRegionOfPage(page, pageInRegion);
  int16_t dex = GAL_REGION[rg].first + pageInRegion * 16 + r * 4 + c;
  return (dex > GAL_REGION[rg].last) ? 0 : dex;
}

void renderGallery() {
  if (galleryDetail) {  // vista detalle: se redibuja siempre (animada)
    gfx->fillScreen(RGB565_BLACK);
    gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
    const DexEntry &d = DEX_TBL[galleryDetail];
    bool reg = pet.isRegistered(galleryDetail);
    char head[24];
    snprintf(head, sizeof(head), "N.%03d %s%s", galleryDetail,
             pet.isShinyRegistered(galleryDetail) ? "*" : "", reg ? dexName(galleryDetail) : "???");
    gfx->setTextColor(reg ? d.accent : UI_INK);
    int glen = umlautLen(head);
    int gts = (glen <= 13) ? 3 : 2;  // auto-encoge nombres largos (no caben a t3)
    gfx->setTextSize(gts);
    printUmlaut(head, CX - glen * (gts == 3 ? 9 : 6), gts == 3 ? 56 : 60, gts, reg ? d.accent : UI_INK);
    if (!galleryInfoShown) {
      if (galleryPmd.loaded) {
        // animado y a color si esta registrado; silueta estatica si no (estilo "?")
        drawPmdActM(galleryPmd, PMD_IDLE, CX, 300, reg ? millis() : 0, true, !reg, 6, false);
      } else {
        const uint8_t *t = thumbs.get(galleryDetail);
        if (t) drawThumb(t, CX - GAL_CELL, 135, 4, !reg);
      }
      // 8 insignias (pokebolas) en circulo alrededor del bicho: una por
      // medalla, a color si esta especie la gano alguna vez, en silueta si no.
      // Radio grande y centro mas arriba que el sprite (no en su mismo punto
      // de anclaje) para que no se solape con especies altas/grandes
      uint8_t earned = pet.dexMedals[galleryDetail];
      if (earned) medalBallUpdate(galleryDetail, earned, millis());
      for (int m = 0; m < MED_COUNT; m++) {
        float a = -(float)PI / 2 + m * (2.0f * (float)PI / MED_COUNT);
        int bx = CX + (int)(cosf(a) * 130) - 16, by = 240 + (int)(sinf(a) * 130) - 16;
        if (earned & (1 << m)) {  // freigeschaltet: zufaellig Hopser/Wackeln (seit 6.48, zufaellig seit 6.50)
          float mAng; int mLift;
          medalBallPose(m, millis(), mAng, mLift);
          drawMedalBall(bx, by, mAng, mLift);
        } else drawMapTint(SPR_ICON_PLAY, 16, bx, by, 2, MEDAL_SHADOW_COL);  // Schatten: still, heller
      }
      if (earned == 0xFF) galSparklesBig(CX, 235, galleryDetail, millis());  // alle 8 Medaillen: Glitzersterne
      if (galleryMedalTapId < MED_COUNT && millis() < galleryMedalTapUntil) {
        const char *nm = medalDesc(galleryMedalTapId);
        gfx->fillRoundRect(CX - (int)strlen(nm) * 6 - 10, 138, (int)strlen(nm) * 12 + 20, 28, 8, UI_INK);
        gfx->setTextColor(UI_BG_DAY);
        gfx->setTextSize(2);
        gfx->setCursor(CX - (int)strlen(nm) * 6, 145);
        gfx->print(nm);
      }
    } else {
      // info: tipo (con su color oficial), begegnungen, tasa de victorias, criado
      gfx->setTextSize(3);
      int ty = 140;
      const char *typeLbl = T(S_INFO_TYPE);
      const char *tName = typeName(d.type);
      char full[32];
      snprintf(full, sizeof(full), "%s %s", typeLbl, tName);
      int sx = CX - (int)umlautLen(full) * 9;
      gfx->setTextColor(UI_INK);
      gfx->setCursor(sx, ty);
      printUmlaut(typeLbl, sx, ty, 3, UI_INK);
      gfx->setTextColor(TYPE_COLOR[d.type]);
      printUmlaut(tName, sx + ((int)strlen(typeLbl) + 1) * 18, ty, 3, TYPE_COLOR[d.type]);

      uint16_t bt = pet.dexBattleTotal[galleryDetail], bw = pet.dexBattleWins[galleryDetail];
      uint16_t winPct = bt ? (uint16_t)((uint32_t)bw * 100 / bt) : 0;
      char l2[28], l3[28], l4[28];
      snprintf(l2, sizeof(l2), "%s %u", T(S_INFO_MEETINGS), pet.dexEncounters[galleryDetail]);
      snprintf(l3, sizeof(l3), "%s %u%%", T(S_INFO_WINRATE), winPct);
      snprintf(l4, sizeof(l4), "%s %u", T(S_INFO_RAISED), pet.dexRaised[galleryDetail]);
      gfx->setTextColor(UI_INK);
      gfx->setCursor(CX - (int)strlen(l2) * 9, ty + 54);
      gfx->print(l2);
      gfx->setCursor(CX - (int)strlen(l3) * 9, ty + 108);
      gfx->print(l3);
      gfx->setCursor(CX - (int)strlen(l4) * 9, ty + 162);
      gfx->print(l4);
    }
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    printUmlaut(T(S_DETAIL_BACK), CX - umlautLen(T(S_DETAIL_BACK)) * 6, 408, 2, UI_INK);
    gfx->flush();
    return;
  }

  // Seiten mit Goldzellen laufen animiert (~12 Bilder/s), alle anderen bleiben statisch
  if (!galleryDirty) {
    if (!(galleryGoldOnPage && millis() - galleryFxLast >= GAL_FX_MS)) return;  // la rejilla es estatica
  }
  if (galleryDirty) galFxSeed = esp_random();  // neue Seite/Besuch: Lichtband-Takte neu wuerfeln (seit 6.54)
  galleryDirty = false;
  galleryFxLast = millis();
  galleryGoldOnPage = false;

  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  // Ueberschrift = Name der Region dieser Seite (Antippen: naechste Region)
  int pageInRegion;
  int rgn = galRegionOfPage(galleryPage, pageInRegion);
  const char *head = GAL_REGION[rgn].name;
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(head) * 9, 26);
  gfx->print(head);

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      int16_t dex = galleryDexAt(galleryPage, r, c);
      if (dex == 0) continue;  // celda vacia (pagina a medias al final de una generacion)
      int x = GAL_X + c * GAL_CELL, y = GAL_Y + r * GAL_CELL;
      // las 8 medallas completas para esta especie: brillo dorado suave y
      // circular (antes era un rectangulo solido con borde duro). Varios
      // circulos concentricos, mas tenues hacia el borde, simulan un
      // desenfoque sin necesitar mezcla real de alpha; pulsa despacio en
      // tamano para dar sensacion de brillo vivo, con una fase distinta
      // por especie para que no todas pulsen exactamente igual
      const uint8_t *tb = thumbs.get(dex);
      if (pet.dexMedals[dex] == 0xFF && tb) {  // Gold: Lichtreflex + Glitzersterne (Optik A und B)
        galleryGoldOnPage = true;
        int gcx = x + GAL_CELL / 2, gcy = y + GAL_CELL / 2;
        uint32_t tf = millis();
        {
          drawThumbShine(tb, x, y, 2, !pet.isRegistered(dex), gcx, gcy, galShinePos(dex, tf));
          if (pet.isShinyRegistered(dex)) {
            gfx->setTextColor(UI_BAR_WARN);
            gfx->setTextSize(2);
            gfx->setCursor(x + 62, y + 4);
            gfx->print("*");
          }
        }
        galSparkles(gcx, gcy, dex, tf);
        continue;
      }
      const uint8_t *t = thumbs.get(dex);
      if (t) {
        drawThumb(t, x, y, 2, !pet.isRegistered(dex));
        if (pet.isShinyRegistered(dex)) {
          gfx->setTextColor(UI_BAR_WARN);
          gfx->setTextSize(2);
          gfx->setCursor(x + 62, y + 4);
          gfx->print("*");
        }
      } else {
        char num[6];
        snprintf(num, sizeof(num), "%d", dex);
        gfx->setTextColor(UI_TRACK);
        gfx->setTextSize(2);
        gfx->setCursor(x + 24, y + 32);
        gfx->print(num);
      }
    }
  }
  // Seitenpunkte: eine Reihe, nur die Seiten der aktuellen Region (max. 10),
  // zentriert; der aktive Punkt ist gefuellt. Darunter "tippen fuer zurueck".
  int nDots = galRegionPages(rgn);
  int dx0 = CX - (nDots - 1) * 7;
  for (int i = 0; i < nDots; i++) {
    int dx = dx0 + i * 14;
    if (i == pageInRegion) gfx->fillCircle(dx, 404, 4, UI_INK);
    else gfx->drawCircle(dx, 404, 3, UI_INK);
  }
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  printUmlaut(T(S_DETAIL_BACK), CX - umlautLen(T(S_DETAIL_BACK)) * 6, 420, 2, UI_INK);
  gfx->flush();
}

void galleryTap(int16_t x, int16_t y) {
  if (galleryDetail) {
    if (y > 390) {  // tocar el aviso "tap: back" = volver a la rejilla
      galleryDetail = 0;
      galleryInfoShown = false;
      galleryPmd.unload();
      galleryDirty = true;
      return;
    }
    if (!galleryInfoShown) {
      // tocar una insignia de medalla: muestra su nombre unos segundos
      // (para depurar sin ambiguedad cual esta desbloqueada). Zona de toque
      // un poco mas grande que el dibujo, para que sea facil de acertar
      for (int m = 0; m < MED_COUNT; m++) {
        float a = -(float)PI / 2 + m * (2.0f * (float)PI / MED_COUNT);
        int ccx = CX + (int)(cosf(a) * 130), ccy = 240 + (int)(sinf(a) * 130);
        if (x >= ccx - 22 && x < ccx + 22 && y >= ccy - 22 && y < ccy + 22) {
          galleryMedalTapId = (uint8_t)m;
          galleryMedalTapUntil = millis() + 2500;
          return;
        }
      }
      // la info solo se abre tocando CENTRADO sobre la imagen del bicho, no
      // en cualquier sitio (antes bastaba con fallar una insignia de medalla)
      int ddx = x - CX, ddy = y - 240;
      if (ddx * ddx + ddy * ddy <= 100 * 100) galleryInfoShown = true;
      return;
    }
    galleryInfoShown = false;  // en la vista de info, cualquier toque vuelve a la imagen
    return;
  }
  if (y < 66) {  // Ueberschrift (Regionsname) antippen = naechste Region, erste Seite
    int pr;
    int rg = (galRegionOfPage(galleryPage, pr) + 1) % GAL_REGIONS;
    galleryPage = galRegionStartPage(rg);
    galleryDirty = true;
    return;
  }
  if (y >= 396) {  // Punkte / "tippen fuer zurueck" = Pokedex verlassen
    galleryOpen = false;
    galleryPmd.unload();
    return;
  }
  if (x < GAL_X || y < GAL_Y) return;  // links/oberhalb des Rasters (Ganzzahl-Division rundet sonst auf 0)
  int c = (x - GAL_X) / GAL_CELL, r = (y - GAL_Y) / GAL_CELL;
  if (c < 0 || c > 3 || r < 0 || r > 3) return;
  int16_t dex = galleryDexAt(galleryPage, r, c);
  if (dex == 0) return;
  if (!pet.isRegistered(dex)) return;  // aun no aufgedeckt: no se puede abrir
  galleryDetail = dex;
  galleryInfoShown = false;
  galleryMedalTapId = 255;
  galleryPmd.load(dex, pet.isShinyRegistered(dex));
}

void drawBattery() {
  int pc = batPercent();
  if (pc < 0) return;  // sin bateria conectada
  int x = CX - 14, y = 12, w = 24, h = 11;
  bool charging = batCharging();
  uint16_t col = charging ? UI_BAR_OK
                 : (pc >= 40) ? inkColor()
                 : (pc >= 15) ? UI_BAR_WARN
                              : UI_BAR_BAD;
  gfx->drawRoundRect(x, y, w, h, 2, col);
  gfx->fillRect(x + w, y + 3, 3, 5, col);  // borne
  if (charging) {
    // rayo de carga (zigzag) en vez de la barra de nivel
    uint16_t bolt = C565(0xff, 0xd9, 0x4a);
    int bx = x + w / 2;
    gfx->fillTriangle(bx + 3, y + 1, bx - 4, y + 6, bx + 1, y + 6, bolt);
    gfx->fillTriangle(bx - 1, y + 5, bx + 4, y + 5, bx - 3, y + 10, bolt);
  } else {
    int fw = (pc > 95) ? (w - 4) : (w - 4) * pc / 100;
    if (fw > 0) gfx->fillRect(x + 2, y + 2, fw, h - 4, col);
  }
}

void drawHeader(const char *name, uint16_t nameColor, const char *msg) {
  drawBattery();
  gfx->setTextColor(nameColor);
  gfx->setTextSize(3);
  printUmlaut(name, CX - umlautLen(name) * 9, 52, 3, nameColor);
  gfx->setTextColor(inkColor());
  if (sleepMsgId == S_BATTLE_NO_OPP1 && millis() < sleepMsgUntil) {
    // ahora son dos lineas cortas fijas: cabe con letra grande, igual que
    // un aviso normal (ya no hace falta partir una frase larga)
    const char *line1 = T(S_BATTLE_NO_OPP1), *line2 = T(S_BATTLE_NO_OPP2);
    gfx->setTextSize(3);
    gfx->setCursor(CX - (int)strlen(line1) * 9, 78);
    gfx->print(line1);
    printUmlaut(line2, CX - umlautLen(line2) * 9, 104, 3, inkColor());
  } else {
    printUmlaut(msg, CX - umlautLen(msg) * 9, 90, 3, inkColor());
  }
}

// animacion de la ceremonia (10s): despedida = reverencia con corazones y se
// aleja caminando; escapada = se asusta y sale corriendo. Sustituye al idle.
void drawCeremony() {
  if (!pmd.loaded) { drawPet(); return; }  // respaldo si no hay sprite PMD
  uint32_t now = millis();
  float t = pet.ceremonyT();               // 0..1 a lo largo de los 10s
  bool panic = (pet.ceremony == CER_RUNAWAY);
  int x = CX, y = PET_GROUND;
  uint8_t act = PMD_IDLE;

  if (panic) {
    // final triste: penumbra azulada + lluvia
    for (int i = 0; i < 46; i++) {
      int rx = (i * 47 + now / 3) % 466;
      int ry = (i * 91 + now / 2) % 470;
      gfx->drawLine(rx, ry, rx - 3, ry + 12, C565(0x6a, 0x84, 0xb0));
    }
    bool fade = false;
    if (t < 0.30f) {                       // cabizbajo, temblando
      act = pmd.has(PMD_HURT) ? PMD_HURT : PMD_IDLE;
      x = CX + (int)(4 * sinf(now * 0.04f));
    } else {                               // se aleja despacio y se desvanece
      act = pmd.has(PMD_WALKL) ? PMD_WALKL : PMD_IDLE;
      x = CX - (int)(((t - 0.30f) / 0.70f) * (CX + 120));
      fade = (t > 0.6f) && ((now / 160) % 2 == 0);  // parpadea hacia la silueta
    }
    drawPmdAct(act, x, y, now, true, fade, 5, false);  // fade=silueta: se difumina al irse
    // lagrima cayendo del bicho
    if (t < 0.55f) {
      int ty = y - 150 + (int)((now / 6) % 40);
      gfx->fillRect(x + 6, ty, 3, 6, C565(0x9a, 0xc4, 0xe8));
    }
    return;
  }

  // despedida epica: halo dorado pulsante + chispas y corazones que ascienden
  int gcy = PET_GROUND - 96;
  for (int k = 0; k < 4; k++) {
    int r = 60 + k * 34 + (int)(10 * sinf(now * 0.02f));
    gfx->drawCircle(CX, gcy, r, C565(0xff, 0xdf, 0x8a));
  }
  for (int i = 0; i < 16; i++) {
    int px = (i * 71 + 28) % 466;
    int py = 410 - (int)((now / 8 + i * 70) % 360);   // suben y reaparecen abajo
    if (py < 30) continue;
    if (i % 4 == 0) drawMap(SPR_HEART, 32, px - 8, py - 8, 1, false);  // corazoncito
    else gfx->fillRect(px, py, 4, 4, (i % 2) ? C565(0xff, 0xe7, 0x9f) : C565(0xff, 0x9a, 0xc0));
  }

  if (t < 0.45f) {                         // reverencia / pose de despedida
    act = pmd.has(PMD_POSE) ? PMD_POSE : (pmd.has(PMD_NOD) ? PMD_NOD : PMD_IDLE);
  } else {                                 // se aleja por la derecha
    act = pmd.has(PMD_WALKR) ? PMD_WALKR : PMD_IDLE;
    x = CX + (int)(((t - 0.45f) / 0.55f) * (CX + 140));
  }
  drawPmdAct(act, x, y, now, true, false, 5, false);
  if (pet.showHeart())                     // corazon grande siguiendo al bicho
    drawMap(SPR_HEART, 32, x + 50, y - 190, 2, false);
}

// dialogo de decision (2 botones apilados): evolucionar/mantener o despedirse/quedaros
void drawSleepConfirmDialog() {
  const char *q = T(S_SLEEP_CONFIRM_Q), *o1 = T(S_SLEEP_CONFIRM_YES), *o2 = T(S_SLEEP_CONFIRM_NO);
  gfx->fillRoundRect(73, 133, 320, 200, 16, UI_WHITE);
  gfx->drawRoundRect(73, 133, 320, 200, 16, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(q) * 6, 153);
  gfx->print(q);
  // Buttons seit 6.51 optisch hoeher (62 px, Luecke 12 px); Touchzonen unveraendert (183..235 / 267..319)
  gfx->fillRoundRect(93, 183, 280, 62, 12, UI_BAR_WARN);
  gfx->setTextColor(UI_INK);
  gfx->setCursor(CX - (int)strlen(o1) * 6, 183 + 31 - 7);
  gfx->print(o1);
  gfx->fillRoundRect(93, 257, 280, 62, 12, UI_BAR_OK);
  gfx->setTextColor(UI_WHITE);
  gfx->setCursor(CX - (int)strlen(o2) * 6, 257 + 31 - 7);
  gfx->print(o2);
}

void drawChoiceDialog() {
  const char *q, *o1, *o2;
  uint16_t c1, c2, t1, t2;
  if (choiceKind == 1) {  // evolucion
    q = T(S_EVO_Q); o1 = T(S_EVO_TAP); o2 = T(S_EVO_KEEP);
    c1 = UI_BAR_BAD; t1 = UI_WHITE; c2 = UI_TRACK; t2 = UI_INK;
  } else {                // despedida
    q = T(S_FAR_Q); o1 = T(S_FAR_GO); o2 = T(S_FAR_STAY);
    c1 = UI_BAR_WARN; t1 = UI_INK; c2 = UI_BAR_OK; t2 = UI_WHITE;
  }
  gfx->fillRoundRect(73, 133, 320, 200, 16, UI_WHITE);
  gfx->drawRoundRect(73, 133, 320, 200, 16, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(q) * 6, 153);
  gfx->print(q);
  // Buttons seit 6.51 optisch hoeher (62 px, Luecke 12 px); Touchzonen unveraendert (183..235 / 267..319)
  gfx->fillRoundRect(93, 183, 280, 62, 12, c1);     // boton accion
  gfx->setTextColor(t1);
  gfx->setCursor(CX - (int)strlen(o1) * 6, 183 + 31 - 7);
  gfx->print(o1);
  gfx->fillRoundRect(93, 257, 280, 62, 12, c2);     // boton mantener/quedaros
  gfx->setTextColor(t2);
  gfx->setCursor(CX - (int)strlen(o2) * 6, 257 + 31 - 7);
  gfx->print(o2);
}

// boton-CTA rojo y grande para evolucionar (pulsa para llamar la atencion)
void drawEvolveButton() {
  uint32_t now = millis();
  int p = (int)(5 * sinf(now * 0.006f));  // late: -5..5
  int x = EVO_BTN_X - p, y = EVO_BTN_Y - p, w = EVO_BTN_W + 2 * p, h = EVO_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 18, UI_BAR_BAD);
  gfx->drawRoundRect(x, y, w, h, 18, UI_WHITE);
  gfx->drawRoundRect(x + 2, y + 2, w - 4, h - 4, 16, UI_WHITE);
  gfx->setTextColor(UI_WHITE);
  gfx->setTextSize(3);
  const char *t = T(S_EVO_TAP);
  gfx->setCursor(CX - (int)strlen(t) * 9, y + h / 2 - 11);
  gfx->print(t);
}

// boton-CTA dorado de despedida: "<nombre> quiere decirte algo..."
void drawFarewellButton() {
  uint32_t now = millis();
  int p = (int)(4 * sinf(now * 0.005f));
  int x = FAR_BTN_X - p, y = FAR_BTN_Y - p, w = FAR_BTN_W + 2 * p, h = FAR_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 16, UI_BAR_WARN);
  gfx->drawRoundRect(x, y, w, h, 16, UI_INK);
  char buf[52];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  snprintf(buf, sizeof(buf), T(S_FAREWELL_BTN), nm);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  printUmlaut(buf, CX - umlautLen(buf) * 6, y + h / 2 - 8, 2, UI_INK);
}

// boton-CTA sombrio de escapada por abandono: "<nombre> se siente abandonado..."
// (final triste: azul-gris oscuro, latido lento y apagado)
void drawRunawayButton() {
  uint32_t now = millis();
  int p = (int)(3 * sinf(now * 0.003f));
  int x = FAR_BTN_X - p, y = FAR_BTN_Y - p, w = FAR_BTN_W + 2 * p, h = FAR_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 16, C565(0x3a, 0x44, 0x5a));
  gfx->drawRoundRect(x, y, w, h, 16, C565(0x70, 0x80, 0x98));
  char buf[52];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  snprintf(buf, sizeof(buf), T(S_RUNAWAY_BTN), nm);
  printUmlaut(buf, CX - umlautLen(buf) * 6, y + h / 2 - 8, 2, C565(0xc8, 0xd2, 0xe0));
}

// animacion epica de evolucion: halo radial + rayos giratorios + parpadeo del
// sprite acelerando + chispas que salen disparadas + fogonazo final
void drawEvolveFX(uint32_t now) {
  float t = pet.evolveT();          // 0..1
  int cx = CX, cy = PET_GROUND - 96;

  // halo radial que crece y pulsa
  int halo = 36 + (int)(t * 150) + (int)(8 * sinf(now * 0.02f));
  for (int k = 0; k < 4; k++) {
    int r = halo - k * 7;
    if (r > 0) gfx->drawCircle(cx, cy, r, UI_WHITE);
  }
  // rayos giratorios desde el centro del bicho
  float base = now * 0.004f;
  for (int i = 0; i < 12; i++) {
    float a = base + i * (float)(PI / 6);
    int len = 90 + (int)(70 * (0.5f + 0.5f * sinf(now * 0.012f + i)));
    gfx->drawLine(cx, cy, cx + (int)(cosf(a) * len), cy + (int)(sinf(a) * len), UI_WHITE);
  }
  // parpadeo entre la forma ANTERIOR y la NUEVA (siluetas), acelerando; al
  // final (t>0.9) se queda fija en la nueva para el fogonazo de revelado
  int period = 60 + (int)(220 * (1.0f - t));
  bool showOld = t < 0.9f && evoPmd.loaded && ((now / period) % 2) == 0;
  if (showOld) drawPmdActM(evoPmd, PMD_IDLE, cx, PET_GROUND, 0, true, true, 5, false);
  else drawPmdAct(PMD_IDLE, cx, PET_GROUND, 0, true, true, 5, false);
  // chispas que salen disparadas
  for (int i = 0; i < 10; i++) {
    float a = i * (float)(PI / 5) + t * 4.0f;
    int d = (int)((now / 14 + i * 33) % 200);
    int sx = cx + (int)(cosf(a) * d), sy = cy + (int)(sinf(a) * d);
    gfx->fillRect(sx - 2, sy - 2, 5, 5, (i & 1) ? C565(0xff, 0xe0, 0x70) : UI_WHITE);
  }
  // fogonazo final antes de revelar la forma nueva
  if (t > 0.9f) gfx->fillCircle(cx, cy, (int)(300 * (t - 0.9f) / 0.1f), UI_WHITE);
}

void drawPet() {
  if (pmd.loaded) {
    drawPetPMD();
    return;
  }
  if (mon.loaded) {
    drawPetSD();
    return;
  }
  int fi = flashIdxForDex(pet.speciesId);
  if (fi < 0) {
    // sin SD y sin sprite de flash: aviso claro de que faltan sprites
    gfx->setTextColor(inkColor());
    gfx->setTextSize(6);
    gfx->setCursor(CX - 18, PET_CY - 80);
    gfx->print("?");
    gfx->setTextSize(2);
    const char *l1 = T(S_NO_SPRITES);
    gfx->setCursor(CX - (int)strlen(l1) * 6, PET_CY - 4);
    gfx->print(l1);
    const char *l2 = T(S_LOAD_SPRITES);
    gfx->setCursor(CX - (int)strlen(l2) * 6, PET_CY + 20);
    gfx->print(l2);
    return;
  }
  const Species &sp = SPECIES[fi];
  int s = sp.scale;
  int x = CX - 16 * s;
  int y = PET_CY - 16 * s;

  // animacion de evolucion: alterna la silueta de la forma anterior y la nueva
  if (pet.evolving()) {
    bool flash = (millis() / 300) % 2;
    int16_t showDex = (flash && pet.prevSpeciesId >= 0) ? pet.prevSpeciesId : pet.speciesId;
    int sfi = flashIdxForDex(showDex);
    if (sfi >= 0) {
      const Species &show = SPECIES[sfi];
      drawMap(show.sprite, SPRITE_H, CX - 16 * show.scale, PET_CY - 16 * show.scale, show.scale, flash);
    }
    return;
  }

  PetMood m = pet.mood();
  if (m == MOOD_HAPPY && (millis() / 500) % 2) y -= 6;  // saltito

  drawMap(sp.sprite, SPRITE_H, x, y, s, false);

  // expresiones superpuestas usando las anclas de la especie
  bool blink = (millis() % 3500 < 300);
  if (m == MOOD_SLEEPING || blink) {
    overlayEye(sp, x, y, s, sp.eyeColL);
    overlayEye(sp, x, y, s, sp.eyeColR);
  }
  if (m == MOOD_EATING) overlayMouth(sp, x, y, s, true);
  else if (m == MOOD_SAD) overlayMouth(sp, x, y, s, false);

  if (pet.showHeart()) {
    if (LOOK_B) drawHeartFloat(x + 16 * s, y);
    else drawMap(SPR_HEART, 32, x + 20 * s, y - 2 * s, 2, false);
  }
}

// ---------- escena de bano ----------

void startBath() {
  if (pet.isEgg() || pet.sleeping || pet.napping() || pet.blocked() || bathUntil) return;
  if (pet.poops == 0 && pet.hygiene >= 25 && pet.bathOnCooldown()) {
    sleepMsgId = S_BATH_CD; sleepMsgUntil = millis() + 2500;
    sfxPlay(SFX_DENY);
    return;
  }
  if (pet.hygiene >= 100 && pet.poops == 0) {  // nada que limpiar
    sleepMsgId = S_NOT_YET; sleepMsgUntil = millis() + 2500;
    sfxPlay(SFX_DENY);
    return;
  }
  bathUntil = millis() + 3000;
  bathPending = true;
  sfxPlay(SFX_CLEAN);
  int cx = (int)beh.x;
  for (auto &b : bubbles) {
    b.x = cx - 70 + random(140);
    b.y = PET_GROUND - random(150);
    b.r = 8 + random(16);
    b.ph = random(64);
  }
}

// ---------- Optik B: Bad, Herz, Schnarchen, Essen (Vorlage: crenberries428, MIT) ----------

// Blasen sind durchsichtig: nur der Rand wird gezeichnet, dazu Glanzpunkte;
// beim Platzen weiten sich die Ringe und Troepfchen fliegen weg.
void drawBathB(uint32_t now, uint32_t left) {
  const uint16_t rim = C565(0x8f, 0xd0, 0xf0), rimIn = C565(0xdf, 0xf4, 0xff);
  float t = now / 220.0f;
  if (left > 800) {
    for (auto &b : bubbles) {
      int bx = b.x + (int)(sinf(t + b.ph) * 6);
      int by = b.y - (int)((3000 - left) / 90);
      gfx->drawCircle(bx, by, b.r, rim);
      gfx->drawCircle(bx, by, b.r - 1, rimIn);
      for (float a = 3.45f; a < 4.35f; a += 0.13f)  // Glanzbogen oben links
        gfx->fillRect(bx + (int)(cosf(a) * b.r * 0.62f), by + (int)(sinf(a) * b.r * 0.62f), 2, 2, UI_WHITE);
      gfx->fillCircle(bx - b.r * 45 / 100, by - b.r * 45 / 100, b.r / 7 + 1, UI_WHITE);
      gfx->fillRect(bx + b.r * 35 / 100, by + b.r * 35 / 100, 2, 2, rimIn);  // schwacher Glanz gegenueber
    }
  } else {
    float p = (800 - (int)left) / 800.0f;
    for (auto &b : bubbles) {
      int bx = b.x + (int)(sinf(t + b.ph) * 6);
      int by = b.y - 24;  // wo der Aufstieg endete
      if (p < 0.5f) gfx->drawCircle(bx, by, b.r + (int)(b.r * p * 0.8f), rim);
      for (int k = 0; k < 6; k++) {
        float a = k * 1.0472f + b.ph * 0.1f;
        float d = b.r * (0.9f + p * 1.6f);
        gfx->fillRect(bx + (int)(cosf(a) * d), by + (int)(sinf(a) * d), 2, 2, (k & 1) ? rimIn : UI_WHITE);
      }
    }
  }
}

// Kopfposition des Pokemon (fuer Herz und Z): x = Mitte des Sprites, y = Oberkante
// des Frames der gegebenen Aktion, so skaliert wie drawPmdActM() es zeichnet.
void petHeadAnchor(uint8_t actId, int &hx, int &hy) {
  if (!pmd.loaded) { hx = CX + 30; hy = PET_CY - 40; return; }
  const PmdAct &a = pmd.has(actId) ? pmd.acts[actId] : pmd.acts[PMD_IDLE];
  uint8_t s = pmd.acts[PMD_IDLE].h ? 170 / pmd.acts[PMD_IDLE].h : 5;
  if (s < 2) s = 2;
  if (s > 5) s = 5;
  while (s > 2 && a.h * s > 250) s--;
  hx = (int)beh.x;
  hy = PET_GROUND - (int)((a.base ? a.base : a.h) * s) + 6;
}

// Schnarchen: Z steigen schraeg vom Kopf auf und wachsen dabei; drei sind
// gleichzeitig unterwegs, zeitlich versetzt.
void drawSnoreB() {
  uint32_t now = millis();
  int hx, hy;
  petHeadAnchor(PMD_SLEEP, hx, hy);
  const uint32_t PERIOD = 2700;
  for (int i = 0; i < 3; i++) {
    float p = ((now + i * (PERIOD / 3)) % PERIOD) / (float)PERIOD;  // 0 am Kopf, 1 weg
    int x = hx + (int)(p * 56) + (int)(sinf(p * 9.0f + i) * 5);
    int y = hy - (int)(p * 78);
    uint8_t sz = p < 0.34f ? 2 : (p < 0.67f ? 3 : 4);
    if (p > 0.92f) continue;
    gfx->setTextSize(sz);
    gfx->setTextColor(INK_K);  // 1 px Schatten, damit es auf jedem Himmel lesbar ist
    gfx->setCursor(x + 1, y + 1);
    gfx->print("Z");
    gfx->setTextColor(UI_WHITE);
    gfx->setCursor(x, y);
    gfx->print("Z");
  }
}

// Ein Herz mit Rand und Glanzpunkt aus zwei Kreisen und einer Spitze.
void drawHeartShapeB(int x, int y, int r) {
  const uint16_t rim = C565(0x5a, 0x10, 0x28), fill = C565(0xff, 0x5c, 0x7c);
  for (int pass = 0; pass < 2; pass++) {
    int rr = pass ? r : r + 1;
    uint16_t c = pass ? fill : rim;
    gfx->fillCircle(x - r, y, rr, c);
    gfx->fillCircle(x + r, y, rr, c);
    gfx->fillTriangle(x - 2 * r - (pass ? 0 : 1), y + r / 3, x + 2 * r + (pass ? 0 : 1), y + r / 3,
                      x, y + 3 * r + (pass ? 0 : 1), c);
  }
  gfx->fillCircle(x - r - r / 3, y - r / 3, r / 4 + 1, UI_WHITE);
}

// Zuneigung: ein Herz steigt vom Kopf auf und wird dabei etwas groesser, ueber
// die ganze HEART_MS. Dauert das Herz laenger (Abschied), bleibt es beim Sprite.
// age = Millisekunden seit dem Start (0..HEART_MS), rise = Steighoehe in px.
void drawHeartRise(int hx, int hy, uint32_t age, int rise) {
  if (age >= HEART_MS) return;
  float p = age / (float)HEART_MS;  // 0 am Kopf, 1 weg
  int x = hx + 10 + (int)(sinf(p * 6.0f) * 7);
  int y = hy - (int)(p * rise);
  drawHeartShapeB(x, y, 3 + (int)(p * 3));
}

void drawHeartFloat(int hx, int hy) {
  uint32_t left = pet.heartLeftMs();
  if (left > HEART_MS) {  // kein normales Streicheln: wie in Optik A
    drawMap(SPR_HEART, 32, hx - 16, hy - 40, 2, false);
    return;
  }
  drawHeartRise(hx, hy, HEART_MS - left, 66);
}

// Das Futter wird vor dem Pokemon in EAT_BITES_B Bissen gegessen: jeder Biss
// nimmt ein rundes Stueck aus dem Symbol, es ruckelt kurz und Kruemel fliegen.
// Der Fortschritt kommt aus dem Essen-Timer des Pokemon, die Animation kann
// also nie laenger oder kuerzer als die Essen-Stimmung dauern.
#define EAT_BITES_B 4
void drawEatFoodB() {
  if (!pet.eating()) return;
  static const char *const *const ICONS[4] = { SPR_ICONB_FOOD, SPR_ICONB_BERRY_B,
                                               SPR_ICONB_BERRY_G, SPR_ICONB_CANDY };
  const char *const *ic = ICONS[eatItem < 4 ? eatItem : 0];
  uint32_t el = EAT_ANIM_MS - pet.eatLeftMs();
  static const uint16_t BITE_AT[EAT_BITES_B] = { 350, 900, 1450, 2000 };
  static const int8_t BITE_X[EAT_BITES_B] = { 13, 3, 12, 8 };
  static const int8_t BITE_Y[EAT_BITES_B] = { 5, 7, 12, 8 };
  static const uint8_t BITE_R[EAT_BITES_B] = { 5, 5, 6, 10 };
  uint8_t bites = 0;
  int jolt = 0;
  for (uint8_t k = 0; k < EAT_BITES_B; k++) {
    if (el >= BITE_AT[k]) {
      bites = k + 1;
      uint32_t since = el - BITE_AT[k];
      if (since < 160) jolt = (since < 80) ? 3 : 1;
    }
  }
  if (bites >= EAT_BITES_B) return;  // der letzte Biss hat es aufgegessen
  int cx = pmd.loaded ? (int)beh.x : CX;
  const int S = 4, x0 = cx - 8 * S, y0 = PET_GROUND - 88 + jolt;
  for (int r = 0; r < 16; r++)
    for (int c = 0; c < 16; c++) {
      char ch = ic[r][c];
      if (ch == '.') continue;
      bool gone = false;
      for (uint8_t k = 0; k < bites && !gone; k++) {
        int dx = c - BITE_X[k], dy = r - BITE_Y[k];
        gone = dx * dx + dy * dy <= BITE_R[k] * BITE_R[k];
      }
      if (!gone) gfx->fillRect(x0 + c * S, y0 + r * S, S, S, spriteColor(ch));
    }
  for (uint8_t k = 0; k < bites; k++) {
    uint32_t since = el - BITE_AT[k];
    if (since >= 380) continue;
    int bx = x0 + BITE_X[k] * S, by = y0 + BITE_Y[k] * S;
    for (int i = 0; i < 3; i++) {
      int dx = (i - 1) * (6 + (int)(since / 14)) + (BITE_X[k] > 8 ? 6 : -6);
      int dy = -4 + (int)(since * since / 3600) - i * 3;
      gfx->fillRect(bx + dx, by + dy, 3, 3, spriteColor(ic[8][8] == '.' ? 'w' : ic[8][8]));
    }
  }
}

void drawBath() {
  uint32_t now = millis();
  if (now > bathUntil) {
    bathUntil = 0;
    if (bathPending) {
      bathPending = false;
      pet.clean();
      // pose de alegria al quedar limpio
      if (pmd.has(PMD_POSE)) {
        beh.mode = 2;
        beh.act = PMD_POSE;
        beh.t0 = now;
        beh.until = now + pmdActTotalMs(pmd.acts[PMD_POSE]) * 2;
      }
    }
    return;
  }
  uint32_t left = bathUntil - now;
  if (LOOK_B) { drawBathB(now, left); return; }
  if (left > 800) {
    // espuma: pompas meciendose y subiendo poco a poco
    float t = now / 220.0f;
    for (auto &b : bubbles) {
      int bx = b.x + (int)(sinf(t + b.ph) * 6);
      int by = b.y - (int)((3000 - left) / 90);
      gfx->fillCircle(bx, by, b.r, UI_WHITE);
      gfx->drawCircle(bx, by, b.r, 0x7E3D);
      gfx->fillCircle(bx - b.r / 3, by - b.r / 3, b.r / 4, UI_BG_DAY);
    }
  } else {
    // las pompas revientan: destellos
    for (int i = 0; i < 8; i++) {
      auto &b = bubbles[i];
      int sx = b.x + (i % 3) * 6 - 6, sy = b.y - 18;
      uint16_t col = (i % 2) ? UI_BAR_WARN : UI_WHITE;
      gfx->fillRect(sx - 6, sy - 1, 13, 3, col);
      gfx->fillRect(sx - 1, sy - 6, 3, 13, col);
    }
  }
}

// ---------- mascota PMD: comportamiento ----------

uint32_t pmdActTotalMs(const PmdAct &a) {
  uint32_t t = 0;
  for (uint8_t i = 0; i < a.frames; i++) t += a.ms[i];
  return t ? t : 100;
}

uint8_t pmdFrameAt(const PmdAct &a, uint32_t t, bool loop) {
  uint32_t total = pmdActTotalMs(a);
  if (!loop && t >= total) return a.frames - 1;
  t %= total;
  uint8_t i = 0;
  while (t >= a.ms[i]) {
    t -= a.ms[i];
    i = (i + 1) % a.frames;
  }
  return i;
}

// dibuja una accion anclada por la base (centro-x, suelo) y devuelve su escala
// dibuja una accion de un PmdMon concreto (m); drawPmdAct usa el global pmd
void drawPmdActM(PmdMon &m, uint8_t actId, int cx, int groundY, uint32_t t, bool loop, bool sil, uint8_t maxS, bool flip) {
  const PmdAct &a = m.acts[actId];
  if (!a.frames) return;
  uint8_t sBase = m.acts[PMD_IDLE].h ? 170 / m.acts[PMD_IDLE].h : 5;
  if (sBase < 2) sBase = 2;
  if (sBase > maxS) sBase = maxS;
  uint8_t s = sBase;
  while (s > 2 && a.h * s > 250) s--;  // acciones con frame grande (ataque)
  uint8_t fi = pmdFrameAt(a, t, loop);
  const uint8_t *fr = a.data + (uint32_t)fi * a.w * a.h;
  // anclar por los pies (a.base), no por el alto del lienzo: asi las acciones
  // con padding distinto (Hurt, Eat...) quedan todas a la misma altura de suelo
  int x0 = cx - a.w * s / 2, y0 = groundY - (a.base ? a.base : a.h) * s;
  for (int r = 0; r < a.h; r++) {
    const uint8_t *row = fr + r * a.w;
    for (int c = 0; c < a.w; c++) {
      uint8_t idx = row[c];
      if (idx == 0xFF) continue;
      int cc = flip ? (a.w - 1 - c) : c;
      gfx->fillRect(x0 + cc * s, y0 + r * s, s, s, sil ? INK_K : m.pal[idx]);
    }
  }
}
void drawPmdAct(uint8_t actId, int cx, int groundY, uint32_t t, bool loop, bool sil, uint8_t maxS, bool flip) {
  drawPmdActM(pmd, actId, cx, groundY, t, loop, sil, maxS, flip);
}

// elige el siguiente capricho del bicho cuando esta contento
void behNext() {
  uint32_t now = millis();
  beh.t0 = now;
  int r = random(100);
  if (r < 35 && (pmd.has(PMD_WALKL) || pmd.has(PMD_WALKR))) {
    beh.mode = 1;  // paseo
    beh.targetX = 150 + random(176);
    beh.until = now + 15000;
  } else if (r < 60) {
    // gesto aleatorio entre los disponibles
    // (Hop fuera: salta demasiado alto; Sit fuera: mira hacia atras)
    static const uint8_t flair[] = { PMD_POSE, PMD_NOD, PMD_BREATH };
    uint8_t pick[3], n = 0;
    for (uint8_t f : flair)
      if (pmd.has(f)) pick[n++] = f;
    if (n) {
      beh.mode = 2;
      beh.act = pick[random(n)];
      beh.until = now + pmdActTotalMs(pmd.acts[beh.act]);
      return;
    }
    beh.mode = 0;
    beh.until = now + 2000 + random(3000);
  } else {
    beh.mode = 0;  // mirar al frente
    beh.until = now + 2000 + random(3000);
  }
}

void drawPetPMD() {
  uint32_t now = millis();

  if (pet.evolving()) {
    drawEvolveFX(now);
    return;
  }
  if (evoPmd.loaded) evoPmd.unload();  // termino la evolucion: libera la forma anterior

  PetMood m = pet.mood();
  uint8_t act;
  bool loop = true;
  if (m == MOOD_SLEEPING && pmd.has(PMD_SLEEP)) {
    act = PMD_SLEEP;
    beh.mode = 0;
  } else if (m == MOOD_EATING && pmd.has(PMD_EAT)) {
    act = PMD_EAT;
    beh.t0 = 0;
  } else if (m == MOOD_SAD && pmd.has(PMD_HURT)) {
    act = PMD_HURT;
  } else {
    // contento: el planificador decide (idle / paseo / gesto)
    if (now > beh.until) behNext();
    if (beh.mode == 1) {
      float d = beh.targetX - beh.x;
      if (fabsf(d) < 4) {
        behNext();
        act = PMD_IDLE;
      } else {
        beh.x += (d > 0 ? 3.0f : -3.0f);
        act = (d > 0) ? PMD_WALKR : PMD_WALKL;
      }
    } else {
      act = (beh.mode == 2) ? beh.act : PMD_IDLE;
      loop = false;
    }
    if (!pmd.has(act)) act = PMD_IDLE;
  }

  drawPmdAct(act, (int)beh.x, PET_GROUND, now - beh.t0, loop || act == PMD_IDLE, false, 5, false);

  if (pet.showHeart()) {
    if (LOOK_B) {
      int hx, hy;
      petHeadAnchor(PMD_IDLE, hx, hy);
      drawHeartFloat(hx, hy);
    } else {
      drawMap(SPR_HEART, 32, (int)beh.x + 50, PET_GROUND - 190, 2, false);
    }
  }
}

// sprite animado desde la SD: zoom entero por pixel, frames a su ritmo
void drawPetSD() {
  int s = mon.scale;
  int w = mon.w * s, h = mon.h * s;
  int x = CX - w / 2;
  int y = PET_CY - h / 2;

  bool sil = false;
  if (pet.evolving()) {
    sil = (millis() / 300) % 2;
  } else if (pet.mood() == MOOD_HAPPY && (millis() / 500) % 2) {
    y -= 6;  // saltito
  }

  uint16_t fm = mon.frameMs ? mon.frameMs : 100;
  uint16_t fi = (pet.sleeping || pet.napping()) ? 0 : (millis() / fm) % mon.frames;
  const uint8_t *fr = mon.data + (uint32_t)fi * mon.w * mon.h;
  for (int r = 0; r < mon.h; r++) {
    const uint8_t *row = fr + r * mon.w;
    for (int c = 0; c < mon.w; c++) {
      uint8_t idx = row[c];
      if (idx == 0xFF) continue;
      gfx->fillRect(x + c * s, y + r * s, s, s, sil ? INK_K : mon.pal[idx]);
    }
  }

  // emotes en vez de expresiones (los sprites importados no tienen anclas)
  if (pet.showHeart()) {
    if (LOOK_B) drawHeartFloat(x + w / 2, y);
    else drawMap(SPR_HEART, 32, x + w - 30, y - 50, 2, false);
  }
}

// ojo cerrado: borra el ojo 3x4 y dibuja el parpado
void overlayEye(const Species &sp, int x, int y, int s, int col) {
  gfx->fillRect(x + col * s, y + sp.eyeRow * s, 3 * s, 4 * s, sp.bodyColor);
  gfx->fillRect(x + col * s, y + (sp.eyeRow + 2) * s, 3 * s, s, INK_K);
}

// borra la sonrisa base y pinta boca abierta (comer) o ceno (triste)
void overlayMouth(const Species &sp, int x, int y, int s, bool open) {
  int mc = sp.mouthCol, mr = sp.mouthRow;
  gfx->fillRect(x + (mc - 3) * s, y + mr * s, 7 * s, 2 * s, sp.bodyColor);
  if (open) {
    gfx->fillRect(x + (mc - 2) * s, y + mr * s, 5 * s, 2 * s, INK_K);
  } else {
    gfx->fillRect(x + (mc - 2) * s, y + mr * s, 5 * s, s, INK_K);
    gfx->fillRect(x + (mc - 3) * s, y + (mr + 1) * s, s, s, INK_K);
    gfx->fillRect(x + (mc + 3) * s, y + (mr + 1) * s, s, s, INK_K);
  }
}

void drawPoops() {
  for (int i = 0; i < pet.poops; i++) {
    const int px = 36 + i * 46, py = 244;
    if (!LOOK_B) {
      drawMap(SPR_POOP, 32, px, py, 2, false);
      continue;
    }
    drawMap(SPR_POOP_B, 32, px, py, 2, false);
    // zwei Fliegen kreisen auf versetzten, unrunden Bahnen um jedes Haeufchen
    const uint32_t t = millis();
    for (int f = 0; f < 2; f++) {
      float a = t * (f ? -0.0071f : 0.0053f) + i * 2.1f + f * 3.0f;
      int fx = px + 32 + (int)(cosf(a) * (24 + f * 6));
      int fy = py + 24 + (int)(sinf(a * 1.7f) * (14 + f * 4));
      bool up = ((t / 70 + f * 3 + i) & 1) != 0;  // Fluegelschlag
      gfx->fillRect(fx, fy, 3, 3, INK_K);
      gfx->fillRect(fx - 1, fy - (up ? 2 : 1), 2, 2, UI_WHITE);
      gfx->fillRect(fx + 2, fy - (up ? 2 : 1), 2, 2, UI_WHITE);
    }
  }
}

void drawBars() {
  drawBar(78, 318, T(S_BAR_FOOD), pet.fullness);
  drawBar(244, 318, T(S_BAR_JOY), pet.joy);
  int y2 = LOOK_B ? 340 : 346;  // Optik B: zweite Reihe naeher dran, die Kacheln sind groesser
  drawBar(78, y2, T(S_BAR_ENE), pet.energy);
  drawBar(244, y2, T(S_BAR_HYG), pet.hygiene);
}

void drawBar(int x, int y, const char *label, uint8_t val) {
  gfx->setTextColor(inkColor());
  gfx->setTextSize(2);
  gfx->setCursor(x, y);
  gfx->print(label);
  int bx = x + 48, bw = 100, bh = 15;  // +48: deja sitio a etiquetas de 4 letras (EN)
  uint16_t fill = (val >= 50) ? UI_BAR_OK : (val >= 25) ? UI_BAR_WARN : UI_BAR_BAD;
  gfx->fillRoundRect(bx, y, bw, bh, 4, UI_TRACK);
  int fw = (bw - 4) * val / 100;
  if (val > 0 && fw < 2) fw = 2;  // un valor >0 nunca debe verse igual de vacio que 0
  if (fw > 0) gfx->fillRoundRect(bx + 2, y + 2, fw, bh - 4, 3, fill);
}

void drawButtons() {
  // Optik B: weiche Pastellkachel je Symbol, Schatten, Glanz und Ring in der
  // Akzentfarbe, Symbol x3. Reihenfolge/Funktion wie in A.
  const uint16_t tint[4]   = { C565(255, 230, 224), C565(226, 240, 220),
                               C565(255, 244, 204), C565(220, 238, 255) };
  const uint16_t accent[4] = { C565(232, 110, 96), C565(110, 180, 100),
                               C565(232, 180, 60), C565(90, 160, 220) };
  for (int i = 0; i < 4; i++) {
    bool resting = pet.sleeping || pet.napping();
    bool off = resting && i != 2;  // descansando solo funciona LUZ/NAP
    if (LOOK_B) {
      int bx = buttonsB[i].cx - BTN_HALF_B, by = buttonsB[i].cy - BTN_HALF_B;
      int bs = 2 * BTN_HALF_B;
      if (!resting) {
        gfx->fillRoundRect(bx + 1, by + 4, bs, bs, 14, gNight ? C565(8, 10, 20) : C565(150, 144, 128));
        gfx->fillRoundRect(bx, by, bs, bs, 14, tint[i]);
        gfx->fillRoundRect(bx + 4, by + 3, bs - 8, bs / 2 - 4, 10, UI_WHITE);  // Glanz
        gfx->drawRoundRect(bx + 1, by + 1, bs - 2, bs - 2, 13, accent[i]);
      }
      gfx->drawRoundRect(bx, by, bs, bs, 14, inkColor());
      if (!off) drawMap(buttonsB[i].icon, 16, buttonsB[i].cx - 24, buttonsB[i].cy - 24, 3, false);
      continue;
    }
    int bx = buttons[i].cx - BTN_HALF, by = buttons[i].cy - BTN_HALF;
    if (!resting) gfx->fillRoundRect(bx, by, 2 * BTN_HALF, 2 * BTN_HALF, 14, UI_WHITE);
    gfx->drawRoundRect(bx, by, 2 * BTN_HALF, 2 * BTN_HALF, 14, inkColor());
    if (!off) drawMap(buttons[i].icon, 16, buttons[i].cx - 16, buttons[i].cy - 16, 2, false);
  }
}

const char *eggMsg() {
  uint8_t t = pet.eggCracks();
  if (t < 5) return T(S_EGG_TOUCH);
  if (t < 10) return T(S_EGG_MOVES);
  return T(S_EGG_ALMOST);
}

const char *statusMsg() {
  if (millis() < sleepMsgUntil) return T(sleepMsgId);
  if (pet.evolving()) return T(S_EVOLVING);
  if (bathUntil) return "Splish splash!";  // onomatopeya universal
  if (pet.napping()) {
    static char buf[24];
    snprintf(buf, sizeof(buf), "%s %lus", T(S_NAPPING), (unsigned long)pet.napSecondsLeft());
    return buf;
  }
  if (pet.sleeping) return pet.lightsOut ? T(S_DREAMS_FMT) : T(S_BAD_DREAMS_FMT);
  if (pet.eating()) return T(S_EATING);
  if (pet.showHeart()) return T(S_LIKES);
  if (pet.fullness < 25) return T(S_HUNGRY);
  if (pet.hygiene < 25) return T(S_NEEDS_BATH);
  if (pet.energy < 25) return T(S_EXHAUSTED);
  if (pet.joy < 25) return T(S_SAD);
  if (pet.weight >= OVERWEIGHT_FROM) return T(S_CHUBBY);
  if (pet.shiny && pet.ageMinutes < 15) return T(S_IS_SHINY);
  return T(S_HAPPY);
}

// dibuja un mapa de n x n pixeles escalado; silhouette=true lo pinta en tinta
void drawMap(const char *const *map, int n, int x, int y, int s, bool silhouette) {
  for (int r = 0; r < n; r++) {
    for (int c = 0; c < n; c++) {
      char ch = map[r][c];
      if (ch == '.') continue;
      gfx->fillRect(x + c * s, y + r * s, s, s, silhouette ? INK_K : spriteColor(ch));
    }
  }
}

// como drawMap() con silhouette=true, pero con un color propio en vez de
// INK_K -- para no afectar a las siluetas de especies no descubiertas
void drawMapTint(const char *const *map, int n, int x, int y, int s, uint16_t color) {
  for (int r = 0; r < n; r++) {
    for (int c = 0; c < n; c++) {
      char ch = map[r][c];
      if (ch == '.') continue;
      gfx->fillRect(x + c * s, y + r * s, s, s, color);
    }
  }
}
