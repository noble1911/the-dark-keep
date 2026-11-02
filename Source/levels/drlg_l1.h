/**
 * @file levels/drlg_l1.h
 *
 * Interface of the cathedral level generation algorithms.
 */
#pragma once

#include <cstdint>

#include "engine/world_tile.hpp"
#include "levels/gendung.h"

namespace devilution {

void PlaceMiniSetRandom(const Miniset &miniset, int rndper);
WorldTilePosition SelectChamber();
void CreateL5Dungeon(uint32_t rseed, lvl_entry entry);
void LoadPreL1Dungeon(const char *path);
void LoadL1Dungeon(const char *path, Point spawn);
/** [DARK KEEP] See keeper_map.cpp. `stairs*` are megatile positions, x < 0 for none. Also builds the Crypt. */
void KeeperL1Build(Point stairsUp, Point stairsDown, const uint8_t openings[DMAXX][DMAXY], bool decorate);
void KeeperL1Pass3();

} // namespace devilution
