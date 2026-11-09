/**
 * @file keeper/keeper_lighting.hpp
 *
 * The keep's lighting: a dark ambient per tileset, light baked in at the entrance and the vault, and
 * torchlight carried by every hero, so a raid reads as lights moving through the dark. The whole keep
 * stays visible (RevealKeep); only how bright it is changes.
 *
 * How the engine lights a level (lighting.cpp): dLight is what is drawn, 0 = fully lit, 15 = black.
 * dPreLight holds the static light (map objects, lava) and every moving light is drawn on top of it,
 * each tile taking the brightest (lowest) value. A light's falloff runs linearly from 0 at its centre to
 * 15 at radius + 1 tiles, so against an ambient of A a light of radius r brightens the floor for about
 * A * (r + 1) / 15 tiles. This module keeps its own copy of the static light without the ambient, and
 * dPreLight = min(static, ambient): moving lights then restore and redraw against it as usual.
 *
 * Client-side and visual only: dLight is read only by drawing, the light slots aren't synced, and
 * nothing here takes a random number or changes the simulation.
 */
#pragma once

#include <cstdint>
#include <string>

#include "engine/point.hpp"

namespace devilution::keeper {

/** On entering the keep, after ResetGame: bakes the fixed lights and frees the masters' unused lights. */
void LightingOnLevelLoaded(Point entrance, Point vault);
/** The editor rebuilt the level in place: takes the static light again from a cleared dLight. */
void LightingBeforeLevelRebuild();
void LightingAfterLevelRebuild(Point entrance, Point vault);
/** Every keep tick on each client: heroes' lights follow them, the ambient follows the phase. */
void LightingTick();

/**
 * How bright to draw a monster standing on a tile this lit. In the keep every monster is drawn as the
 * engine draws other players (5 steps brighter), so the master's army doesn't vanish into its own dark.
 */
int MonsterLightIndex(int lightTableIndex);

/**
 * Each frame before drawing the keep: the rooms seen from the cursor's tile, cast as a hero's sight is.
 * Their walls are drawn see-through just as walls in a hero's sight are, so the master can look past a
 * wall at what the cursor is on. Drawing only: no tile becomes visible or explored.
 */
void UpdateCursorSeeThrough();
/** Whether a room's walls (dTransVal) are drawn see-through: a hero or the cursor sees into it. */
bool IsRoomSeeThrough(int8_t trans);

/** Dev/test: overrides the ambient (prep and raid; -1 = the tileset's own). */
void DevSetAmbient(int prep, int raid);
/** Dev/test: one line about the lighting now. */
std::string DevLightingReport();
/** Dev/test: the light at a tile, drawn, static with the ambient, and static alone. */
std::string DevLightAt(Point tile);

} // namespace devilution::keeper
