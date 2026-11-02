/**
 * @file levels/drlg_l3.h
 *
 * Interface of the caves level generation algorithms.
 */
#pragma once

#include <cstdint>

#include "levels/gendung.h"

namespace devilution {

void CreateL3Dungeon(uint32_t rseed, lvl_entry entry);
void LoadPreL3Dungeon(const char *sFileName);
void LoadL3Dungeon(const char *sFileName, Point spawn);
/** [DARK KEEP] See keeper_map.cpp. `stairs*` are megatile positions, x < 0 for none. */
void KeeperL3Build(Point stairsUp, Point stairsDown, const uint8_t gates[DMAXX][DMAXY], bool decorate);
void KeeperL3Pass3();

} // namespace devilution
