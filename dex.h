#pragma once
#include <stdint.h>
#include "i18n.h"  // gLang

// GENERADO por tools/gen_dex.py desde tools/dex_data.py - no editar

#define DEX_COUNT 386
#define DEX_EEVEE 133  // rama: 134/135/136 (gen1) + 196/197 (gen2)

// rareza: 0 = solo por evolucion, 1 = comun, 2 = raro, 3 = legendario
enum : uint8_t { R_EVO = 0, R_COMUN, R_RARO, R_LEGENDARIO };

// tipos elementales (los 17 que existen como tipo principal en la era
// clasica gen1-3, ANTES de que se introdujera Hada en la gen6 -- por
// consistencia con gen1, esta base usa la clasificacion clasica, no la
// moderna con retcon de Hada)
enum : uint8_t { TYPE_NORMAL, TYPE_FIRE, TYPE_WATER, TYPE_GRASS, TYPE_ELECTRIC, TYPE_ICE, TYPE_FIGHTING, TYPE_POISON, TYPE_GROUND, TYPE_PSYCHIC, TYPE_BUG, TYPE_ROCK, TYPE_GHOST, TYPE_DRAGON, TYPE_FLYING, TYPE_DARK, TYPE_STEEL, TYPE_COUNT };

struct DexEntry {
  const char *name;
  uint16_t evolvesTo;   // numero de dex, 0 = forma final
  uint8_t evolveLevel;
  uint8_t rarity;       // sale de huevo si > 0
  uint16_t accent;      // color RGB565 del tipo para la UI
  uint8_t bHp, bAtk, bDef, bSpe;  // base stats reales de gen 1
  uint8_t biome;        // 0 pradera 1 playa 2 bosque 3 volcan 4 montana 5 nieve
  uint8_t type;         // TYPE_xxx
};

// tabla de tipos: TYPE_CHART[atacante][defensor], en octavos (8 = x1)
// orden: normal, fuego, agua, planta, electrico, hielo, lucha, veneno,
// tierra, psiquico, bicho, roca, fantasma, dragon, volador, siniestro, acero
static const uint8_t TYPE_CHART[17][17] = {
  { 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 4, 0, 8,  8, 8, 4 },  // normal
  { 8, 4, 4, 16, 8, 16, 8, 8, 8, 8, 16, 4, 8, 4,  8, 8, 16 },  // fuego
  { 8, 16, 4, 4, 8, 8, 8, 8, 16, 8, 8, 16, 8, 4,  8, 8, 8 },  // agua
  { 8, 4, 16, 4, 8, 8, 8, 4, 16, 8, 4, 16, 8, 4,  4, 8, 4 },  // planta
  { 8, 8, 16, 4, 4, 8, 8, 8, 0, 8, 8, 8, 8, 4,  16, 8, 8 },  // electrico
  { 8, 4, 4, 16, 8, 4, 8, 8, 16, 8, 8, 8, 8, 16,  16, 8, 4 },  // hielo
  { 16, 8, 8, 8, 8, 16, 8, 4, 8, 4, 4, 16, 0, 8,  4, 16, 16 },  // lucha
  { 8, 8, 8, 16, 8, 8, 8, 4, 4, 8, 8, 4, 4, 8,  8, 8, 0 },  // veneno
  { 8, 16, 8, 4, 16, 8, 8, 16, 8, 8, 4, 16, 8, 8,  0, 8, 16 },  // tierra
  { 8, 8, 8, 8, 8, 8, 16, 16, 8, 4, 8, 8, 8, 8,  8, 0, 4 },  // psiquico
  { 8, 4, 8, 16, 8, 8, 4, 4, 8, 16, 8, 8, 4, 8,  4, 16, 4 },  // bicho
  { 8, 16, 8, 8, 8, 16, 4, 8, 4, 8, 16, 8, 8, 8,  16, 8, 4 },  // roca
  { 0, 8, 8, 8, 8, 8, 8, 8, 8, 16, 8, 8, 16, 8,  8, 4, 4 },  // fantasma
  { 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 16,  8, 8, 4 },  // dragon
  { 8, 8, 8, 16, 4, 8, 16, 8, 8, 8, 16, 4, 8, 8,  8, 8, 4 },  // volador
  { 8, 8, 8, 8, 8, 8, 4, 8, 8, 16, 8, 8, 16, 8,  8, 4, 4 },  // siniestro
  { 8, 4, 4, 8, 4, 16, 8, 8, 8, 8, 8, 16, 8, 8,  8, 8, 4 },  // acero
};

// color oficial por tipo (pantalla de info), TYPE_COLOR[TYPE_xxx]
static const uint16_t TYPE_COLOR[17] = {
  0xAD4F,  // normal
  0xF406,  // fuego
  0x6C9E,  // agua
  0x7E4A,  // planta
  0xFE86,  // electrico
  0x9EDB,  // hielo
  0xC185,  // lucha
  0xA214,  // veneno
  0xE60D,  // tierra
  0xFAD1,  // psiquico
  0xADC4,  // bicho
  0xBD07,  // roca
  0x72D3,  // fantasma
  0x71DF,  // dragon
  0xAC9E,  // volador
  0x72C9,  // siniestro
  0xBDDA,  // acero
};

static const DexEntry DEX_TBL[DEX_COUNT + 1] = {
  { "?", 0, 0, 0, 0x2946, 50, 50, 50, 50, 0 },  // 0: sin usar
  { "BULBASAUR", 2, 16, R_COMUN, 0x3C49, 45, 49, 49, 45, 2, TYPE_GRASS },  // 1 planta
  { "IVYSAUR", 3, 32, R_EVO, 0x3C49, 60, 62, 63, 60, 2, TYPE_GRASS },  // 2 planta
  { "VENUSAUR", 0, 0, R_EVO, 0x3C49, 80, 82, 83, 80, 2, TYPE_GRASS },  // 3 planta
  { "CHARMANDER", 5, 16, R_COMUN, 0xEA87, 39, 52, 43, 65, 3, TYPE_FIRE },  // 4 fuego
  { "CHARMELEON", 6, 36, R_EVO, 0xEA87, 58, 64, 58, 80, 3, TYPE_FIRE },  // 5 fuego
  { "CHARIZARD", 0, 0, R_EVO, 0xEA87, 78, 84, 78, 100, 3, TYPE_FIRE },  // 6 fuego
  { "SQUIRTLE", 8, 16, R_COMUN, 0x4C98, 44, 48, 65, 43, 1, TYPE_WATER },  // 7 agua
  { "WARTORTLE", 9, 36, R_EVO, 0x4C98, 59, 63, 80, 58, 1, TYPE_WATER },  // 8 agua
  { "BLASTOISE", 0, 0, R_EVO, 0x4C98, 79, 83, 100, 78, 1, TYPE_WATER },  // 9 agua
  { "CATERPIE", 11, 7, R_COMUN, 0x7CC4, 45, 30, 35, 45, 2, TYPE_BUG },  // 10 bicho
  { "METAPOD", 12, 10, R_EVO, 0x7CC4, 50, 20, 55, 30, 2, TYPE_BUG },  // 11 bicho
  { "BUTTERFREE", 0, 0, R_EVO, 0x7CC4, 60, 45, 50, 70, 2, TYPE_BUG },  // 12 bicho
  { "WEEDLE", 14, 7, R_COMUN, 0x7CC4, 40, 35, 30, 50, 2, TYPE_BUG },  // 13 bicho
  { "KAKUNA", 15, 10, R_EVO, 0x7CC4, 45, 25, 50, 35, 2, TYPE_BUG },  // 14 bicho
  { "BEEDRILL", 0, 0, R_EVO, 0x7CC4, 65, 90, 40, 75, 2, TYPE_BUG },  // 15 bicho
  { "PIDGEY", 17, 18, R_COMUN, 0x8C4D, 40, 45, 40, 56, 0, TYPE_NORMAL },  // 16 normal
  { "PIDGEOTTO", 18, 36, R_EVO, 0x8C4D, 63, 60, 55, 71, 0, TYPE_NORMAL },  // 17 normal
  { "PIDGEOT", 0, 0, R_EVO, 0x8C4D, 83, 80, 75, 101, 0, TYPE_NORMAL },  // 18 normal
  { "RATTATA", 20, 20, R_COMUN, 0x8C4D, 30, 56, 35, 72, 0, TYPE_NORMAL },  // 19 normal
  { "RATICATE", 0, 0, R_EVO, 0x8C4D, 55, 81, 60, 97, 0, TYPE_NORMAL },  // 20 normal
  { "SPEAROW", 22, 20, R_COMUN, 0x8C4D, 40, 60, 30, 70, 0, TYPE_NORMAL },  // 21 normal
  { "FEAROW", 0, 0, R_EVO, 0x8C4D, 65, 90, 65, 100, 0, TYPE_NORMAL },  // 22 normal
  { "EKANS", 24, 22, R_COMUN, 0x8A73, 35, 60, 44, 55, 0, TYPE_POISON },  // 23 veneno
  { "ARBOK", 0, 0, R_EVO, 0x8A73, 60, 95, 69, 80, 0, TYPE_POISON },  // 24 veneno
  { "PIKACHU", 26, 30, R_COMUN, 0xBCA1, 35, 55, 40, 90, 0, TYPE_ELECTRIC },  // 25 electrico
  { "RAICHU", 0, 0, R_EVO, 0xBCA1, 60, 90, 55, 110, 0, TYPE_ELECTRIC },  // 26 electrico
  { "SANDSHREW", 28, 22, R_COMUN, 0xB447, 50, 75, 85, 40, 4, TYPE_GROUND },  // 27 tierra
  { "SANDSLASH", 0, 0, R_EVO, 0xB447, 75, 100, 110, 65, 4, TYPE_GROUND },  // 28 tierra
  { "NIDORAN f", 30, 16, R_COMUN, 0x8A73, 55, 47, 52, 41, 0, TYPE_POISON },  // 29 veneno
  { "NIDORINA", 31, 30, R_EVO, 0x8A73, 70, 62, 67, 56, 0, TYPE_POISON },  // 30 veneno
  { "NIDOQUEEN", 0, 0, R_EVO, 0x8A73, 90, 92, 87, 76, 0, TYPE_POISON },  // 31 veneno
  { "NIDORAN m", 33, 16, R_COMUN, 0x8A73, 46, 57, 40, 50, 0, TYPE_POISON },  // 32 veneno
  { "NIDORINO", 34, 30, R_EVO, 0x8A73, 61, 72, 57, 65, 0, TYPE_POISON },  // 33 veneno
  { "NIDOKING", 0, 0, R_EVO, 0x8A73, 81, 102, 77, 85, 0, TYPE_POISON },  // 34 veneno
  { "CLEFAIRY", 36, 30, R_COMUN, 0x8C4D, 70, 45, 48, 35, 0, TYPE_NORMAL },  // 35 normal
  { "CLEFABLE", 0, 0, R_EVO, 0x8C4D, 95, 70, 73, 60, 0, TYPE_NORMAL },  // 36 normal
  { "VULPIX", 38, 30, R_COMUN, 0xEA87, 38, 41, 40, 65, 3, TYPE_FIRE },  // 37 fuego
  { "NINETALES", 0, 0, R_EVO, 0xEA87, 73, 76, 75, 100, 3, TYPE_FIRE },  // 38 fuego
  { "JIGGLYPUFF", 40, 30, R_COMUN, 0x8C4D, 115, 45, 20, 20, 0, TYPE_NORMAL },  // 39 normal
  { "WIGGLYTUFF", 0, 0, R_EVO, 0x8C4D, 140, 70, 45, 45, 0, TYPE_NORMAL },  // 40 normal
  { "ZUBAT", 42, 22, R_COMUN, 0x8A73, 40, 45, 35, 55, 0, TYPE_POISON },  // 41 veneno
  { "GOLBAT", 169, 30, R_EVO, 0x8A73, 75, 80, 70, 90, 0, TYPE_POISON },  // 42 veneno -> evoluciona a Crobat (gen2) por vinculo, simplificado a nivel
  { "ODDISH", 44, 21, R_COMUN, 0x3C49, 45, 50, 55, 30, 2, TYPE_GRASS },  // 43 planta
  { "GLOOM", 45, 36, R_EVO, 0x3C49, 60, 65, 70, 40, 2, TYPE_GRASS },  // 44 planta
  { "VILEPLUME", 0, 0, R_EVO, 0x3C49, 75, 80, 85, 50, 2, TYPE_GRASS },  // 45 planta
  { "PARAS", 47, 24, R_COMUN, 0x7CC4, 35, 70, 55, 25, 2, TYPE_BUG },  // 46 bicho
  { "PARASECT", 0, 0, R_EVO, 0x7CC4, 60, 95, 80, 30, 2, TYPE_BUG },  // 47 bicho
  { "VENONAT", 49, 31, R_COMUN, 0x7CC4, 60, 55, 50, 45, 2, TYPE_BUG },  // 48 bicho
  { "VENOMOTH", 0, 0, R_EVO, 0x7CC4, 70, 65, 60, 90, 2, TYPE_BUG },  // 49 bicho
  { "DIGLETT", 51, 26, R_COMUN, 0xB447, 10, 55, 25, 95, 4, TYPE_GROUND },  // 50 tierra
  { "DUGTRIO", 0, 0, R_EVO, 0xB447, 35, 100, 50, 120, 4, TYPE_GROUND },  // 51 tierra
  { "MEOWTH", 53, 28, R_COMUN, 0x8C4D, 40, 45, 35, 90, 0, TYPE_NORMAL },  // 52 normal
  { "PERSIAN", 0, 0, R_EVO, 0x8C4D, 65, 70, 60, 115, 0, TYPE_NORMAL },  // 53 normal
  { "PSYDUCK", 55, 33, R_COMUN, 0x4C98, 50, 52, 48, 55, 1, TYPE_WATER },  // 54 agua
  { "GOLDUCK", 0, 0, R_EVO, 0x4C98, 80, 82, 78, 85, 1, TYPE_WATER },  // 55 agua
  { "MANKEY", 57, 28, R_COMUN, 0xA2A5, 40, 80, 35, 70, 0, TYPE_FIGHTING },  // 56 lucha
  { "PRIMEAPE", 0, 0, R_EVO, 0xA2A5, 65, 105, 60, 95, 0, TYPE_FIGHTING },  // 57 lucha
  { "GROWLITHE", 59, 30, R_RARO, 0xEA87, 55, 70, 45, 60, 3, TYPE_FIRE },  // 58 fuego
  { "ARCANINE", 0, 0, R_EVO, 0xEA87, 90, 110, 80, 95, 3, TYPE_FIRE },  // 59 fuego
  { "POLIWAG", 61, 25, R_COMUN, 0x4C98, 40, 50, 40, 90, 1, TYPE_WATER },  // 60 agua
  { "POLIWHIRL", 62, 36, R_EVO, 0x4C98, 65, 65, 65, 90, 1, TYPE_WATER },  // 61 agua
  { "POLIWRATH", 0, 0, R_EVO, 0x4C98, 90, 95, 95, 70, 1, TYPE_WATER },  // 62 agua
  { "ABRA", 64, 16, R_COMUN, 0xD28F, 25, 20, 15, 90, 0, TYPE_PSYCHIC },  // 63 psiquico
  { "KADABRA", 65, 40, R_EVO, 0xD28F, 40, 35, 30, 105, 0, TYPE_PSYCHIC },  // 64 psiquico
  { "ALAKAZAM", 0, 0, R_EVO, 0xD28F, 55, 50, 45, 120, 0, TYPE_PSYCHIC },  // 65 psiquico
  { "MACHOP", 67, 28, R_COMUN, 0xA2A5, 70, 80, 50, 35, 0, TYPE_FIGHTING },  // 66 lucha
  { "MACHOKE", 68, 40, R_EVO, 0xA2A5, 80, 100, 70, 45, 0, TYPE_FIGHTING },  // 67 lucha
  { "MACHAMP", 0, 0, R_EVO, 0xA2A5, 90, 130, 80, 55, 0, TYPE_FIGHTING },  // 68 lucha
  { "BELLSPROUT", 70, 21, R_COMUN, 0x3C49, 50, 75, 35, 40, 2, TYPE_GRASS },  // 69 planta
  { "WEEPINBELL", 71, 36, R_EVO, 0x3C49, 65, 90, 50, 55, 2, TYPE_GRASS },  // 70 planta
  { "VICTREEBEL", 0, 0, R_EVO, 0x3C49, 80, 105, 65, 70, 2, TYPE_GRASS },  // 71 planta
  { "TENTACOOL", 73, 30, R_COMUN, 0x4C98, 40, 40, 35, 70, 1, TYPE_WATER },  // 72 agua
  { "TENTACRUEL", 0, 0, R_EVO, 0x4C98, 80, 70, 65, 100, 1, TYPE_WATER },  // 73 agua
  { "GEODUDE", 75, 25, R_COMUN, 0x9407, 40, 80, 100, 20, 4, TYPE_ROCK },  // 74 roca
  { "GRAVELER", 76, 40, R_EVO, 0x9407, 55, 95, 115, 35, 4, TYPE_ROCK },  // 75 roca
  { "GOLEM", 0, 0, R_EVO, 0x9407, 80, 120, 130, 45, 4, TYPE_ROCK },  // 76 roca
  { "PONYTA", 78, 40, R_RARO, 0xEA87, 50, 85, 55, 90, 3, TYPE_FIRE },  // 77 fuego
  { "RAPIDASH", 0, 0, R_EVO, 0xEA87, 65, 100, 70, 105, 3, TYPE_FIRE },  // 78 fuego
  { "SLOWPOKE", 80, 37, R_COMUN, 0x4C98, 90, 65, 65, 15, 1, TYPE_WATER },  // 79 agua
  { "SLOWBRO", 0, 0, R_EVO, 0x4C98, 95, 75, 110, 30, 1, TYPE_WATER },  // 80 agua
  { "MAGNEMITE", 82, 30, R_COMUN, 0xBCA1, 25, 35, 70, 45, 0, TYPE_ELECTRIC },  // 81 electrico
  { "MAGNETON", 0, 0, R_EVO, 0xBCA1, 50, 60, 95, 70, 0, TYPE_ELECTRIC },  // 82 electrico
  { "FARFETCHD", 0, 0, R_RARO, 0x8C4D, 52, 90, 55, 60, 0, TYPE_NORMAL },  // 83 normal
  { "DODUO", 85, 31, R_COMUN, 0x8C4D, 35, 85, 45, 75, 0, TYPE_NORMAL },  // 84 normal
  { "DODRIO", 0, 0, R_EVO, 0x8C4D, 60, 110, 70, 110, 0, TYPE_NORMAL },  // 85 normal
  { "SEEL", 87, 34, R_COMUN, 0x4C98, 65, 45, 55, 45, 1, TYPE_WATER },  // 86 agua
  { "DEWGONG", 0, 0, R_EVO, 0x4C98, 90, 70, 80, 70, 1, TYPE_WATER },  // 87 agua
  { "GRIMER", 89, 38, R_RARO, 0x8A73, 80, 80, 50, 25, 0, TYPE_POISON },  // 88 veneno
  { "MUK", 0, 0, R_EVO, 0x8A73, 105, 105, 75, 50, 0, TYPE_POISON },  // 89 veneno
  { "SHELLDER", 91, 30, R_COMUN, 0x4C98, 30, 65, 100, 40, 1, TYPE_WATER },  // 90 agua
  { "CLOYSTER", 0, 0, R_EVO, 0x4C98, 50, 95, 180, 70, 1, TYPE_WATER },  // 91 agua
  { "GASTLY", 93, 25, R_COMUN, 0x6AD3, 30, 35, 30, 80, 0, TYPE_GHOST },  // 92 fantasma
  { "HAUNTER", 94, 40, R_EVO, 0x6AD3, 45, 50, 45, 95, 0, TYPE_GHOST },  // 93 fantasma
  { "GENGAR", 0, 0, R_EVO, 0x6AD3, 60, 65, 60, 110, 0, TYPE_GHOST },  // 94 fantasma
  { "ONIX", 208, 30, R_RARO, 0x9407, 35, 45, 160, 70, 4, TYPE_ROCK },  // 95 roca -> evoluciona a Steelix (gen2)
  { "DROWZEE", 97, 26, R_COMUN, 0xD28F, 60, 48, 45, 42, 0, TYPE_PSYCHIC },  // 96 psiquico
  { "HYPNO", 0, 0, R_EVO, 0xD28F, 85, 73, 70, 67, 0, TYPE_PSYCHIC },  // 97 psiquico
  { "KRABBY", 99, 28, R_COMUN, 0x4C98, 30, 105, 90, 50, 1, TYPE_WATER },  // 98 agua
  { "KINGLER", 0, 0, R_EVO, 0x4C98, 55, 130, 115, 75, 1, TYPE_WATER },  // 99 agua
  { "VOLTORB", 101, 30, R_COMUN, 0xBCA1, 40, 30, 50, 100, 0, TYPE_ELECTRIC },  // 100 electrico
  { "ELECTRODE", 0, 0, R_EVO, 0xBCA1, 60, 50, 70, 150, 0, TYPE_ELECTRIC },  // 101 electrico
  { "EXEGGCUTE", 103, 30, R_COMUN, 0x3C49, 60, 40, 80, 40, 2, TYPE_GRASS },  // 102 planta
  { "EXEGGUTOR", 0, 0, R_EVO, 0x3C49, 95, 95, 85, 55, 2, TYPE_GRASS },  // 103 planta
  { "CUBONE", 105, 28, R_COMUN, 0xB447, 50, 50, 95, 35, 4, TYPE_GROUND },  // 104 tierra
  { "MAROWAK", 0, 0, R_EVO, 0xB447, 60, 80, 110, 45, 4, TYPE_GROUND },  // 105 tierra
  { "HITMONLEE", 0, 0, R_RARO, 0xA2A5, 50, 120, 53, 87, 0, TYPE_FIGHTING },  // 106 lucha
  { "HITMONCHAN", 0, 0, R_RARO, 0xA2A5, 50, 105, 79, 76, 0, TYPE_FIGHTING },  // 107 lucha
  { "LICKITUNG", 0, 0, R_RARO, 0x8C4D, 90, 55, 75, 30, 0, TYPE_NORMAL },  // 108 normal
  { "KOFFING", 110, 35, R_COMUN, 0x8A73, 40, 65, 95, 35, 0, TYPE_POISON },  // 109 veneno
  { "WEEZING", 0, 0, R_EVO, 0x8A73, 65, 90, 120, 60, 0, TYPE_POISON },  // 110 veneno
  { "RHYHORN", 112, 42, R_RARO, 0xB447, 80, 85, 95, 25, 4, TYPE_GROUND },  // 111 tierra
  { "RHYDON", 0, 0, R_EVO, 0xB447, 105, 130, 120, 40, 4, TYPE_GROUND },  // 112 tierra
  { "CHANSEY", 242, 30, R_RARO, 0x8C4D, 250, 5, 5, 50, 0, TYPE_NORMAL },  // 113 normal -> evoluciona a Blissey (gen2) por vinculo, simplificado a nivel
  { "TANGELA", 0, 0, R_RARO, 0x3C49, 65, 55, 115, 60, 2, TYPE_GRASS },  // 114 planta
  { "KANGASKHAN", 0, 0, R_RARO, 0x8C4D, 105, 95, 80, 90, 0, TYPE_NORMAL },  // 115 normal
  { "HORSEA", 117, 32, R_COMUN, 0x4C98, 30, 40, 70, 60, 1, TYPE_WATER },  // 116 agua
  { "SEADRA", 230, 42, R_EVO, 0x4C98, 55, 65, 95, 85, 1, TYPE_WATER },  // 117 agua -> evoluciona a Kingdra (gen2)
  { "GOLDEEN", 119, 33, R_COMUN, 0x4C98, 45, 67, 60, 63, 1, TYPE_WATER },  // 118 agua
  { "SEAKING", 0, 0, R_EVO, 0x4C98, 80, 92, 65, 68, 1, TYPE_WATER },  // 119 agua
  { "STARYU", 121, 30, R_COMUN, 0x4C98, 30, 45, 55, 85, 1, TYPE_WATER },  // 120 agua
  { "STARMIE", 0, 0, R_EVO, 0x4C98, 60, 75, 85, 115, 1, TYPE_WATER },  // 121 agua
  { "MR. MIME", 0, 0, R_RARO, 0xD28F, 40, 45, 65, 90, 0, TYPE_PSYCHIC },  // 122 psiquico
  { "SCYTHER", 212, 30, R_RARO, 0x7CC4, 70, 110, 80, 105, 2, TYPE_BUG },  // 123 bicho -> evoluciona a Scizor (gen2)
  { "JYNX", 0, 0, R_RARO, 0x4DB8, 65, 50, 35, 95, 5, TYPE_ICE },  // 124 hielo
  { "ELECTABUZZ", 0, 0, R_RARO, 0xBCA1, 65, 83, 57, 105, 0, TYPE_ELECTRIC },  // 125 electrico
  { "MAGMAR", 0, 0, R_RARO, 0xEA87, 65, 95, 57, 93, 3, TYPE_FIRE },  // 126 fuego
  { "PINSIR", 0, 0, R_RARO, 0x7CC4, 65, 125, 100, 85, 2, TYPE_BUG },  // 127 bicho
  { "TAUROS", 0, 0, R_RARO, 0x8C4D, 75, 100, 95, 110, 0, TYPE_NORMAL },  // 128 normal
  { "MAGIKARP", 130, 20, R_COMUN, 0x4C98, 20, 10, 55, 80, 1, TYPE_WATER },  // 129 agua
  { "GYARADOS", 0, 0, R_EVO, 0x4C98, 95, 125, 79, 81, 1, TYPE_WATER },  // 130 agua
  { "LAPRAS", 0, 0, R_RARO, 0x4C98, 130, 85, 80, 60, 1, TYPE_WATER },  // 131 agua
  { "DITTO", 0, 0, R_RARO, 0x8C4D, 48, 48, 48, 48, 0, TYPE_NORMAL },  // 132 normal
  { "EEVEE", 134, 30, R_COMUN, 0x8C4D, 55, 55, 50, 55, 0, TYPE_NORMAL },  // 133 normal
  { "VAPOREON", 0, 0, R_EVO, 0x4C98, 130, 65, 60, 65, 1, TYPE_WATER },  // 134 agua
  { "JOLTEON", 0, 0, R_EVO, 0xBCA1, 65, 65, 60, 130, 0, TYPE_ELECTRIC },  // 135 electrico
  { "FLAREON", 0, 0, R_EVO, 0xEA87, 65, 130, 60, 65, 3, TYPE_FIRE },  // 136 fuego
  { "PORYGON", 233, 30, R_RARO, 0x8C4D, 65, 60, 70, 40, 0, TYPE_NORMAL },  // 137 normal -> evoluciona a Porygon2 (gen2)
  { "OMANYTE", 139, 40, R_RARO, 0x9407, 35, 40, 100, 35, 1, TYPE_ROCK },  // 138 roca
  { "OMASTAR", 0, 0, R_EVO, 0x9407, 70, 60, 125, 55, 1, TYPE_ROCK },  // 139 roca
  { "KABUTO", 141, 40, R_RARO, 0x9407, 30, 80, 90, 55, 1, TYPE_ROCK },  // 140 roca
  { "KABUTOPS", 0, 0, R_EVO, 0x9407, 60, 115, 105, 80, 1, TYPE_ROCK },  // 141 roca
  { "AERODACTYL", 0, 0, R_RARO, 0x9407, 80, 105, 65, 130, 4, TYPE_ROCK },  // 142 roca
  { "SNORLAX", 0, 0, R_RARO, 0x8C4D, 160, 110, 65, 30, 0, TYPE_NORMAL },  // 143 normal
  { "ARTICUNO", 0, 0, R_LEGENDARIO, 0x4DB8, 90, 85, 100, 85, 5, TYPE_ICE },  // 144 hielo
  { "ZAPDOS", 0, 0, R_LEGENDARIO, 0xBCA1, 90, 90, 85, 100, 0, TYPE_ELECTRIC },  // 145 electrico
  { "MOLTRES", 0, 0, R_LEGENDARIO, 0xEA87, 90, 100, 90, 90, 3, TYPE_FIRE },  // 146 fuego
  { "DRATINI", 148, 30, R_RARO, 0x5A98, 41, 64, 45, 50, 1, TYPE_DRAGON },  // 147 dragon
  { "DRAGONAIR", 149, 55, R_EVO, 0x5A98, 61, 84, 65, 70, 1, TYPE_DRAGON },  // 148 dragon
  { "DRAGONITE", 0, 0, R_EVO, 0x5A98, 91, 134, 95, 80, 1, TYPE_DRAGON },  // 149 dragon
  { "MEWTWO", 0, 0, R_LEGENDARIO, 0xD28F, 106, 110, 90, 130, 0, TYPE_PSYCHIC },  // 150 psiquico
  { "MEW", 0, 0, R_LEGENDARIO, 0xD28F, 100, 100, 100, 100, 0, TYPE_PSYCHIC },  // 151 psiquico
  { "CHIKORITA", 153, 16, R_COMUN, 0x3C49, 45, 49, 65, 45, 2, TYPE_GRASS },  // 152 grass
  { "BAYLEEF", 154, 32, R_EVO, 0x3C49, 60, 62, 80, 60, 2, TYPE_GRASS },  // 153 grass
  { "MEGANIUM", 0, 0, R_EVO, 0x3C49, 80, 82, 100, 80, 2, TYPE_GRASS },  // 154 grass
  { "CYNDAQUIL", 156, 14, R_COMUN, 0xEA87, 39, 52, 43, 65, 3, TYPE_FIRE },  // 155 fire
  { "QUILAVA", 157, 36, R_EVO, 0xEA87, 58, 64, 58, 80, 3, TYPE_FIRE },  // 156 fire
  { "TYPHLOSION", 0, 0, R_EVO, 0xEA87, 78, 84, 78, 100, 3, TYPE_FIRE },  // 157 fire
  { "TOTODILE", 159, 18, R_COMUN, 0x4C98, 50, 65, 64, 43, 1, TYPE_WATER },  // 158 water
  { "CROCONAW", 160, 30, R_EVO, 0x4C98, 65, 80, 80, 58, 1, TYPE_WATER },  // 159 water
  { "FERALIGATR", 0, 0, R_EVO, 0x4C98, 85, 105, 100, 78, 1, TYPE_WATER },  // 160 water
  { "SENTRET", 162, 15, R_COMUN, 0x8C4D, 35, 46, 34, 20, 0, TYPE_NORMAL },  // 161 normal
  { "FURRET", 0, 0, R_EVO, 0x8C4D, 85, 76, 64, 90, 0, TYPE_NORMAL },  // 162 normal
  { "HOOTHOOT", 164, 20, R_COMUN, 0x8C4D, 60, 30, 30, 50, 0, TYPE_NORMAL },  // 163 normal
  { "NOCTOWL", 0, 0, R_EVO, 0x8C4D, 100, 50, 50, 70, 0, TYPE_NORMAL },  // 164 normal
  { "LEDYBA", 166, 18, R_COMUN, 0x7CC4, 40, 20, 30, 55, 2, TYPE_BUG },  // 165 bug
  { "LEDIAN", 0, 0, R_EVO, 0x7CC4, 55, 35, 50, 85, 2, TYPE_BUG },  // 166 bug
  { "SPINARAK", 168, 22, R_COMUN, 0x7CC4, 40, 60, 40, 30, 2, TYPE_BUG },  // 167 bug
  { "ARIADOS", 0, 0, R_EVO, 0x7CC4, 70, 90, 70, 40, 2, TYPE_BUG },  // 168 bug
  { "CROBAT", 0, 0, R_EVO, 0x8A73, 85, 90, 80, 130, 0, TYPE_POISON },  // 169 poison
  { "CHINCHOU", 171, 27, R_COMUN, 0x4C98, 75, 38, 38, 67, 1, TYPE_WATER },  // 170 water
  { "LANTURN", 0, 0, R_EVO, 0x4C98, 125, 58, 58, 67, 1, TYPE_WATER },  // 171 water
  { "PICHU", 25, 10, R_COMUN, 0xBCA1, 20, 40, 15, 60, 0, TYPE_ELECTRIC },  // 172 electric
  { "CLEFFA", 35, 10, R_COMUN, 0x8C4D, 50, 25, 28, 15, 0, TYPE_NORMAL },  // 173 normal (klassisch, kein Fee-Retcon)
  { "IGGLYBUFF", 39, 10, R_COMUN, 0x8C4D, 90, 30, 15, 15, 0, TYPE_NORMAL },  // 174 normal
  { "TOGEPI", 176, 10, R_RARO, 0x8C4D, 35, 20, 65, 20, 0, TYPE_NORMAL },  // 175 normal
  { "TOGETIC", 0, 0, R_EVO, 0x8C4D, 55, 40, 85, 40, 0, TYPE_NORMAL },  // 176 normal
  { "NATU", 178, 25, R_COMUN, 0xD28F, 40, 50, 45, 70, 0, TYPE_PSYCHIC },  // 177 psychic
  { "XATU", 0, 0, R_EVO, 0xD28F, 65, 75, 70, 95, 0, TYPE_PSYCHIC },  // 178 psychic
  { "MAREEP", 180, 15, R_COMUN, 0xBCA1, 55, 40, 40, 35, 0, TYPE_ELECTRIC },  // 179 electric
  { "FLAAFFY", 181, 30, R_EVO, 0xBCA1, 70, 55, 55, 45, 0, TYPE_ELECTRIC },  // 180 electric
  { "AMPHAROS", 0, 0, R_EVO, 0xBCA1, 90, 75, 85, 55, 0, TYPE_ELECTRIC },  // 181 electric
  { "BELLOSSOM", 0, 0, R_EVO, 0x3C49, 75, 80, 95, 50, 2, TYPE_GRASS },  // 182 grass
  { "MARILL", 184, 18, R_EVO, 0x4C98, 70, 20, 50, 40, 1, TYPE_WATER },  // 183 water
  { "AZUMARILL", 0, 0, R_EVO, 0x4C98, 100, 50, 80, 50, 1, TYPE_WATER },  // 184 water
  { "SUDOWOODO", 0, 0, R_COMUN, 0x9407, 70, 100, 115, 30, 4, TYPE_ROCK },  // 185 rock
  { "POLITOED", 0, 0, R_EVO, 0x4C98, 90, 75, 75, 70, 1, TYPE_WATER },  // 186 water
  { "HOPPIP", 188, 18, R_COMUN, 0x3C49, 35, 35, 40, 50, 2, TYPE_GRASS },  // 187 grass
  { "SKIPLOOM", 189, 27, R_EVO, 0x3C49, 55, 45, 50, 80, 2, TYPE_GRASS },  // 188 grass
  { "JUMPLUFF", 0, 0, R_EVO, 0x3C49, 75, 55, 70, 110, 2, TYPE_GRASS },  // 189 grass
  { "AIPOM", 0, 0, R_COMUN, 0x8C4D, 55, 70, 55, 85, 0, TYPE_NORMAL },  // 190 normal
  { "SUNKERN", 192, 20, R_COMUN, 0x3C49, 30, 30, 30, 30, 2, TYPE_GRASS },  // 191 grass
  { "SUNFLORA", 0, 0, R_EVO, 0x3C49, 75, 75, 55, 30, 2, TYPE_GRASS },  // 192 grass
  { "YANMA", 0, 0, R_RARO, 0x7CC4, 65, 65, 45, 95, 2, TYPE_BUG },  // 193 bug
  { "WOOPER", 195, 20, R_COMUN, 0x4C98, 55, 45, 45, 15, 1, TYPE_WATER },  // 194 water
  { "QUAGSIRE", 0, 0, R_EVO, 0x4C98, 95, 85, 85, 35, 1, TYPE_WATER },  // 195 water
  { "ESPEON", 0, 0, R_EVO, 0xD28F, 65, 65, 60, 110, 0, TYPE_PSYCHIC },  // 196 psychic
  { "UMBREON", 0, 0, R_EVO, 0x5A6E, 95, 65, 110, 65, 0, TYPE_DARK },  // 197 dark
  { "MURKROW", 0, 0, R_RARO, 0x5A6E, 60, 85, 42, 91, 0, TYPE_DARK },  // 198 dark
  { "SLOWKING", 0, 0, R_EVO, 0x4C98, 95, 75, 80, 30, 1, TYPE_WATER },  // 199 water
  { "MISDREAVUS", 0, 0, R_RARO, 0x6AD3, 60, 60, 60, 85, 0, TYPE_GHOST },  // 200 ghost
  { "UNOWN", 0, 0, R_RARO, 0xD28F, 48, 72, 48, 48, 0, TYPE_PSYCHIC },  // 201 psychic
  { "WOBBUFFET", 0, 0, R_EVO, 0xD28F, 190, 33, 58, 33, 0, TYPE_PSYCHIC },  // 202 psychic
  { "GIRAFARIG", 0, 0, R_COMUN, 0x8C4D, 70, 80, 65, 85, 0, TYPE_NORMAL },  // 203 normal
  { "PINECO", 205, 31, R_COMUN, 0x7CC4, 50, 65, 90, 15, 2, TYPE_BUG },  // 204 bug
  { "FORRETRESS", 0, 0, R_EVO, 0x7CC4, 75, 90, 140, 40, 2, TYPE_BUG },  // 205 bug
  { "DUNSPARCE", 0, 0, R_RARO, 0x8C4D, 100, 70, 70, 45, 0, TYPE_NORMAL },  // 206 normal
  { "GLIGAR", 0, 0, R_COMUN, 0xB447, 65, 75, 105, 85, 4, TYPE_GROUND },  // 207 ground
  { "STEELIX", 0, 0, R_EVO, 0xB447, 75, 85, 200, 30, 4, TYPE_STEEL },  // 208 steel
  { "SNUBBULL", 210, 23, R_COMUN, 0x8C4D, 60, 80, 50, 30, 0, TYPE_NORMAL },  // 209 normal
  { "GRANBULL", 0, 0, R_EVO, 0x8C4D, 90, 120, 75, 45, 0, TYPE_NORMAL },  // 210 normal
  { "QWILFISH", 0, 0, R_COMUN, 0x4C98, 65, 95, 85, 85, 1, TYPE_WATER },  // 211 water
  { "SCIZOR", 0, 0, R_EVO, 0x7CC4, 70, 130, 100, 65, 2, TYPE_BUG },  // 212 bug
  { "SHUCKLE", 0, 0, R_RARO, 0x7CC4, 20, 10, 230, 5, 2, TYPE_BUG },  // 213 bug
  { "HERACROSS", 0, 0, R_RARO, 0x7CC4, 80, 125, 75, 85, 2, TYPE_BUG },  // 214 bug
  { "SNEASEL", 0, 0, R_RARO, 0x5A6E, 55, 95, 55, 115, 0, TYPE_DARK },  // 215 dark
  { "TEDDIURSA", 217, 30, R_COMUN, 0x8C4D, 60, 80, 50, 40, 0, TYPE_NORMAL },  // 216 normal
  { "URSARING", 0, 0, R_EVO, 0x8C4D, 90, 130, 75, 55, 0, TYPE_NORMAL },  // 217 normal
  { "SLUGMA", 219, 38, R_COMUN, 0xEA87, 40, 40, 40, 20, 3, TYPE_FIRE },  // 218 fire
  { "MAGCARGO", 0, 0, R_EVO, 0xEA87, 60, 50, 120, 30, 3, TYPE_FIRE },  // 219 fire
  { "SWINUB", 221, 33, R_COMUN, 0x4DB8, 50, 50, 40, 50, 5, TYPE_ICE },  // 220 ice
  { "PILOSWINE", 0, 0, R_EVO, 0x4DB8, 100, 100, 80, 50, 5, TYPE_ICE },  // 221 ice
  { "CORSOLA", 0, 0, R_RARO, 0x4C98, 65, 55, 95, 35, 1, TYPE_WATER },  // 222 water
  { "REMORAID", 224, 25, R_COMUN, 0x4C98, 35, 65, 35, 65, 1, TYPE_WATER },  // 223 water
  { "OCTILLERY", 0, 0, R_EVO, 0x4C98, 75, 105, 75, 45, 1, TYPE_WATER },  // 224 water
  { "DELIBIRD", 0, 0, R_RARO, 0x4DB8, 45, 55, 45, 75, 5, TYPE_ICE },  // 225 ice
  { "MANTINE", 0, 0, R_RARO, 0x4C98, 85, 40, 70, 70, 1, TYPE_WATER },  // 226 water
  { "SKARMORY", 0, 0, R_RARO, 0x7C73, 65, 80, 140, 70, 4, TYPE_STEEL },  // 227 steel
  { "HOUNDOUR", 229, 24, R_COMUN, 0x5A6E, 45, 60, 30, 65, 0, TYPE_DARK },  // 228 dark
  { "HOUNDOOM", 0, 0, R_EVO, 0x5A6E, 75, 90, 50, 95, 0, TYPE_DARK },  // 229 dark
  { "KINGDRA", 0, 0, R_EVO, 0x4C98, 75, 95, 95, 85, 1, TYPE_WATER },  // 230 water
  { "PHANPY", 232, 25, R_COMUN, 0xB447, 90, 60, 60, 40, 4, TYPE_GROUND },  // 231 ground
  { "DONPHAN", 0, 0, R_EVO, 0xB447, 90, 120, 120, 50, 4, TYPE_GROUND },  // 232 ground
  { "PORYGON2", 0, 0, R_EVO, 0x8C4D, 85, 80, 90, 60, 0, TYPE_NORMAL },  // 233 normal
  { "STANTLER", 0, 0, R_RARO, 0x8C4D, 73, 95, 62, 85, 0, TYPE_NORMAL },  // 234 normal
  { "SMEARGLE", 0, 0, R_RARO, 0x8C4D, 55, 20, 35, 75, 0, TYPE_NORMAL },  // 235 normal
  { "TYROGUE", 106, 20, R_RARO, 0xA2A5, 35, 35, 35, 35, 0, TYPE_FIGHTING },  // 236 fighting; Lv20 -> Zweig 106/107/237 (BRANCHES in pet.cpp waehlt zufaellig)
  { "HITMONTOP", 0, 0, R_EVO, 0xA2A5, 50, 95, 95, 70, 0, TYPE_FIGHTING },  // 237 fighting
  { "SMOOCHUM", 124, 30, R_RARO, 0x4DB8, 45, 30, 15, 65, 5, TYPE_ICE },  // 238 ice
  { "ELEKID", 125, 30, R_RARO, 0xBCA1, 45, 63, 37, 95, 0, TYPE_ELECTRIC },  // 239 electric
  { "MAGBY", 126, 30, R_RARO, 0xEA87, 45, 75, 37, 83, 3, TYPE_FIRE },  // 240 fire
  { "MILTANK", 0, 0, R_RARO, 0x8C4D, 95, 80, 105, 100, 0, TYPE_NORMAL },  // 241 normal
  { "BLISSEY", 0, 0, R_EVO, 0x8C4D, 255, 10, 10, 55, 0, TYPE_NORMAL },  // 242 normal
  { "RAIKOU", 0, 0, R_LEGENDARIO, 0xBCA1, 90, 85, 75, 115, 0, TYPE_ELECTRIC },  // 243 electric
  { "ENTEI", 0, 0, R_LEGENDARIO, 0xEA87, 115, 115, 85, 100, 3, TYPE_FIRE },  // 244 fire
  { "SUICUNE", 0, 0, R_LEGENDARIO, 0x4C98, 100, 75, 115, 85, 1, TYPE_WATER },  // 245 water
  { "LARVITAR", 247, 30, R_RARO, 0x9407, 50, 64, 50, 41, 4, TYPE_ROCK },  // 246 rock
  { "PUPITAR", 248, 55, R_EVO, 0x9407, 70, 84, 70, 51, 4, TYPE_ROCK },  // 247 rock
  { "TYRANITAR", 0, 0, R_EVO, 0x9407, 100, 134, 110, 61, 4, TYPE_ROCK },  // 248 rock
  { "LUGIA", 0, 0, R_LEGENDARIO, 0xD28F, 106, 90, 130, 110, 0, TYPE_PSYCHIC },  // 249 psychic
  { "HO-OH", 0, 0, R_LEGENDARIO, 0xEA87, 106, 130, 90, 90, 3, TYPE_FIRE },  // 250 fire
  { "CELEBI", 0, 0, R_LEGENDARIO, 0xD28F, 100, 100, 100, 100, 0, TYPE_PSYCHIC },  // 251 psychic
  { "TREECKO", 253, 16, R_COMUN, 0x3C49, 40, 45, 35, 70, 2, TYPE_GRASS },  // 252 grass
  { "GROVYLE", 254, 36, R_EVO, 0x3C49, 50, 65, 45, 95, 2, TYPE_GRASS },  // 253 grass
  { "SCEPTILE", 0, 0, R_EVO, 0x3C49, 70, 85, 65, 120, 2, TYPE_GRASS },  // 254 grass
  { "TORCHIC", 256, 16, R_COMUN, 0xEA87, 45, 60, 40, 45, 3, TYPE_FIRE },  // 255 fire
  { "COMBUSKEN", 257, 36, R_EVO, 0xEA87, 60, 85, 60, 55, 3, TYPE_FIRE },  // 256 fire
  { "BLAZIKEN", 0, 0, R_EVO, 0xEA87, 80, 120, 70, 80, 3, TYPE_FIRE },  // 257 fire
  { "MUDKIP", 259, 16, R_COMUN, 0x4C98, 50, 70, 50, 40, 1, TYPE_WATER },  // 258 water
  { "MARSHTOMP", 260, 36, R_EVO, 0x4C98, 70, 85, 70, 50, 1, TYPE_WATER },  // 259 water
  { "SWAMPERT", 0, 0, R_EVO, 0x4C98, 100, 110, 90, 60, 1, TYPE_WATER },  // 260 water
  { "POOCHYENA", 262, 18, R_COMUN, 0x5A6E, 35, 55, 35, 35, 0, TYPE_DARK },  // 261 dark
  { "MIGHTYENA", 0, 0, R_EVO, 0x5A6E, 70, 90, 70, 70, 0, TYPE_DARK },  // 262 dark
  { "ZIGZAGOON", 264, 20, R_COMUN, 0x8C4D, 38, 30, 41, 60, 0, TYPE_NORMAL },  // 263 normal
  { "LINOONE", 0, 0, R_EVO, 0x8C4D, 78, 70, 61, 100, 0, TYPE_NORMAL },  // 264 normal
  { "WURMPLE", 266, 7, R_COMUN, 0x7CC4, 45, 45, 35, 20, 2, TYPE_BUG },  // 265 bug
  { "SILCOON", 267, 10, R_EVO, 0x7CC4, 50, 35, 55, 15, 2, TYPE_BUG },  // 266 bug
  { "BEAUTIFLY", 0, 0, R_EVO, 0x7CC4, 60, 70, 50, 65, 2, TYPE_BUG },  // 267 bug
  { "CASCOON", 269, 10, R_EVO, 0x7CC4, 50, 35, 55, 15, 2, TYPE_BUG },  // 268 bug
  { "DUSTOX", 0, 0, R_EVO, 0x7CC4, 60, 50, 70, 65, 2, TYPE_BUG },  // 269 bug
  { "LOTAD", 271, 14, R_COMUN, 0x4C98, 40, 30, 30, 30, 1, TYPE_WATER },  // 270 water
  { "LOMBRE", 272, 30, R_EVO, 0x4C98, 60, 50, 50, 50, 1, TYPE_WATER },  // 271 water
  { "LUDICOLO", 0, 0, R_EVO, 0x4C98, 80, 70, 70, 70, 1, TYPE_WATER },  // 272 water
  { "SEEDOT", 274, 14, R_COMUN, 0x3C49, 40, 40, 50, 30, 2, TYPE_GRASS },  // 273 grass
  { "NUZLEAF", 275, 30, R_EVO, 0x3C49, 70, 70, 40, 60, 2, TYPE_GRASS },  // 274 grass
  { "SHIFTRY", 0, 0, R_EVO, 0x3C49, 90, 100, 60, 80, 2, TYPE_GRASS },  // 275 grass
  { "TAILLOW", 277, 22, R_COMUN, 0x8C4D, 40, 55, 30, 85, 0, TYPE_NORMAL },  // 276 normal
  { "SWELLOW", 0, 0, R_EVO, 0x8C4D, 60, 85, 60, 125, 0, TYPE_NORMAL },  // 277 normal
  { "WINGULL", 279, 25, R_COMUN, 0x4C98, 40, 30, 30, 85, 1, TYPE_WATER },  // 278 water
  { "PELIPPER", 0, 0, R_EVO, 0x4C98, 60, 50, 100, 65, 1, TYPE_WATER },  // 279 water
  { "RALTS", 281, 20, R_COMUN, 0xD28F, 28, 25, 25, 40, 0, TYPE_PSYCHIC },  // 280 psychic
  { "KIRLIA", 282, 30, R_EVO, 0xD28F, 38, 35, 35, 50, 0, TYPE_PSYCHIC },  // 281 psychic
  { "GARDEVOIR", 0, 0, R_EVO, 0xD28F, 68, 65, 65, 80, 0, TYPE_PSYCHIC },  // 282 psychic
  { "SURSKIT", 284, 22, R_COMUN, 0x7CC4, 40, 30, 32, 65, 2, TYPE_BUG },  // 283 bug
  { "MASQUERAIN", 0, 0, R_EVO, 0x7CC4, 70, 60, 62, 80, 2, TYPE_BUG },  // 284 bug
  { "SHROOMISH", 286, 23, R_COMUN, 0x3C49, 60, 40, 60, 35, 2, TYPE_GRASS },  // 285 grass
  { "BRELOOM", 0, 0, R_EVO, 0x3C49, 60, 130, 80, 70, 2, TYPE_GRASS },  // 286 grass
  { "SLAKOTH", 288, 18, R_COMUN, 0x8C4D, 60, 60, 60, 30, 0, TYPE_NORMAL },  // 287 normal
  { "VIGOROTH", 289, 36, R_EVO, 0x8C4D, 80, 80, 80, 90, 0, TYPE_NORMAL },  // 288 normal
  { "SLAKING", 0, 0, R_EVO, 0x8C4D, 150, 160, 100, 100, 0, TYPE_NORMAL },  // 289 normal
  { "NINCADA", 291, 20, R_COMUN, 0x7CC4, 31, 45, 90, 40, 2, TYPE_BUG },  // 290 bug
  { "NINJASK", 0, 0, R_EVO, 0x7CC4, 61, 90, 45, 160, 2, TYPE_BUG },  // 291 bug
  { "SHEDINJA", 0, 0, R_EVO, 0x7CC4, 1, 90, 45, 40, 2, TYPE_BUG },  // 292 bug
  { "WHISMUR", 294, 20, R_COMUN, 0x8C4D, 64, 51, 23, 28, 0, TYPE_NORMAL },  // 293 normal
  { "LOUDRED", 295, 40, R_EVO, 0x8C4D, 84, 71, 43, 48, 0, TYPE_NORMAL },  // 294 normal
  { "EXPLOUD", 0, 0, R_EVO, 0x8C4D, 104, 91, 63, 68, 0, TYPE_NORMAL },  // 295 normal
  { "MAKUHITA", 297, 24, R_COMUN, 0xA2A5, 72, 60, 30, 25, 0, TYPE_FIGHTING },  // 296 fighting
  { "HARIYAMA", 0, 0, R_EVO, 0xA2A5, 144, 120, 60, 50, 0, TYPE_FIGHTING },  // 297 fighting
  { "AZURILL", 183, 10, R_COMUN, 0x8C4D, 50, 20, 40, 20, 0, TYPE_NORMAL },  // 298 normal
  { "NOSEPASS", 0, 0, R_COMUN, 0x9407, 30, 45, 135, 30, 4, TYPE_ROCK },  // 299 rock
  { "SKITTY", 301, 30, R_COMUN, 0x8C4D, 50, 45, 45, 50, 0, TYPE_NORMAL },  // 300 normal
  { "DELCATTY", 0, 0, R_EVO, 0x8C4D, 70, 65, 65, 90, 0, TYPE_NORMAL },  // 301 normal
  { "SABLEYE", 0, 0, R_RARO, 0x5A6E, 50, 75, 75, 50, 0, TYPE_DARK },  // 302 dark
  { "MAWILE", 0, 0, R_RARO, 0x7C73, 50, 85, 85, 50, 4, TYPE_STEEL },  // 303 steel
  { "ARON", 305, 32, R_COMUN, 0x7C73, 50, 70, 100, 30, 4, TYPE_STEEL },  // 304 steel
  { "LAIRON", 306, 42, R_EVO, 0x7C73, 60, 90, 140, 40, 4, TYPE_STEEL },  // 305 steel
  { "AGGRON", 0, 0, R_EVO, 0x7C73, 70, 110, 180, 50, 4, TYPE_STEEL },  // 306 steel
  { "MEDITITE", 308, 37, R_COMUN, 0xA2A5, 30, 40, 55, 60, 0, TYPE_FIGHTING },  // 307 fighting
  { "MEDICHAM", 0, 0, R_EVO, 0xA2A5, 60, 60, 75, 80, 0, TYPE_FIGHTING },  // 308 fighting
  { "ELECTRIKE", 310, 26, R_COMUN, 0xBCA1, 40, 45, 40, 65, 0, TYPE_ELECTRIC },  // 309 electric
  { "MANECTRIC", 0, 0, R_EVO, 0xBCA1, 70, 75, 60, 105, 0, TYPE_ELECTRIC },  // 310 electric
  { "PLUSLE", 0, 0, R_COMUN, 0xBCA1, 60, 50, 40, 95, 0, TYPE_ELECTRIC },  // 311 electric
  { "MINUN", 0, 0, R_COMUN, 0xBCA1, 60, 40, 50, 95, 0, TYPE_ELECTRIC },  // 312 electric
  { "VOLBEAT", 0, 0, R_COMUN, 0x7CC4, 65, 73, 75, 85, 2, TYPE_BUG },  // 313 bug
  { "ILLUMISE", 0, 0, R_COMUN, 0x7CC4, 65, 47, 75, 85, 2, TYPE_BUG },  // 314 bug
  { "ROSELIA", 0, 0, R_COMUN, 0x3C49, 50, 60, 45, 65, 2, TYPE_GRASS },  // 315 grass
  { "GULPIN", 317, 26, R_COMUN, 0x8A73, 70, 43, 53, 40, 0, TYPE_POISON },  // 316 poison
  { "SWALOT", 0, 0, R_EVO, 0x8A73, 100, 73, 83, 55, 0, TYPE_POISON },  // 317 poison
  { "CARVANHA", 319, 30, R_COMUN, 0x4C98, 45, 90, 20, 65, 1, TYPE_WATER },  // 318 water
  { "SHARPEDO", 0, 0, R_EVO, 0x4C98, 70, 120, 40, 95, 1, TYPE_WATER },  // 319 water
  { "WAILMER", 321, 40, R_COMUN, 0x4C98, 130, 70, 35, 60, 1, TYPE_WATER },  // 320 water
  { "WAILORD", 0, 0, R_EVO, 0x4C98, 170, 90, 45, 60, 1, TYPE_WATER },  // 321 water
  { "NUMEL", 323, 33, R_COMUN, 0xEA87, 60, 60, 40, 35, 3, TYPE_FIRE },  // 322 fire
  { "CAMERUPT", 0, 0, R_EVO, 0xEA87, 70, 100, 70, 40, 3, TYPE_FIRE },  // 323 fire
  { "TORKOAL", 0, 0, R_RARO, 0xEA87, 70, 85, 140, 20, 3, TYPE_FIRE },  // 324 fire
  { "SPOINK", 326, 32, R_COMUN, 0xD28F, 60, 25, 35, 60, 0, TYPE_PSYCHIC },  // 325 psychic
  { "GRUMPIG", 0, 0, R_EVO, 0xD28F, 80, 45, 65, 80, 0, TYPE_PSYCHIC },  // 326 psychic
  { "SPINDA", 0, 0, R_RARO, 0x8C4D, 60, 60, 60, 60, 0, TYPE_NORMAL },  // 327 normal
  { "TRAPINCH", 329, 35, R_COMUN, 0xB447, 45, 100, 45, 10, 4, TYPE_GROUND },  // 328 ground
  { "VIBRAVA", 330, 45, R_EVO, 0xB447, 50, 70, 50, 70, 4, TYPE_GROUND },  // 329 ground
  { "FLYGON", 0, 0, R_EVO, 0xB447, 80, 100, 80, 100, 4, TYPE_GROUND },  // 330 ground
  { "CACNEA", 332, 32, R_COMUN, 0x3C49, 50, 85, 40, 35, 2, TYPE_GRASS },  // 331 grass
  { "CACTURNE", 0, 0, R_EVO, 0x3C49, 70, 115, 60, 55, 2, TYPE_GRASS },  // 332 grass
  { "SWABLU", 334, 35, R_COMUN, 0x8C4D, 45, 40, 60, 50, 0, TYPE_NORMAL },  // 333 normal
  { "ALTARIA", 0, 0, R_EVO, 0x5A98, 75, 70, 90, 80, 1, TYPE_DRAGON },  // 334 dragon
  { "ZANGOOSE", 0, 0, R_RARO, 0x8C4D, 73, 115, 60, 90, 0, TYPE_NORMAL },  // 335 normal
  { "SEVIPER", 0, 0, R_RARO, 0x8A73, 73, 100, 60, 65, 0, TYPE_POISON },  // 336 poison
  { "LUNATONE", 0, 0, R_RARO, 0x9407, 90, 55, 65, 70, 4, TYPE_ROCK },  // 337 rock
  { "SOLROCK", 0, 0, R_RARO, 0x9407, 90, 95, 85, 70, 4, TYPE_ROCK },  // 338 rock
  { "BARBOACH", 340, 30, R_COMUN, 0x4C98, 50, 48, 43, 60, 1, TYPE_WATER },  // 339 water
  { "WHISCASH", 0, 0, R_EVO, 0x4C98, 110, 78, 73, 60, 1, TYPE_WATER },  // 340 water
  { "CORPHISH", 342, 30, R_COMUN, 0x4C98, 43, 80, 65, 35, 1, TYPE_WATER },  // 341 water
  { "CRAWDAUNT", 0, 0, R_EVO, 0x4C98, 63, 120, 85, 55, 1, TYPE_WATER },  // 342 water
  { "BALTOY", 344, 36, R_COMUN, 0xB447, 40, 40, 55, 55, 4, TYPE_GROUND },  // 343 ground
  { "CLAYDOL", 0, 0, R_EVO, 0xB447, 60, 70, 105, 75, 4, TYPE_GROUND },  // 344 ground
  { "LILEEP", 346, 40, R_RARO, 0x9407, 66, 41, 77, 23, 4, TYPE_ROCK },  // 345 rock
  { "CRADILY", 0, 0, R_EVO, 0x9407, 86, 81, 97, 43, 4, TYPE_ROCK },  // 346 rock
  { "ANORITH", 348, 40, R_RARO, 0x9407, 45, 95, 50, 75, 4, TYPE_ROCK },  // 347 rock
  { "ARMALDO", 0, 0, R_EVO, 0x9407, 75, 125, 100, 45, 4, TYPE_ROCK },  // 348 rock
  { "FEEBAS", 350, 30, R_RARO, 0x4C98, 20, 15, 20, 80, 1, TYPE_WATER },  // 349 water
  { "MILOTIC", 0, 0, R_EVO, 0x4C98, 95, 60, 79, 81, 1, TYPE_WATER },  // 350 water
  { "CASTFORM", 0, 0, R_COMUN, 0x8C4D, 70, 70, 70, 70, 0, TYPE_NORMAL },  // 351 normal
  { "KECLEON", 0, 0, R_RARO, 0x8C4D, 60, 90, 70, 40, 0, TYPE_NORMAL },  // 352 normal
  { "SHUPPET", 354, 37, R_COMUN, 0x6AD3, 44, 75, 35, 45, 0, TYPE_GHOST },  // 353 ghost
  { "BANETTE", 0, 0, R_EVO, 0x6AD3, 64, 115, 65, 65, 0, TYPE_GHOST },  // 354 ghost
  { "DUSKULL", 356, 37, R_COMUN, 0x6AD3, 20, 40, 90, 25, 0, TYPE_GHOST },  // 355 ghost
  { "DUSCLOPS", 0, 0, R_EVO, 0x6AD3, 40, 70, 130, 25, 0, TYPE_GHOST },  // 356 ghost
  { "TROPIUS", 0, 0, R_RARO, 0x3C49, 99, 68, 83, 51, 2, TYPE_GRASS },  // 357 grass
  { "CHIMECHO", 0, 0, R_COMUN, 0xD28F, 75, 50, 80, 65, 0, TYPE_PSYCHIC },  // 358 psychic
  { "ABSOL", 0, 0, R_RARO, 0x5A6E, 65, 130, 60, 75, 0, TYPE_DARK },  // 359 dark
  { "WYNAUT", 202, 15, R_COMUN, 0xD28F, 95, 23, 48, 23, 0, TYPE_PSYCHIC },  // 360 psychic
  { "SNORUNT", 362, 42, R_RARO, 0x4DB8, 50, 50, 50, 50, 5, TYPE_ICE },  // 361 ice
  { "GLALIE", 0, 0, R_EVO, 0x4DB8, 80, 80, 80, 80, 5, TYPE_ICE },  // 362 ice
  { "SPHEAL", 364, 32, R_COMUN, 0x4DB8, 70, 40, 50, 25, 5, TYPE_ICE },  // 363 ice
  { "SEALEO", 365, 44, R_EVO, 0x4DB8, 90, 60, 70, 45, 5, TYPE_ICE },  // 364 ice
  { "WALREIN", 0, 0, R_EVO, 0x4DB8, 110, 80, 90, 65, 5, TYPE_ICE },  // 365 ice
  { "CLAMPERL", 367, 40, R_COMUN, 0x4C98, 35, 64, 85, 32, 1, TYPE_WATER },  // 366 water
  { "HUNTAIL", 0, 0, R_EVO, 0x4C98, 55, 104, 105, 52, 1, TYPE_WATER },  // 367 water
  { "GOREBYSS", 0, 0, R_EVO, 0x4C98, 55, 84, 105, 52, 1, TYPE_WATER },  // 368 water
  { "RELICANTH", 0, 0, R_RARO, 0x4C98, 100, 90, 130, 55, 1, TYPE_WATER },  // 369 water
  { "LUVDISC", 0, 0, R_RARO, 0x4C98, 43, 30, 55, 97, 1, TYPE_WATER },  // 370 water
  { "BAGON", 372, 30, R_RARO, 0x5A98, 45, 75, 60, 50, 1, TYPE_DRAGON },  // 371 dragon
  { "SHELGON", 373, 50, R_EVO, 0x5A98, 65, 95, 100, 50, 1, TYPE_DRAGON },  // 372 dragon
  { "SALAMENCE", 0, 0, R_EVO, 0x5A98, 95, 135, 80, 100, 1, TYPE_DRAGON },  // 373 dragon
  { "BELDUM", 375, 20, R_RARO, 0x7C73, 40, 55, 80, 30, 4, TYPE_STEEL },  // 374 steel
  { "METANG", 376, 45, R_EVO, 0x7C73, 60, 75, 100, 50, 4, TYPE_STEEL },  // 375 steel
  { "METAGROSS", 0, 0, R_EVO, 0x7C73, 80, 135, 130, 70, 4, TYPE_STEEL },  // 376 steel
  { "REGIROCK", 0, 0, R_LEGENDARIO, 0x9407, 80, 100, 200, 50, 4, TYPE_ROCK },  // 377 rock
  { "REGICE", 0, 0, R_LEGENDARIO, 0x4DB8, 80, 50, 100, 50, 5, TYPE_ICE },  // 378 ice
  { "REGISTEEL", 0, 0, R_LEGENDARIO, 0x7C73, 80, 75, 150, 50, 4, TYPE_STEEL },  // 379 steel
  { "LATIAS", 0, 0, R_LEGENDARIO, 0x5A98, 80, 80, 90, 110, 1, TYPE_DRAGON },  // 380 dragon
  { "LATIOS", 0, 0, R_LEGENDARIO, 0x5A98, 80, 90, 80, 110, 1, TYPE_DRAGON },  // 381 dragon
  { "KYOGRE", 0, 0, R_LEGENDARIO, 0x4C98, 100, 100, 90, 90, 1, TYPE_WATER },  // 382 water
  { "GROUDON", 0, 0, R_LEGENDARIO, 0xB447, 100, 150, 140, 90, 4, TYPE_GROUND },  // 383 ground
  { "RAYQUAZA", 0, 0, R_LEGENDARIO, 0x5A98, 105, 150, 90, 95, 1, TYPE_DRAGON },  // 384 dragon
  { "JIRACHI", 0, 0, R_LEGENDARIO, 0x7C73, 100, 100, 100, 100, 4, TYPE_STEEL },  // 385 steel
  { "DEOXYS", 0, 0, R_LEGENDARIO, 0xD28F, 50, 150, 50, 150, 0, TYPE_PSYCHIC },  // 386 psychic
};

// Nombres oficiales de DE (en gen 1 es el unico que difiere del
// ingles). nullptr = sin nombre propio.
static const char *const DEX_NAME_DE[DEX_COUNT + 1] = {
  nullptr, "BISASAM", "BISAKNOSP", "BISAFLOR",
  "GLUMANDA", "GLUTEXO", "GLURAK", "SCHIGGY",
  "SCHILLOK", "TURTOK", "RAUPY", "SAFCON",
  "SMETTBO", "HORNLIU", "KOKUNA", "BIBOR",
  "TAUBSI", "TAUBOGA", "TAUBOSS", "RATTFRATZ",
  "RATTIKARL", "HABITAK", "IBITAK", "RETTAN",
  nullptr, nullptr, nullptr, "SANDAN",
  "SANDAMER", "NIDORAN w", nullptr, nullptr,
  nullptr, nullptr, nullptr, "PIEPI",
  "PIXI", nullptr, "VULNONA", "PUMMELUFF",
  "KNUDDELUFF", nullptr, nullptr, "MYRAPLA",
  "DUFLOR", "GIFLOR", nullptr, "PARASEK",
  "BLUZUK", "OMOT", "DIGDA", "DIGDRI",
  "MAUZI", "SNOBILIKAT", "ENTON", "ENTORON",
  "MENKI", "RASAFF", "FUKANO", "ARKANI",
  "QUAPSEL", "QUAPUTZI", "QUAPPO", nullptr,
  nullptr, "SIMSALA", "MACHOLLO", "MASCHOCK",
  "MACHOMEI", "KNOFENSA", "ULTRIGARIA", "SARZENIA",
  "TENTACHA", "TENTOXA", "KLEINSTEIN", "GEOROK",
  "GEOWAZ", "PONITA", "GALLOPA", "FLEGMON",
  "LAHMUS", "MAGNETILO", nullptr, "PORENTA",
  "DODU", "DODRI", "JUROB", "JUGONG",
  "SLEIMA", "SLEIMOK", "MUSCHAS", "AUSTOS",
  "NEBULAK", "ALPOLLO", nullptr, nullptr,
  "TRAUMATO", nullptr, nullptr, nullptr,
  "VOLTOBAL", "LEKTROBAL", "OWEI", "KOKOWEI",
  "TRAGOSSO", "KNOGGA", "KICKLEE", "NOCKCHAN",
  "SCHLURP", "SMOGON", "SMOGMOG", "RIHORN",
  "RIZEROS", "CHANEIRA", nullptr, "KANGAMA",
  "SEEPER", "SEEMON", "GOLDINI", "GOLKING",
  "STERNDU", nullptr, "PANTIMOS", "SICHLOR",
  "ROSSANA", "ELEKTEK", nullptr, nullptr,
  nullptr, "KARPADOR", "GARADOS", nullptr,
  nullptr, "EVOLI", "AQUANA", "BLITZA",
  "FLAMARA", nullptr, "AMONITAS", "AMOROSO",
  nullptr, nullptr, nullptr, "RELAXO",
  "ARKTOS", nullptr, "LAVADOS", nullptr,
  "DRAGONIR", "DRAGORAN", "MEWTU", nullptr,
  "ENDIVIE",
  "LORBLATT",
  "MEGANIE",
  "FEURIGEL",
  "IGELAVAR",
  "TORNUPTO",
  "KARNIMANI",
  "TYRACROC",
  "IMPERGATOR",
  "WIESOR",
  "WIESENIOR",
  nullptr,
  "NOCTUH",
  nullptr,
  nullptr,
  "WEBARAK",
  nullptr,
  "IKSBAT",
  "LAMPI",
  nullptr,
  nullptr,
  "PII",
  "FLUFFELUFF",
  nullptr,
  nullptr,
  nullptr,
  nullptr,
  "VOLTILAMM",
  "WAATY",
  nullptr,
  "BLUBELLA",
  nullptr,
  nullptr,
  "MOGELBAUM",
  "QUAXO",
  "HOPPSPROSS",
  "HUBELUPF",
  "PAPUNGHA",
  "GRIFFEL",
  "SONNKERN",
  "SONNFLORA",
  nullptr,
  "FELINO",
  "MORLORD",
  "PSIANA",
  "NACHTARA",
  "KRAMURX",
  "LASCHOKING",
  "TRAUNFUGIL",
  "ICOGNITO",
  "WOINGENAU",
  nullptr,
  "TANNZA",
  "FORSTELLKA",
  "DUMMISEL",
  "SKORGLA",
  "STAHLOS",
  nullptr,
  nullptr,
  "BALDORFISH",
  "SCHEROX",
  "POTTROTT",
  "SKARABORN",
  "SNIEBEL",
  nullptr,
  nullptr,
  "SCHNECKMAG",
  nullptr,
  "QUIEKEL",
  "KEIFEL",
  "CORASONN",
  nullptr,
  nullptr,
  "BOTOGEL",
  "MANTAX",
  "PANZAERON",
  "HUNDUSTER",
  "HUNDEMON",
  "SEEDRAKING",
  nullptr,
  nullptr,
  nullptr,
  "DAMHIRPLEX",
  "FARBEAGLE",
  "RABAUZ",
  "KAPOERA",
  "KUSSILLA",
  nullptr,
  nullptr,
  nullptr,
  "HEITEIRA",
  nullptr,
  nullptr,
  nullptr,
  nullptr,
  nullptr,
  "DESPOTAR",
  nullptr,
  nullptr,
  nullptr,
  "GECKARBOR",
  "REPTAIN",
  "GEWALDRO",
  "FLEMMLI",
  "JUNGGLUT",
  "LOHGOCK",
  "HYDROPI",
  "MOORABBEL",
  "SUMPEX",
  "FIFFYEN",
  "MAGNAYEN",
  "ZIGZACHS",
  "GERADAKS",
  "WAUMPEL",
  "SCHALOKO",
  "PAPINELLA",
  "PANEKON",
  "PUDOX",
  "LOTURZEL",
  "LOMBRERO",
  "KAPPALORES",
  "SAMURZEL",
  "BLANAS",
  "TENGULIST",
  "SCHWALBINI",
  "SCHWALBOSS",
  nullptr,
  nullptr,
  "TRASLA",
  nullptr,
  "GUARDEVOIR",
  "GEHWEIHER",
  "MASKEREGEN",
  "KNILZ",
  "KAPILZ",
  "BUMMELZ",
  "MUNTIER",
  "LETARKING",
  nullptr,
  nullptr,
  "NINJATOM",
  "FLURMEL",
  "KRAKEELO",
  "KRAWUMMS",
  nullptr,
  nullptr,
  nullptr,
  "NASGNET",
  "ENECO",
  "ENEKORO",
  "ZOBIRIS",
  "FLUNKIFER",
  "STOLLUNIOR",
  "STOLLRAK",
  "STOLLOSS",
  "MEDITIE",
  "MEDITALIS",
  "FRIZELBLIZ",
  "VOLTENSO",
  nullptr,
  nullptr,
  nullptr,
  nullptr,
  nullptr,
  "SCHLUPPUCK",
  "SCHLUKWECH",
  "KANIVANHA",
  "TOHAIDO",
  nullptr,
  nullptr,
  "CAMAUB",
  nullptr,
  "QURTEL",
  nullptr,
  "GROINK",
  "PANDIR",
  "KNACKLION",
  nullptr,
  "LIBELLDRA",
  "TUSKA",
  "NOKTUSKA",
  "WABLU",
  nullptr,
  "SENGO",
  "VIPITIS",
  "LUNASTEIN",
  "SONNFEL",
  "SCHMERBE",
  "WELSAR",
  "KREBSCORPS",
  "KREBUTACK",
  "PUPPANCE",
  "LEPUMENTAS",
  "LILIEP",
  "WIELIE",
  nullptr,
  nullptr,
  "BARSCHWA",
  nullptr,
  "FORMEO",
  nullptr,
  nullptr,
  nullptr,
  "ZWIRRLICHT",
  "ZWIRRKLOP",
  nullptr,
  "PALIMPALIM",
  nullptr,
  "ISSO",
  "SCHNEPPKE",
  "FIRNONTOR",
  "SEEMOPS",
  "SEEJONG",
  "WALRAISA",
  "PERLU",
  "AALABYSS",
  "SAGANABYSS",
  nullptr,
  "LIEBISKUS",
  "KINDWURM",
  "DRASCHEL",
  "BRUTALANDA",
  "TANHEL",
  nullptr,
  nullptr,
  nullptr,
  nullptr,
  nullptr,
  nullptr,
  nullptr,
  nullptr,
  nullptr,
  nullptr,
  nullptr,
  nullptr,
};

// Nombre de la especie en el idioma activo (cae al de DEX_TBL si ese
// idioma no tiene nombre propio para ella).
static inline const char *dexName(int16_t dex) {
  if (dex < 1 || dex > DEX_COUNT) return DEX_TBL[0].name;
  const char *n = (gLang == LANG_DE || gLang == LANG_MIX) ? DEX_NAME_DE[dex] : nullptr;
  return n ? n : DEX_TBL[dex].name;
}

// el primer huevo de la partida: iniciales clasicos
static const int16_t CLASSIC_DEX[] = { 1, 4, 7, 25, 133 };
#define NUM_CLASSIC_DEX 5
