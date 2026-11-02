/**
 * @file levels/drlg_l2.h
 *
 * Interface of the catacombs level generation algorithms.
 */
#pragma once

#include <cstdint>

#include "levels/gendung.h"

namespace devilution {

void CreateL2Dungeon(uint32_t rseed, lvl_entry entry);
void LoadPreL2Dungeon(const char *path);
void LoadL2Dungeon(const char *path, Point spawn);
/** [DARK KEEP] See keeper_map.cpp. `stairs*` are megatile positions, x < 0 for none. */
void KeeperL2Build(const char grid[DMAXX][DMAXY], Point stairsUp, Point stairsDown, bool decorate);
void KeeperL2Pass3();

} // namespace devilution
