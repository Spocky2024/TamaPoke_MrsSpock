#pragma once
#include <Arduino.h>

// Idiomas soportados. La fuente del firmware no tiene acentos: ambos textos van
// sin tildes ni enes (igual que ya iba el espanol).
enum Lang : uint8_t { LANG_EN = 0, LANG_DE, LANG_MIX, LANG_DE_EN, LANG_COUNT };
#define LANG_DEFAULT LANG_EN  // idioma por defecto: ingles
// MIX ("EN/DE") reutiliza los textos de la interfaz de EN, con nombres de
// Pokemon en aleman. LANG_DE_EN ("DE/EN") es lo contrario: interfaz en
// aleman, nombres de Pokemon en ingles. Por eso las tablas de texto
// (STRINGS, MED_*, TYPE_NAME) solo tienen TEXT_LANGS filas, no LANG_COUNT.
#define TEXT_LANGS 2

extern Lang gLang;  // idioma activo (definido en i18n.cpp)

// IDs de cadena. El orden debe coincidir con la tabla STRINGS de i18n.cpp.
enum StrId : uint8_t {
  // estado del bicho (statusMsg)
  S_EVOLVING, S_EATING, S_LIKES, S_HUNGRY, S_NEEDS_BATH,
  S_EXHAUSTED, S_SAD, S_CHUBBY, S_IS_SHINY, S_HAPPY,
  // boton dormir/luz: avisos breves (Licht aus/an, Power-Nap)
  S_LIGHTS_OUT, S_LIGHTS_ON, S_NAP, S_NOT_YET, S_BERRY_CD, S_BATH_CD, S_NAP_CD, S_NAPPING, S_NOT_HUNGRY, S_TOO_ROUND, S_IS_SLEEPING,
  // ceremonias de despedida
  S_FAREWELL, S_RUNAWAY, S_GOODBYE,
  // huevo
  S_EGG_HDR, S_EGG_LEGEND, S_EGG_RARE, S_EGG_TOUCH, S_EGG_MOVES, S_EGG_ALMOST,
  // formatos compartidos
  S_POKEDEX_FMT,   // "POKEDEX %u/%u"
  S_NAME_FMT,      // "%s%s Nv.%u"
  // dialogo soltar
  S_RELEASE_FMT, S_YES, S_NO,
  // minijuego y saco
  S_HITS_FMT, S_STR_GAIN_FMT, S_NEW_RECORD, S_RECORD_FMT, S_HIT_FAST,
  S_ATK_MAX, S_SPD_GAIN_FMT, S_SPD_MAX, S_DEF_GAIN_FMT, S_DEF_MAX, S_WIN_STREAK_FMT, S_CHAMPION_TXT,
  S_SCORE_FMT, S_GREAT_JOY, S_PLUS_JOY,
  // reloj / ajustes
  S_SET_TIME, S_HOUR, S_MIN, S_CLOCK_CANCEL, S_LANG_LABEL,
  S_CLOCK_SAVED, S_APP_NAME, S_VERSION_LBL, S_VERSION_NAME, S_COUNTRY,
  // celebracion
  S_MEDAL_BANNER, S_GREAT, S_STREAK_DAYS_FMT,
  S_MILESTONE_BANNER, S_STEP_MILESTONE_FMT, S_STREAK_RECORD_BANNER, S_STREAK_RECORD_FMT,
  S_TIER_UP_BANNER,
  // ficha: perfil
  S_STREAK_FMT, S_VIN, S_BERRY_UNK, S_BERRY_RED, S_BERRY_BLUE, S_BERRY_GREEN,
  S_INFO_FMT, S_INFO_FMT_1, S_RENAME_HINT,
  // ficha: combate
  S_BATTLE, S_STAT_ATK, S_STAT_DEF, S_STAT_SPE, S_STAT_WGT, S_TRAIN_STR,
  S_BATTLE_WIN, S_BATTLE_LOSE,
  S_INFO_TYPE, S_INFO_MEETINGS, S_INFO_WINRATE, S_INFO_RAISED,
  S_WGT_THIN, S_WGT_NORMAL, S_WGT_THICK, S_WGT_FAT,
  // ficha: medallas
  S_MEDALS_FMT,
  // teclado y galeria
  S_NAME, S_DETAIL_BACK,
  // barras
  S_BAR_FOOD, S_BAR_JOY, S_BAR_ENE, S_BAR_HYG,
  // ficha: pagina de progreso
  S_PROGRESS, S_LVL_FMT, S_NEXT_LVL_FMT, S_EVO_LABEL, S_FINAL_FORM,
  S_EVO_READY, S_EVO_BLOCKED, S_EVO_IN_FMT, S_MISTAKES_FMT,
  // interruptor de sonido (ajustes)
  S_SND_ON, S_SND_OFF,
  S_EVO_TAP,        // texto del boton de evolucion
  S_FAREWELL_BTN,   // texto del boton de despedida (lleva el nombre: "%s ...")
  S_RUNAWAY_BTN,    // texto del boton de escapada por abandono (final triste)
  // dialogos de decision (evolucionar/mantener, despedirse/quedaros)
  S_EVO_Q, S_EVO_KEEP, S_FAR_Q, S_FAR_GO, S_FAR_STAY,
  S_CHOOSE_STARTER,  // titulo de la eleccion del inicial (primera vez)
  S_NO_SPRITES, S_LOAD_SPRITES,  // aviso cuando falta el sprite en la SD
  // ausflug / trip: contador de pasos
  S_TRIP, S_TRIP_STEPS, S_TRIP_CANCEL, S_TRIP_TOTAL, S_TRIP_NO_SENSOR, S_TRIP_REALLY,
  S_WD_SUN, S_WD_MON, S_WD_TUE, S_WD_WED, S_WD_THU, S_WD_FRI, S_WD_SAT,
  S_TRIP_GO, S_TRIP_TODAY, S_TRIP_BERRY_COUNT, S_TRIP_EAT_BERRY,
  S_TRIP_NOBODY, S_TRIP_NEXT, S_TRIP_GOT_BERRY, S_TRIP_MET_FMT, S_TRIP_NEW,
  S_BERRY_LOST1, S_BERRY_LOST2,
  S_FULL_HUNGER, S_FULL_JOY, S_FULL_ENERGY, S_FULL_HYGIENE,
  S_BERRY_MSG_HUNGER, S_BERRY_MSG_JOY, S_BERRY_MSG_ENERGY, S_BERRY_MSG_HYGIENE, S_YUMMY,
  S_TRIP_TOGETHER, S_TRIP_ALONE, S_CALL_BACK, S_POKEMON_BACK, S_SOLO_SAD, S_SOLO_LIMIT,
  S_SOLO_EVT1, S_SOLO_EVT2, S_SOLO_EVT3, S_SOLO_EVT4, S_SOLO_EVT5,
  S_SOLO_EVT6, S_SOLO_EVT7, S_SOLO_EVT8, S_SOLO_EVT9, S_SOLO_EVT10,
  S_TIME_UNTIL_RETURN, S_MINUTES_FMT, S_MINUTE_FMT, S_BACK_SOON, S_SECONDS_FMT, S_SECOND_FMT, S_TRIP_SOLO_END,
  S_TRIP_POWEROFF_WARN, S_SOLO_EVT1_BONUS,
  S_BATTLE_NO_OPP1, S_BATTLE_NO_OPP2, S_TOO_LATE,
  S_RELEASE_LIMIT1, S_RELEASE_LIMIT2, S_RELEASE_LIMIT3,
  S_FAV_TITLE, S_FAV_CONFIRMED, S_FAV_HINT1, S_FAV_HINT2, S_FAV_HINT3,
  S_POKEDEX_HDR, S_STAGE_FMT, S_STAGE_FINAL,
  S_RANK_0, S_RANK_1, S_RANK_2, S_RANK_3, S_RANK_4, S_RANK_5,
  S_RANK_6, S_RANK_7, S_RANK_8, S_RANK_9, S_RANK_10,
  S_DREAMS_FMT,
  S_BAD_DREAMS_FMT,  // Zwangsschlaf ohne ausgeschaltetes Licht
  S_POKE_SEARCH1, S_POKE_SEARCH2,  // "Suche nach" / "Pokemon..."
  S_POKE_NOBODY,                    // "Niemand da!"
  S_POKE_SUCCESS1, S_POKE_SUCCESS2, // "Suche" / "erfolgreich!"
  S_VISIT_PLAYING,                  // "Es spielt"
  S_VISIT_AWAY,                     // "unterwegs"
  S_SLEEP_CONFIRM_Q, S_SLEEP_CONFIRM_YES, S_SLEEP_CONFIRM_NO,
  S_POKE_CONN_ERROR,  // "Verbindungsfehler" / "Connection error"
  S_TRIP_LOST_BERRY_TO,  // "-1 Zauberbeere!" / "-1 Magic Berry!" (verschenkt beim Treffen)
  S_VISIT_FIGHTING,  // "Streiterei..." / "Confrontation..."
  S_CLOCK_DELETE_L1, S_CLOCK_DELETE_L2, S_DELETE_CONFIRM1, S_DELETE_CONFIRM2, S_DELETE_CONFIRM3, S_DELETE_DONE1, S_DELETE_DONE2,
  STR_COUNT
};

const char *T(StrId id);       // texto en el idioma activo
const char *medalName(int i);  // banner de medalla (MED_COUNT)
const char *medalLabel(int i); // etiqueta corta de medalla
const char *medalDesc(int i);  // descripcion larga de medalla
const char *typeName(int t);   // nombre de tipo para la pantalla de info

void loadLang();             // lee el idioma de NVS (llamar en setup)
void setLang(Lang l);        // cambia y persiste el idioma
