/**
 * @file keeper/keeper_input.hpp
 *
 * The master's controls that aren't tied to the HUD layout: key bindings (through the engine's
 * Keymapper, so they appear in Settings > Keymapping and persist in the ini), the set of selected
 * structures, the drag-select box and how a group order is spread over the floor.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine/point.hpp"
#include "engine/rectangle.hpp"
#include "keeper/keeper_game.hpp"

namespace devilution::keeper {

// ---- key bindings -------------------------------------------------------------------

enum class KeeperAction : uint8_t {
	/** Index: which visible card of the current tab (0-based). */
	CardSlot,
	/** Index: which spell. */
	Spell,
	LockDoor,
	Bait,
	NextTab,
	StartRaid,
	CenterEntrance,
	CenterVault,
	ZoomIn,
	ZoomOut,
	Possess,
	Sell,
	CameraLeft,
	CameraRight,
	CameraUp,
	CameraDown,
	RoutePreview,
	ScoutCard,
};

/** Hotkeys bind "card slot N of the current tab", never a kind: the roster can grow freely. */
constexpr size_t NumCardSlotKeys = 9;

/** Registers the keeper's actions. Called from InitKeymapActions, so they head the settings list. */
void AddKeymapActions();

/**
 * Loads the keeper's bindings again after LoadOptions. A key has one action, and the last one to
 * claim it wins: this makes that the keeper, over the hero actions that share its default keys.
 */
void ReapplyKeyBindings();

/** Runs the keeper action bound to `key` (an SDL keycode or mouse button id). @return true if there was one. */
bool DispatchBoundKey(uint32_t key);

/** Short name of the key bound to an action, for HUD labels. Empty when unbound. */
std::string KeyLabel(KeeperAction action, size_t index = 0);

/** True while the keyboard key bound to an action is down (for the camera, which is polled). */
bool IsKeyHeld(KeeperAction action);

/** Does what a bound key asks. Defined in keeper.cpp, which owns the tools and the camera. */
void PerformKeeperAction(KeeperAction action, size_t index);

// ---- selection ----------------------------------------------------------------------

/** What is selected, with anything sold or destroyed since dropped. */
const std::vector<StructureRef> &Selection();
void SetSelection(std::vector<StructureRef> selection);
void ClearSelection();
/** The lair slots in the selection. */
std::vector<size_t> SelectedLairs();
/** What selling the whole selection would pay. */
int SelectionRefund();

/**
 * Up to `count` different floor tiles around `center`, nearest first, reached by walking from it so
 * a group never ends up behind a wall. There can be fewer than `count`; `center` is always first.
 */
std::vector<Point> SpreadTiles(Point center, size_t count);

// ---- drag-select box (screen pixels, not UI pixels) ---------------------------------

void BeginBoxDrag(Point mouse, float cameraX, float cameraY);
void CancelBoxDrag();
bool IsBoxDragging();
/**
 * The box from where the drag began to the mouse. The start stays on the same spot of the keep if
 * the camera scrolls. nullopt until the mouse has moved far enough for this not to be a click.
 */
std::optional<Rectangle> BoxDragRect(Point mouse, float cameraX, float cameraY, int zoom);

} // namespace devilution::keeper
