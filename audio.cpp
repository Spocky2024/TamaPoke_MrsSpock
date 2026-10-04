#include "audio.h"
#include "pin_config.h"
#include <Arduino.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include <Preferences.h>

// ---------------------------------------------------------------------------
// Audio del TamaPoke: códec ES8311 (DAC -> amplificador PA -> altavoz) por I2S.
// Init del ES8311 portado del driver oficial de Espressif (esp-bsp), fijado a
// MCLK=4.096MHz (256*fs), 16kHz, 16-bit, esclavo I2S. Los efectos son tonos
// cuadrados (estilo Game Boy) sintetizados en una tarea aparte para no
// bloquear el loop de juego.
// ---------------------------------------------------------------------------

#define ES8311_ADDR 0x18
#define SAMPLE_RATE 16000

static I2SClass i2s;
static bool gReady = false;
static bool gOn = true;
static QueueHandle_t gQ = nullptr;

// ---- I2C del códec ----
static bool esW(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}
static uint8_t esR(uint8_t reg) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(ES8311_ADDR, 1);
  return Wire.available() ? Wire.read() : 0;
}

// Secuencia de init VERIFICADA EN ESTA PLACA (proyecto PlaneRadar2.0, misma
// Waveshare 1.75). Clave: reloj DERIVADO DEL BCLK (reg01=0xBF, sin MCLK externo)
// y referencia interna que alimenta el DAC (reg44=0x58); sin esos dos el codec
// respondia por I2C pero no salia audio. 16kHz, 16-bit, esclavo I2S.
static bool es8311Init() {
  Wire.beginTransmission(ES8311_ADDR);
  if (Wire.endTransmission() != 0) return false;

  // open()
  esW(0x0D, 0xFA); esW(0x44, 0x08); esW(0x44, 0x08);  // power up + quirk de 1a escritura
  esW(0x01, 0x30); esW(0x02, 0x00); esW(0x03, 0x10); esW(0x16, 0x24);
  esW(0x04, 0x10); esW(0x05, 0x00); esW(0x0B, 0x00); esW(0x0C, 0x00);
  esW(0x10, 0x1F); esW(0x11, 0x7F);
  esW(0x00, 0x80); esW(0x00, 0x80);                   // reset clock, esclavo
  esW(0x01, 0xBF);                                    // clk src = BCLK (sin MCLK externo)
  { uint8_t r = esR(0x06); r &= ~0x20; esW(0x06, r); }  // SCLK no invertido
  esW(0x13, 0x10); esW(0x1B, 0x0A); esW(0x1C, 0x6A);
  esW(0x44, 0x58);                                    // referencia interna -> alimenta el DAC

  // config_sample(): BCLK*8 = DIG_MCLK
  esW(0x02, 0x18); esW(0x05, 0x00); esW(0x03, 0x10); esW(0x04, 0x20);
  { uint8_t r = esR(0x07); r &= 0xC0; esW(0x07, r); }
  esW(0x08, 0xFF);
  { uint8_t r = esR(0x06); r &= 0xE0; r |= 0x03; esW(0x06, r); }  // bclk_div=4

  // formato I2S 16-bit
  esW(0x09, 0x0C); esW(0x0A, 0x0C);

  // start() DAC esclavo
  esW(0x00, 0x80); esW(0x01, 0xBF); esW(0x09, 0x0C); esW(0x0A, 0x0C);
  esW(0x17, 0xBF); esW(0x0E, 0x02); esW(0x12, 0x00); esW(0x14, 0x1A);
  esW(0x0D, 0x01); esW(0x15, 0x40); esW(0x37, 0x08); esW(0x45, 0x00);

  // volumen + unmute
  // reg 0x32 = 0.5dB/paso, 0xBF=0dB (ver datasheet ES8311). -10dB ~ mitad de
  // volumen percibido (convencion habitual en audio) -> 0xBF - 20 = 0xAB
  esW(0x32, 0xAB);                                    // volumen DAC -10 dB (mitad)
  { uint8_t r = esR(0x31); r &= 0x9F; esW(0x31, r); }  // unmute
  return true;
}

// ---- sintetizador de tono cuadrado ----
struct Note { uint16_t f, ms; };

static const Note N_EAT[]    = {{660, 90}, {0, 20}, {660, 90}};
static const Note N_PLAY[]   = {{784, 70}, {988, 130}};
static const Note N_HEART[]  = {{1047, 90}, {1319, 130}};
static const Note N_HATCH[]  = {{523, 80}, {659, 80}, {784, 110}, {1047, 170}};
static const Note N_EVOLVE[] = {{523, 80}, {659, 80}, {784, 80}, {1047, 90}, {1319, 230}};
static const Note N_MEDAL[]  = {{784, 70}, {0, 25}, {784, 70}, {0, 25}, {1047, 200}};
static const Note N_DENY[]   = {{300, 110}, {200, 170}};
static const Note N_BYE[]    = {{784, 150}, {659, 150}, {523, 280}};
static const Note N_LEVEL[]  = {{784, 70}, {1047, 130}};
static const Note N_HIT_STRONG[]  = {{200, 160}, {150, 130}};    // golpe fuerte: grave, ahora mas largo
static const Note N_HIT_WEAK[]    = {{550, 130}, {700, 130}};    // golpe flojo: agudo, dos notas
static const Note N_MISS[]        = {{1500, 130}, {1900, 130}};  // fallo: corto y claro/agudo
static const Note N_CLEAN[]       = {{1568, 90}, {1976, 110}, {0, 30}, {1760, 100}};  // adaptado de
                                       // SFX_EVENT_SPARKLE de la otra base de codigo
                                       // (sin la rafaga de ruido, esta engine solo
                                       // hace onda cuadrada pura)
static const Note N_TRAIN[]       = {{150, 90}, {0, 20}, {150, 90}};  // lo mas grave
                                       // posible: 150Hz es la frecuencia mas baja
                                       // ya probada con exito en este proyecto
                                       // (ver N_HIT_STRONG)
static const Note N_BATTLE_WIN[]  = {{523, 70}, {659, 70}, {784, 70}, {0, 15},
                                      {784, 70}, {1047, 70}, {1568, 260}};  // fanfarria alegre
static const Note N_BATTLE_LOSE[] = {{392, 140}, {330, 140}, {262, 280}};  // triste, descendente

struct SfxDef { const Note *n; uint8_t len; };
static const SfxDef SFX[SFX_COUNT] = {
  {N_EAT, 3}, {N_PLAY, 2}, {N_HEART, 2}, {N_HATCH, 4},
  {N_EVOLVE, 5}, {N_MEDAL, 5}, {N_DENY, 2}, {N_BYE, 3}, {N_LEVEL, 2},
  {N_HIT_STRONG, 2}, {N_HIT_WEAK, 2}, {N_MISS, 2}, {N_BATTLE_WIN, 7}, {N_BATTLE_LOSE, 3},
  {N_CLEAN, 4},
  {N_TRAIN, 3},
};

static int16_t buf[256 * 2];  // estéreo intercalado (L=R)

// reproduce un tono (o silencio si f==0) con rampa de ataque/caida anti-click
static void playTone(uint16_t f, uint16_t ms) {
  int total = SAMPLE_RATE * ms / 1000;
  int half = f ? (SAMPLE_RATE / (2 * f)) : 0;  // medio periodo en muestras
  const int16_t amp = 5000;
  int phase = 0, done = 0;
  bool high = true;
  while (done < total) {
    int n = total - done; if (n > 256) n = 256;
    for (int i = 0; i < n; i++) {
      int16_t s = 0;
      if (f) {
        s = high ? amp : -amp;
        int idx = done + i;
        if (idx < 64) s = (int16_t)(s * idx / 64);                 // ataque
        else if (idx > total - 96) s = (int16_t)(s * (total - idx) / 96);  // caida
        if (++phase >= half) { phase = 0; high = !high; }
      }
      buf[i * 2] = s; buf[i * 2 + 1] = s;
    }
    i2s.write((uint8_t *)buf, n * 4);
    done += n;
  }
}

static void audioTask(void *) {
  uint8_t id;
  for (;;) {
    if (xQueueReceive(gQ, &id, portMAX_DELAY) && gOn && gReady && id < SFX_COUNT) {
      digitalWrite(PA, HIGH);  // enciende el amplificador
      delay(25);               // deja que arranque DE VERDAD antes del primer
                                // tono: con solo 8ms, un sonido de una sola nota
                                // (SFX_TAP, ya eliminado por no lograrse que
                                // sonara con fiabilidad en este altavoz) caia
                                // casi entero dentro de la rampa de subida del
                                // ampli y sonaba practicamente inaudible; en
                                // sonidos de varias notas el efecto se
                                // disimulaba solo
      const SfxDef &d = SFX[id];
      for (uint8_t i = 0; i < d.len; i++) playTone(d.n[i].f, d.n[i].ms);
      delay(60);               // deja salir la cola del DMA antes de cortar
      digitalWrite(PA, LOW);   // apaga el amp entre sonidos (evita siseo)
    }
  }
}

void audioBegin() {
  // I2S primero: arranca el MCLK que necesita el códec para engancharse
  pinMode(PA, OUTPUT);
  digitalWrite(PA, LOW);   // amp apagado; la tarea lo enciende al reproducir

  i2s.setPins(I2S_BCK_IO, I2S_WS_IO, I2S_DO_IO, I2S_DI_IO, I2S_MCK_IO);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("I2S begin fallo");
    return;
  }
  if (!es8311Init()) { Serial.println("ES8311 no responde (audio off)"); return; }

  Preferences p;
  p.begin("tamapoke", true);
  gOn = p.getBool("snd", true);
  p.end();

  gReady = true;
  gQ = xQueueCreate(8, sizeof(uint8_t));
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 1, nullptr, 0);
  sfxPlay(SFX_CLEAN);  // jingle de arranque -- tono del bano/limpieza, mas discreto que el de eclosion
}

void sfxPlay(uint8_t id) {
  if (gReady && gOn && gQ) xQueueSend(gQ, &id, 0);  // descarta si la cola esta llena
}

// vacia la cola de golpe (p.ej. al eclosionar: no debe esperar a que
// terminen de sonar los toques del huevo acumulados por tocar muy rapido)
void sfxFlush() {
  if (gQ) xQueueReset(gQ);
}

void audioSetEnabled(bool on) {
  gOn = on;
  Preferences p;
  p.begin("tamapoke", false);
  p.putBool("snd", on);
  p.end();
}
bool audioEnabled() { return gOn; }
