#include "i18n.h"
#include "pet.h"        // MED_COUNT
#include "dex.h"        // TYPE_COUNT
#include <Preferences.h>

Lang gLang = LANG_DEFAULT;

// Tabla de cadenas [idioma][id]. Sin acentos ni enes: la fuente bitmap del
// firmware no los tiene (por eso el espanol ya iba "Esta", "bano", etc.).
// Solo EN y DE (resto de idiomas retirados a peticion del usuario).
static const char *const STRINGS[TEXT_LANGS][STR_COUNT] = {
  // ---------------- EN ----------------
  {
    "Evolving!", "Yum yum!", "It likes it!", "It's hungry!", "Needs a bath!",
    "Worn out...", "Feeling sad...", "A bit chubby...", "It's SHINY!!", "It's happy",
    "Lights out", "Lights on", "Power-Nap", "Not yet.", "still digesting...", "still dripping...", "too wired...", "Power nap:", "Not hungry.", "roly-poly...", "It's sleeping...",
    "THANKS! Farewell", "It ran away...", "Bye! Waving goodbye.",
    "EGG", "Legendary egg!?", "Rare egg!", "Tap the egg...", "It moves!", "Almost there!",
    "POKEDEX %u/%u",
    "%s%s Lv.%u",
    "Release %s?", "YES", "NO",
    "%u HITS", "ATK +%u", "NEW RECORD!", "BEST: %u", "HIT FAST!",
    "ATK max", "SPD +%u", "SPD max", "DEF +%u", "DEF max", "%u/10 Consecutive Wins", "CHAMPION",
    "SCORE: %u", "So much fun!", "You can do better!",
    "SET TIME", "HOUR", "MIN", "swipe: cancel", "Lang",
    "saved", "TamaPoke", "Version", "Mrs. Spock", "Germany",
    "MEDAL!", "AWESOME!", "%u DAY STREAK!",
    "Milestone", "%s Steps", "Streak Record", "%u Days", "New stage!",
    "STREAK %u  best %u", "BOND", "BERRY ???", "RED BERRY", "BLUE BERRY", "GREEN BERRY",
    "%s   AGE %lu days", "%s   AGE %lu day", "tap name: rename",
    "BATTLE", "ATK", "DEF", "SPD", "WEIGHT", "TRAINING",
    "You won!", "You lost.",
    "Type:", "Meetings:", "Win rate:", "raised:",
    "agile", "normal", "lazy", "fat",
    "MEDALS %d/%d",
    "NAME:", "tap to go back",
    "FOOD", "JOY", "ENE", "HYG",
    "PROGRESS", "Lv.%u", "%u min to Lv.%u", "EVOLUTION", "Final form",
    "Ready to evolve!", "Feeling sad...",
    "%u levels to go", "Care Miss: %u",
    "SND ON", "SND OFF",
    "EVOLVE!", "%s wants to tell you...", "%s feels abandoned...",
    "Evolve?", "Keep form", "Say goodbye?", "Goodbye", "Stay together",
    "Choose your starter",
    "No sprites", "Load them onto the SD",
    "TRIP", "STEPS", "End Trip", "Total:", "Sensor not found", "Really?",
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
    "Go on a trip", "Today:", "Magic Berry:", "Eat Magic Berry",
    "Didn't see anyone", "next", "+1 Magic Berry!", "You met %s!", "NEW",
    "Magic Berry lost.", "Pockets are full.",
    "Hunger", "Joy", "Energy", "Hygiene",
    "Not hungry anymore", "Completely happy", "Fully recharged", "Everything is completely hygienic", "Yummy!",
    "Together\nwith your\nPokemon.", "Send the\nPokemon away\non its own.", "Call the\nPokemon back", "Your Pokemon\nis back", "Your Pokemon\nis sad", "Tomorrow again...",
    "A friendly Pokemon,\na little present,\ntwo Magic Berries.",
    "A friendly scuffle,\nkind words,\nso much fun.",
    "A beautiful picnic,\na full stomach,\ntotal satisfaction.",
    "Fought hard,\nbut still lost,\ntotally depressed.",
    "Walked a really long way,\ndiscovered so much,\ntotally satisfied.",
    "A berry bush,\nate so much,\nhappy and full.",
    "Got lost in the woods,\nwandered aimlessly,\nfully exhausted.",
    "Took a mud bath,\nsquish, squish, splash,\nlittle muddy buddy.",
    "Watched the clouds,\nchilled out,\nwell rested and happy.",
    "Fell into a hole,\ninjured and frustrated,\npretty unhappy.",
    "Time until return:", "%d minutes", "%d minute", "Back soon!", "%d seconds", "%d second", "End",
    "Do not\nturn off completely\nin step counter\nmode!",
    "+2 Magic Berrys!",
    "No opponents.", "Go on a trip!", "It's too late...",
    "Already sent", "one off today.", "Wait for Tomorrow!",
    "Choose slot", "Favorite confirmed", "Press and hold",
    "a Pokemon in the Pokedex", "to mark as a favorite",
    "POKEDEX", "Stage %d", "final stage",
    "Novice", "Field researcher", "Explorer", "Apprentice", "Assistant",
    "Expert", "Veteran", "Professor", "Master", "Legend", "God",
    "dreaming...", "bad dreams...",
    "Searching for", "Pokemon...",
    "Nobody here!",
    "Search", "Successful!",
    "It's playing",
    "on the go...",
    "Put Pokemon to sleep?", "Yes, lights out!", "No, stay awake!",
    "Connection error",
    "-1 Magic Berry!",
    "Confrontation...",
    "Hold to", "delete", "Really delete", "ALL", "Data?", "Data deleted!", "New game.",
  },
  // ---------------- DE ----------------
  {
    "Entwickelt sich!", "Mampf mampf!", "Gef`allt ihm!", "Hat Hunger!", "Braucht ein Bad!",
    "Ersch`opft...", "Traurig...", "Etwas rundlich...", "Es ist SHINY!!", "Es ist froh",
    "Licht aus", "Licht an", "Nickerchen", "Gerade nicht.", "verdaut noch...", "tropft noch...", "zu aufgekratzt...", "Nickerchen:", "Kein Hunger.", "Kugelrund...", "Es schl`aft...",
    "DANKE! Lebwohl", "Es ist weg...", "Tsch`uss! Winkt.",
    "EI", "Legend`ares Ei!?", "Seltenes Ei!", "Ber`uhre das Ei...", "Es bewegt sich!", "Fast soweit!",
    "POKEDEX %u/%u",
    "%s%s Lv.%u",
    "%s freilassen?", "JA", "NEIN",
    "%u TREFFER", "ATK +%u", "NEUER REKORD!", "REKORD: %u", "SCHNELL HAUEN!",
    "ATK max", "SPD +%u", "SPD max", "DEF +%u", "DEF max", "%u/10 Siege in Folge", "CHAMPION",
    "PUNKTE: %u", "Tolles Spiel!", "Das geht besser!",
    "ZEIT `ANDERN", "STD", "MIN", "wischen: Abbruch", "Sprache",
    "gespeichert", "TamaPoke", "Version", "Mrs. Spock", "Deutschland",
    "MEDAILLE!", "TOLL!", "%u TAGE SERIE!",
    "Meilenstein", "%s Schritte", "Streak Rekord", "%u Tage", "Neue Stufe!",
    "SERIE %u  Rekord %u", "BINDUNG", "BEERE ???", "ROTE BEERE", "BLAUE BEERE", "GR`UNE BEERE",
    "%s   ALTER %lu Tage", "%s   ALTER %lu Tag", "Name tippen: umbenennen",
    "KAMPF", "ATK", "DEF", "SPD", "GEWICHT", "TRAINING",
    "Gewonnen!", "Verloren.",
    "Typ:", "Begegnungen:", "Gewinnquote:", "Grossgezogen:",
    "flink", "normal", "tr`age", "dick",
    "MEDAILLEN %d/%d",
    "NAME:", "tippen f`ur zur`uck",
    "ESS", "FRO", "ENE", "HYG",
    "FORTSCHRITT", "Lv.%u", "%u min bis Lv.%u", "ENTWICKLUNG", "Endform",
    "Jetzt entwickeln!", "Es ist traurig...",
    "noch %u Level", "Care Miss: %u",
    "TON AN", "TON AUS",
    "ENTWICKELN", "%s will dir etwas sagen...", "%s f`uhlt sich einsam...",
    "Entwickeln?", "Form behalten", "Abschied?", "Lebwohl", "Zusammen bleiben",
    "W`ahle dein Starter",
    "Keine Sprites", "Auf die SD laden",
    "AUSFLUG", "SCHRITTE", "Ausflug beenden", "Gesamt:", "Sensor nicht gefunden", "Wirklich?",
    "Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag",
    "Mache einen Ausflug", "Heute:", "Zauberbeeren:", "Zauberbeere essen",
    "Niemanden getroffen.", "weiter", "+1 Zauberbeere!", "%s getroffen!", "NEU",
    "Zauberbeere verloren.", "Taschen sind voll.",
    "Hunger", "Freude", "Energie", "Hygiene",
    "Hunger komplett gestillt", "Komplett froh", "Energie komplett aufgef`ullt", "Alles ganz hygienisch", "Lecker!",
    "Zusammen\nmit deinem\nPokemon", "Pokemon\nalleine\nwegschicken.", "Pokemon\nzur`uckrufen", "Dein Pokemon\nist zur`uck", "Dein Pokemon\nist traurig", "Morgen wieder...",
    "Ein nettes Pokemon,\nein kleines Geschenk,\nzwei Zauberbeeren.",
    "Eine tolle Rauferei,\nfreundliche Worte,\nganz viel Spass.",
    "Ein sch`ones Picknick,\nein voller Magen,\ntotale Zufriedenheit.",
    "Hart gek`ampft,\ntrotzdem verloren,\ntotal deprimiert.",
    "Sehr weit gelaufen,\ntotal viel entdeckt,\nabsolut zufrieden.",
    "Ein Strauch mit Beeren,\nganz viel gegessen,\ngl`ucklich und satt.",
    "Im Wald verlaufen,\nziellos herumgeirrt,\nv`ollig ersch`opft.",
    "Schlammbad genommen,\nmatsch, matsch, platsch,\nkleiner Dreckspatz.",
    "Wolken beobachtet,\nausgiebig gechillt,\nausgeruht und froh.",
    "In ein Loch gefallen,\nverletzt und frustriert,\nziemlich ungl`ucklich.",
    "Zeit bis zur R`uckkehr:", "%d Minuten", "%d Minute", "Gleich wieder da!", "%d Sekunden", "%d Sekunde", "Ende",
    "Ger`at im\nSchrittz`ahlermodus\nnicht komplett\nabschalten!",
    "+2 Zauberbeeren!",
    "Keine Gegner.", "Mache Ausfl`uge!", "Schlafenszeit...",
    "Heute bereits", "eines weggeschickt.", "Warte bis morgen!",
    "Platz w`ahlen", "Favorit best`atigt", "Ein Pokemon im Pokedex",
    "gedr`uckt halten", "zum favorisieren",
    "POKEDEX", "Stufe %d", "finale Stufe",
    "Neuling", "Feldforscher", "Abenteurer", "Lehrling", "Assistent",
    "Experte", "Veteran", "Professor", "Meister", "Legende", "Gott",
    "tr`aumt...", "Albtr`aume...",
    "Suche nach", "Pokemon...",
    "Niemand da!",
    "Suche", "erfolgreich!",
    "Es spielt",
    "unterwegs...",
    "Pokemon schlafen legen?", "Ja, Licht aus!", "Nein, wach bleiben!",
    "Verbindungsfehler",
    "-1 Zauberbeere!",
    "Streiterei...",
    "Halten zum", "L`oschen", "Wirklich alle", "Daten", "l`oschen.", "Daten gel`oscht!", "Neues Spiel.",
  },
};

// Nombres de medalla en sus tres longitudes [idioma][medalla].
static const char *const MED_NAME[TEXT_LANGS][MED_COUNT] = {
  { "WALKER", "RENDEZVOUS", "Champion", "BERRY", "PERFECT DAY", "BOND", "FINAL FORM", "IN SHAPE" },
  { "VAGABUND", "RENDEZVOUS", "Champion", "BEERE", "TOLLER TAG", "BINDUNG", "ENDFORM", "TOP FORM" },
};
static const char *const MED_LBL[TEXT_LANGS][MED_COUNT] = {
  { "Walk", "Rdvz", "Champ", "BERRY", "Perf", "BOND", "TOP", "FIT" },
  { "Vaga", "Rdvz", "Champ", "BEERE", "Perf", "BND", "END", "TOPF" },
};
static const char *const MED_DSC[TEXT_LANGS][MED_COUNT] = {
  { "WALKER", "RENDEZVOUS", "CHAMPION", "BERRY FRENZY",
    "PERFECT DAY", "MAX BOND", "FINAL FORM", "IN SHAPE" },
  { "VAGABUND", "RENDEZVOUS", "CHAMPION", "BEEREN-LIEBE",
    "TOLLER TAG", "MAX BINDUNG", "ENDFORM", "TOP FORM" },
};

// nombres de tipo para la pantalla de info del dex, mismo orden que el
// enum Type (TYPE_NORMAL...TYPE_DRAGON)
static const char *const TYPE_NAME[TEXT_LANGS][TYPE_COUNT] = {
  { "Normal", "Fire", "Water", "Grass", "Electric", "Ice", "Fighting",
    "Poison", "Ground", "Psychic", "Bug", "Rock", "Ghost", "Dragon",
    "Flying", "Dark", "Steel" },
  { "Normal", "Feuer", "Wasser", "Pflanze", "Elektro", "Eis", "Kampf",
    "Gift", "Boden", "Psycho", "K`afer", "Gestein", "Geist", "Drache",
    "Flug", "Unlicht", "Stahl" },
};

// MIX usa los mismos textos que EN (solo dexName() cambia para MIX)
static inline uint8_t textLang() { return (gLang == LANG_MIX) ? LANG_EN : (gLang == LANG_DE_EN) ? LANG_DE : gLang; }

const char *T(StrId id) { return STRINGS[textLang()][id]; }
const char *medalName(int i)  { return MED_NAME[textLang()][i]; }
const char *medalLabel(int i) { return MED_LBL[textLang()][i]; }
const char *medalDesc(int i)  { return MED_DSC[textLang()][i]; }
const char *typeName(int t)   { return TYPE_NAME[textLang()][t]; }

void loadLang() {
  Preferences p;
  p.begin("tamapoke", true);  // solo lectura
  uint8_t v = p.getUChar("lang", LANG_DEFAULT);
  p.end();
  gLang = (v < LANG_COUNT) ? (Lang)v : LANG_DEFAULT;
}

void setLang(Lang l) {
  if (l >= LANG_COUNT) return;
  gLang = l;
  Preferences p;
  p.begin("tamapoke", false);
  p.putUChar("lang", (uint8_t)l);
  p.end();
}
