/**
 * @file keeper/keeper.hpp
 *
 * The Dark Keep: the player is the dungeon master, not a hero.
 *
 * This module owns everything that differs from the stock hero game:
 * the bodyless master (parked, hidden, untargetable player entity), the
 * free camera, the master HUD, and (later milestones) placement, spells,
 * raids and AI heroes.
 */
#pragma once

#include <string>
#include <cstdint>

#ifdef USE_SDL3
#include <SDL3/SDL_keycode.h>
#else
#include <SDL.h>
#endif

#include "engine/displacement.hpp"
#include "engine/point.hpp"
#include "engine/surface.hpp"

namespace devilution {

struct Player;

namespace keeper {

/** @brief The single dungeon level the keep is played on. */
constexpr uint8_t KeepLevel = 1;
/** @brief Fixed seed so the keep's layout is identical every game (and on every co-op client). */
constexpr uint32_t KeepLevelSeed = 0x6B656570; // "keep"

/** @brief True once the keep level is loaded and the master is in control. */
bool IsActive();

/** @brief Called at the end of LoadGameLevel. */
void OnLevelLoaded();

/** @brief Called when the game session ends (back to menu). */
void OnGameEnded();

/** @brief Called once per game logic tick (after monsters/players are processed). */
void Tick();

/** @brief Called once per rendered frame, before drawing the view. Moves the free camera. */
void UpdateCamera();

/**
 * @brief Shifts the first drawn tile/offset for the free camera (sub-tile scroll + a one-tile
 * margin to the north-west so scrolling never exposes empty borders).
 */
void ApplyViewShift(Point &position, Displacement &offset);

/** @brief Grows the drawn tile range to cover the margin added by ApplyViewShift. */
void GrowViewRange(int &rows, int &columns);

/** @brief Only the sub-tile camera scroll; used to keep mouse picking in step with drawing. */
Displacement CameraScrollOffset();

/** @brief Players are never drawn in the keep: the master has no body. */
bool ShouldDrawPlayer(const Player &player);

/** @brief Monsters never target players in the keep. */
bool CanMonstersTargetPlayers();

/** @return true if the key was consumed by the keeper controls. */
bool OnKeyDown(SDL_Keycode key);

/** @return true if the click was consumed by the keeper controls. */
bool OnLeftMouseDown(uint16_t modState);

/** Ends a drag-select. */
void OnLeftMouseUp();

/** @return true if the click was consumed by the keeper controls. */
bool OnRightMouseDown();

/** Mouse wheel zooms the keep view in/out. @return true if consumed. */
bool OnMouseWheel(int deltaY);

/** Mouse wheel zooms the keep view in/out. @return true if consumed. */
bool OnMouseWheel(int deltaY);

/** @brief Draws the master HUD over the main panel area. */
void DrawHud(const Surface &out);
/** Markers painted on the floor, between the floor and what stands on it. `out` is the unzoomed world view. */
void DrawFloorOverlays(const Surface &out);
/** scrollrt, per tile: the translucent ghost of the lair monster about to be placed under the cursor. */
void DrawPlacementGhost(const Surface &out, Point tile, Point targetBufferPosition);

/** Camera and HUD metrics, for the map editor's own UI. */
void CenterCamera(Point tile);
void SetViewZoom(bool zoomed);
bool IsViewZoomed();
int UiPixelScale();
Point UiMousePosition();
int UiBarHeight();

/** @brief Dev/test (--keeper-devctl): points the camera at a tile. */
void DevCenterCamera(Point tile);
/** @brief Dev/test (--keeper-devctl): keep ticks since the level loaded. */
uint32_t DevKeeperTicks();

/** @brief Dev/test flag (--keeper-autostart): skip menus and start a single-player keep. */
extern bool AutoStart;

/** @brief Dev/test flag (--keeper-lord-hp <n>): the Keep Lord's hit points (0 = normal). */
extern int TestLordHitPoints;
/** @brief Dev/test flag (--keeper-start-wave <n>): the first raid (0 = normal). */
extern int TestStartWave;
/** @brief Dev/test flag (--keeper-level <n>): the campaign level to autostart or host (0 = level 1). */
extern int TestCampaignLevel;
/** From --keeper-map: autostart or host this map file. */
extern std::string TestCustomMap;
/** @brief Dev/test flag (--keeper-rank <n>): overrides CampaignRank() for the roster (0 = normal). Pass it on every client. */
extern int TestRank;

/** @brief Dev/test flags for co-op (--keeper-host / --keeper-join <ip>): skip menus, TCP game. */
enum class AutoNetMode : uint8_t {
	None,
	Host,
	Join,
};
extern AutoNetMode AutoNet;
extern char AutoJoinAddress[128];

} // namespace keeper
} // namespace devilution
