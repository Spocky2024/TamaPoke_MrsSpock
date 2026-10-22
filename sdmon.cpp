#include "sdmon.h"
#include "pin_config.h"
#include <FS.h>
#include <SD_MMC.h>

bool sdReady = false;
bool sdDirty = false;
SdThumbs thumbs;

// Ordner /mons lesbar? Unterscheidet "Sprite-Datei gibt es nicht" (Karte gesund)
// von "Karte antwortet nicht" (Fehlerzustand, Neueinbinden lohnt sich).
static bool sdFolderOk() {
  File d = SD_MMC.open("/mons");
  bool ok = d && d.isDirectory();
  if (d) d.close();
  return ok;
}

void sdDiag(const char *what, uint16_t dex, int attempt, const char *why) {
  Serial.printf("[SD] %s dex=%u Versuch %d: %s | sdReady=%d | PSRAM frei %u KB (groesster Block %u KB) | Heap frei %u KB (min %u KB, groesster Block %u KB)\n",
                what, dex, attempt, why, sdReady ? 1 : 0,
                (unsigned)(ESP.getFreePsram() / 1024), (unsigned)(ESP.getMaxAllocPsram() / 1024),
                (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMinFreeHeap() / 1024),
                (unsigned)(ESP.getMaxAllocHeap() / 1024));
}

// Ein einzelner Ladeversuch. Rueckgabe: 0 ok, 1 Datei fehlt (Karte gesund -> nicht wiederholen),
// 2 SD-/Lesefehler (wiederholen), 3 kaputte Daten (nicht wiederholen), 4 kein PSRAM (nicht wiederholen)
int PmdMon::loadTry(uint16_t dexNum, bool shiny, const char **why) {
  unload();
  *why = "";

  char path[28];
  snprintf(path, sizeof(path), "/mons/p%s%03u.bin", shiny ? "s" : "", dexNum);
  File f = SD_MMC.open(path, FILE_READ);
  if (!f && shiny) {  // sin shiny PMD: usa el normal
    snprintf(path, sizeof(path), "/mons/p%03u.bin", dexNum);
    f = SD_MMC.open(path, FILE_READ);
  }
  if (!f) {
    if (sdFolderOk() && !SD_MMC.exists(path)) { *why = "Datei fehlt"; return 1; }
    *why = "open fehlgeschlagen, /mons nicht lesbar";
    return 2;
  }

  uint32_t size = f.size();
  if (size < 7 || size > 3UL * 1024 * 1024) { f.close(); *why = "Dateigroesse unplausibel"; return 3; }
  blob = (uint8_t *)ps_malloc(size);
  if (!blob) { f.close(); *why = "ps_malloc fehlgeschlagen"; return 4; }
  if (f.read(blob, size) != size) {
    free(blob); blob = nullptr;
    f.close();
    *why = "Lesefehler (zu wenig Bytes)";
    return 2;
  }
  if (memcmp(blob, "TPK2", 4) != 0) {
    free(blob); blob = nullptr;
    f.close();
    *why = "Kennung TPK2 fehlt";
    return 3;
  }
  f.close();

  uint8_t nActs = blob[4];
  memcpy(&palCount, blob + 5, 2);
  if (palCount > 256 || (uint32_t)7 + palCount * 2 > size) { unload(); *why = "Palette ungueltig"; return 3; }
  memcpy(pal, blob + 7, palCount * 2);

  const uint8_t *p = blob + 7 + palCount * 2;
  const uint8_t *end = blob + size;
  for (uint8_t i = 0; i < nActs && p + 4 <= end; i++) {
    uint8_t id = p[0], w = p[1], h = p[2], nf = p[3];
    p += 4;
    if (id >= PMD_NACTS || nf > 24) { unload(); *why = "Aktion ungueltig"; return 3; }
    // valida que ms[] y los datos del frame caben en el blob (archivo truncado)
    uint32_t bytes = (uint32_t)nf * 2 + (uint32_t)w * h * nf;
    if (w == 0 || h == 0 || nf == 0 || p + bytes > end) { unload(); *why = "Daten abgeschnitten"; return 3; }
    PmdAct &a = acts[id];
    a.w = w;
    a.h = h;
    a.frames = nf;
    for (uint8_t k = 0; k < nf; k++) {
      a.ms[k] = p[0] | (p[1] << 8);
      p += 2;
    }
    a.data = p;
    p += (uint32_t)w * h * nf;
    // fila mas baja con contenido en cualquier frame: anclar por los pies
    uint8_t base = 1;
    for (uint8_t f = 0; f < nf; f++) {
      const uint8_t *fr = a.data + (uint32_t)f * w * h;
      for (int r = h - 1; r >= 0; r--) {
        bool any = false;
        for (int c = 0; c < w && !any; c++)
          if (fr[r * w + c] != 0xFF) any = true;
        if (any) { if (r + 1 > base) base = r + 1; break; }
      }
    }
    a.base = base;
  }
  loaded = true;
  Serial.printf("cargado %s (%u KB)\n", path, size / 1024);
  return 0;
}

// Laden mit Wiederholung (seit 6.72): Bei einem SD-/Lesefehler zweiter Versuch nach kurzer Pause,
// danach SD einmal neu einbinden und ein dritter Versuch. Fehlende Dateien bei gesunder Karte
// (z.B. Generation nicht installiert) werden NICHT wiederholt.
bool PmdMon::load(uint16_t dexNum, bool shiny) {
  unload();
  if (!sdReady) {
    // SD beim Start nicht erkannt oder spaeter ausgefallen: hoechstens alle 30 s neu versuchen
    if (!sdRemount(30000UL) || !sdReady) return false;
  }
  for (int attempt = 1; attempt <= 3; attempt++) {
    const char *why = "";
    int rc = loadTry(dexNum, shiny, &why);
    if (rc == 0) {
      if (attempt > 1) Serial.printf("[SD] dex=%u geladen nach Versuch %d\n", dexNum, attempt);
      return true;
    }
    sdDiag("PMD laden fehlgeschlagen", dexNum, attempt, why);
    if (rc != 2) return false;  // fehlende Datei / kaputte Daten / kein PSRAM: Wiederholen bringt nichts
    if (attempt == 1) delay(40);
    else if (attempt == 2) sdRemount(8000UL);
  }
  return false;
}

void PmdMon::unload() {
  if (blob) {
    free(blob);
    blob = nullptr;
  }
  for (auto &a : acts) {
    a.w = a.h = a.frames = a.base = 0;
    a.data = nullptr;
  }
  loaded = false;
}

bool SdThumbs::load() {
  if (!sdReady) return false;
  File f = SD_MMC.open("/mons/thumbs.bin", FILE_READ);
  if (!f) {
    Serial.println("sin thumbs.bin (galeria sin miniaturas)");
    return false;
  }
  uint32_t size = f.size();
  data = (uint8_t *)ps_malloc(size);
  if (!data || f.read(data, size) != size || memcmp(data, "TPTH", 4) != 0) {
    Serial.println("thumbs.bin invalido");
    if (data) { free(data); data = nullptr; }
    f.close();
    return false;
  }
  f.close();
  memcpy(&count, data + 4, 2);
  loaded = true;
  Serial.printf("miniaturas cargadas: %u (%u KB)\n", count, size / 1024);
  return true;
}

const uint8_t *SdThumbs::get(int16_t dex) const {
  if (!loaded || dex < 1 || dex > count) return nullptr;
  uint32_t off;
  memcpy(&off, data + 6 + 4 * (dex - 1), 4);
  return data + off;
}

static uint32_t sdLastMountTry = 0;
static bool sdMountTried = false;

bool sdBegin() {
  sdLastMountTry = millis();
  sdMountTried = true;
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  // seit 6.73: NIE automatisch formatieren. Frueher stand hier format_if_mount_failed=true: ein kurzer
  // Kontaktfehler beim Einschalten haette die Karte formatiert und alle Sprites geloescht. Eine leere/
  // unlesbare Karte wird nur noch auf ausdruecklichen Wunsch formatiert (USB-Befehl SDFORMAT, siehe
  // sdSerialCommand; der Web-Installer fragt vorher nach).
  sdReady = SD_MMC.begin("/sdcard", true /* modo 1-bit */, false /* nie formatieren */);
  if (sdReady) {
    Serial.printf("SD montada: %llu MB\n", SD_MMC.cardSize() / (1024ULL * 1024ULL));
    SD_MMC.mkdir("/mons");
  } else {
    Serial.println("SD nicht lesbar (nicht eingesteckt, leer oder unbekanntes Format). Es wird NICHT automatisch formatiert;"
                   " neue Karte: Web-Installer (fragt nach) oder USB-Befehl SDFORMAT.");
  }
  return sdReady;
}

// SD neu einbinden (seit 6.72). Formatiert NIEMALS: ein kurzer Kontaktfehler darf nicht alle
// Sprites loeschen. Die Miniaturen (thumbs.bin) werden nachgeladen, falls sie beim Start fehlten.
bool sdRemount(uint32_t minGapMs) {
  uint32_t now = millis();
  if (sdMountTried && now - sdLastMountTry < minGapMs) return sdReady;
  sdMountTried = true;
  sdLastMountTry = now;
  Serial.println("[SD] binde SD neu ein ...");
  SD_MMC.end();
  delay(30);
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  sdReady = SD_MMC.begin("/sdcard", true /* 1-bit */, false /* nie formatieren */);
  Serial.println(sdReady ? "[SD] neu eingebunden: ok" : "[SD] neu einbinden fehlgeschlagen");
  if (sdReady) SD_MMC.mkdir("/mons");
  if (sdReady && !thumbs.loaded) thumbs.load();
  return sdReady;
}

// Formatieren NUR auf ausdruecklichen Wunsch (USB-Befehl SDFORMAT). Eine bereits lesbare Karte wird nie
// angefasst: formatiert wird nur, wenn das Einbinden scheitert (leere/unlesbare Karte).
bool sdFormatNow() {
  if (sdReady) return true;  // lesbar: nichts zu tun, nichts wird geloescht
  Serial.println("[SD] formatiere SD (auf Wunsch) ...");
  SD_MMC.end();
  delay(30);
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  sdReady = SD_MMC.begin("/sdcard", true /* 1-bit */, true /* formatieren, falls Einbinden scheitert */);
  sdLastMountTry = millis();
  sdMountTried = true;
  if (sdReady) {
    SD_MMC.mkdir("/mons");
    if (!thumbs.loaded) thumbs.load();
    sdDirty = true;
    Serial.println("[SD] formatiert und eingebunden");
  } else {
    Serial.println("[SD] formatieren fehlgeschlagen (Karte eingesteckt?)");
  }
  return sdReady;
}

bool SdMon::load(uint16_t dexNum, bool shiny) {
  unload();
  if (!sdReady) return false;

  char path[24];
  snprintf(path, sizeof(path), "/mons/%s%03u.bin", shiny ? "s" : "", dexNum);
  File f = SD_MMC.open(path, FILE_READ);
  if (!f && shiny) {  // sin variante shiny: usa la normal
    snprintf(path, sizeof(path), "/mons/%03u.bin", dexNum);
    f = SD_MMC.open(path, FILE_READ);
  }
  if (!f) {
    Serial.printf("no existe %s\n", path);
    return false;
  }

  char magic[4];
  uint16_t header[4];
  if (f.read((uint8_t *)magic, 4) != 4 || memcmp(magic, "TPK1", 4) != 0 ||
      f.read((uint8_t *)header, 8) != 8) {
    f.close();
    return false;
  }
  w = header[0];
  h = header[1];
  frames = header[2];
  frameMs = header[3];
  // acota dimensiones: evita size desbordado o absurdo con archivo corrupto
  if (f.read((uint8_t *)&palCount, 2) != 2 || palCount > 256 ||
      w == 0 || w > 256 || h == 0 || h > 256 || frames == 0 || frames > 64) {
    f.close();
    return false;
  }
  if (f.read((uint8_t *)pal, palCount * 2) != palCount * 2) {
    f.close();
    return false;
  }

  uint32_t size = (uint32_t)w * h * frames;
  data = (uint8_t *)ps_malloc(size);
  if (!data) {
    Serial.println("sin PSRAM para el sprite");
    f.close();
    return false;
  }
  uint32_t got = f.read(data, size);
  f.close();
  if (got != size) {
    Serial.printf("%s truncado (%u de %u)\n", path, got, size);
    unload();
    return false;
  }

  // zoom entero para que el bicho mida ~200 px de alto en pantalla
  scale = 200 / h;
  if (scale < 2) scale = 2;
  if (scale > 5) scale = 5;

  Serial.printf("cargado %s: %ux%u x%u frames @%ums, escala %u\n",
                path, w, h, frames, frameMs, scale);
  loaded = true;
  return true;
}

void SdMon::unload() {
  if (data) {
    free(data);
    data = nullptr;
  }
  loaded = false;
}

// ---------------------------------------------------------------------------
// Protocolo de carga por USB (para llenar la SD sin sacarla de la placa):
//   PUT <ruta> <bytes>\n  + datos crudos   -> "OK" ... "DONE"
//   LS\n                                   -> listado de /mons
// Usar con tools/send_sd.py
// ---------------------------------------------------------------------------

bool sdSerialCommand(const String &line) {
  if (line.startsWith("PUT ")) {
    int sp = line.lastIndexOf(' ');
    String path = line.substring(4, sp);
    uint32_t size = line.substring(sp + 1).toInt();
    if (!sdReady) sdRemount(0);  // seit 6.73: erst (ohne Formatieren) neu einbinden
    if (!sdReady || size == 0 || size > 4 * 1024 * 1024) {
      Serial.println("ERR");
      return true;
    }
    if (!path.startsWith("/")) path = "/" + path;
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) {
      Serial.println("ERR");
      return true;
    }
    Serial.println("OK");
    static uint8_t buf[2048];
    uint32_t remaining = size;
    Serial.setTimeout(5000);
    while (remaining > 0) {
      size_t want = remaining > sizeof(buf) ? sizeof(buf) : remaining;
      size_t n = Serial.readBytes(buf, want);
      if (n == 0) break;  // timeout
      f.write(buf, n);
      remaining -= n;
      Serial.println("#");  // ack: listo para el siguiente bloque
    }
    f.close();
    Serial.setTimeout(1000);
    sdDirty = (remaining == 0);
    Serial.println(remaining == 0 ? "DONE" : "ERR");
    return true;
  } else if (line == "SDSTAT") {  // seit 6.73: ist die SD lesbar? (versucht vorher ein Neueinbinden, ohne Formatieren)
    if (!sdReady) sdRemount(0);
    Serial.println(sdReady ? "OK" : "ERR");
    return true;
  } else if (line == "SDFORMAT") {  // seit 6.73: leere/unlesbare SD auf Wunsch formatieren
    Serial.println(sdFormatNow() ? "OK" : "ERR");
    return true;
  } else if (line == "LS") {
    File dir = SD_MMC.open("/mons");
    if (dir) {
      File e;
      while ((e = dir.openNextFile())) {
        Serial.printf("%s %u\n", e.name(), (uint32_t)e.size());
        e.close();
      }
      dir.close();
    }
    Serial.println("DONE");
    return true;
  }
  return false;
}
