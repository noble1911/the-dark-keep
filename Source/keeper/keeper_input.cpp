/**
 * @file keeper/keeper_input.cpp
 *
 * See keeper_input.hpp.
 */
#include "keeper/keeper_input.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "control/control.hpp"
#include "engine/displacement.hpp"
#include "gmenu.h"
#include "keeper/keeper.hpp"
#include "keeper/keeper_editor.hpp"
#include "levels/dun_tile.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "options.h"
#include "utils/language.h"
#include "utils/sdl_compat.h"

namespace devilution::keeper {

namespace {

/** Every keeper action's ini key starts with this. */
constexpr std::string_view ActionPrefix = "Keeper";
/** Every keeper action's display name starts with this, in the settings list. */
constexpr std::string_view KeepActionPrefix = "Keep:";
/** How far the mouse has to travel, in screen pixels, before a click becomes a drag. */
constexpr int BoxDragThreshold = 8;
/** How far from the point a group order looks for floor to spread over. */
constexpr int GroupSpreadRadius = 12;

struct ActionInfo {
	KeeperAction action;
	std::string_view key;
	const char *name;
	const char *description;
	uint32_t defaultKey;
};

/** One-key actions. Card slots and spells are added in loops, as they have an index. */
const std::array<ActionInfo, 21> SimpleActions { {
	{ KeeperAction::LockDoor, "KeeperLockDoor", N_("Keep: lock a door"), N_("Picks the door lock."), 'G' },
	{ KeeperAction::Bait, "KeeperBait", N_("Keep: bait"), N_("Picks the pile of fool's gold."), 'T' },
	{ KeeperAction::NextTab, "KeeperNextTab", N_("Keep: next tab"), N_("Switches between the tabs of the card bar."), static_cast<uint32_t>(SDLK_TAB) },
	{ KeeperAction::StartRaid, "KeeperStartRaid", N_("Keep: open the gates"), N_("Starts the next raid early. In co-op: says you are ready. The host pressing again starts it for everyone."), 'R' },
	{ KeeperAction::CenterEntrance, "KeeperCenterEntrance", N_("Keep: go to the entrance"), N_("Centres the view on the entrance."), static_cast<uint32_t>(SDLK_SPACE) },
	{ KeeperAction::CenterVault, "KeeperCenterVault", N_("Keep: go to the vault"), N_("Centres the view on the vault."), 'V' },
	{ KeeperAction::ZoomIn, "KeeperZoomIn", N_("Keep: zoom in"), N_("Zooms the view in."), static_cast<uint32_t>(SDLK_EQUALS) },
	{ KeeperAction::ZoomOut, "KeeperZoomOut", N_("Keep: zoom out"), N_("Zooms the view out."), static_cast<uint32_t>(SDLK_MINUS) },
	{ KeeperAction::Possess, "KeeperPossess", N_("Keep: possess"), N_("Possesses the monster under the cursor, or releases it."), 'E' },
	{ KeeperAction::Sell, "KeeperSell", N_("Keep: sell"), N_("Sells what is selected."), static_cast<uint32_t>(SDLK_DELETE) },
	{ KeeperAction::CameraLeft, "KeeperCameraLeft", N_("Keep: camera left"), N_("Moves the view left."), 'A' },
	{ KeeperAction::CameraRight, "KeeperCameraRight", N_("Keep: camera right"), N_("Moves the view right."), 'D' },
	{ KeeperAction::CameraUp, "KeeperCameraUp", N_("Keep: camera up"), N_("Moves the view up."), 'W' },
	{ KeeperAction::CameraDown, "KeeperCameraDown", N_("Keep: camera down"), N_("Moves the view down."), 'S' },
	{ KeeperAction::CycleSpeed, "KeeperCycleSpeed", N_("Keep: game speed"), N_("Switches between normal, double and triple speed."), 'N' },
	{ KeeperAction::Ping, "KeeperPing", N_("Keep: mark the map"), N_("Shows every master the spot under the cursor, with a sound."), 'M' },
	{ KeeperAction::HoldTimer, "KeeperHoldTimer", N_("Keep: hold the timer"), N_("Single player: stops the clock on preparation, so the raid waits for you."), 'J' },
	{ KeeperAction::Help, "KeeperHelp", N_("Keep: help"), N_("Shows the controls and how to play."), 'H' },
	{ KeeperAction::RoutePreview, "KeeperRoutePreview", N_("Keep: route preview"), N_("Shows or hides the heroes' expected route while you prepare."), 'U' },
	{ KeeperAction::ScoutCard, "KeeperScoutCard", N_("Keep: scout card"), N_("Shows or hides the card about the next raid."), 'O' },
	{ KeeperAction::TurnWalls, "KeeperTurnWalls", N_("Keep: turn walls"), N_("Turns Fire Wall and Shock Wall to run the other way across the keep."), 'K' },
} };

/** Spell bar hotkeys, by slot. */
constexpr std::array<uint32_t, NumSpellSlots> SpellDefaults { 'Z', 'X', 'C', 'F', 'Q', 'B' };

/** Keys that aren't the keeper's own but still do something in the keep. */
const std::array<const char *, 6> SharedKeyActions { N_("Pause Game"), N_("Decrease Brightness"), N_("Increase Brightness"), N_("Screenshot"), N_("Game info"), N_("Chat Log") };

bool CanUseKeeperKeys()
{
	return IsActive() && !IsEditorActive() && !ChatFlag && !gmenu_is_active();
}

std::string ActionKey(KeeperAction action, size_t index)
{
	if (action == KeeperAction::CardSlot)
		return fmt::format("KeeperCard{}", index + 1);
	if (action == KeeperAction::Spell)
		return fmt::format("KeeperSpell{}", index + 1);
	for (const ActionInfo &info : SimpleActions) {
		if (info.action == action)
			return std::string(info.key);
	}
	return {};
}

/** The engine's key names are long ("KEYPADNUM 1"), and the labels sit on small cards. */
std::string ShortKeyName(std::string_view name)
{
	static constexpr std::array<std::pair<std::string_view, std::string_view>, 13> Short { {
	    { "SCROLLUPMOUSE", "WhUp" },
	    { "SCROLLDOWNMOUSE", "WhDn" },
	    { "SCROLLLEFTMOUSE", "WhL" },
	    { "SCROLLRIGHTMOUSE", "WhR" },
	    { "MMOUSE", "MMB" },
	    { "X1MOUSE", "M4" },
	    { "X2MOUSE", "M5" },
	    { "LCONTROL", "LCtl" },
	    { "RCONTROL", "RCtl" },
	    { "BACKSPACE", "BkSp" },
	    { "DELETE", "Del" },
	    { "CAPSLOCK", "Caps" },
	    { "SCROLLLOCK", "ScrL" },
	} };
	for (const auto &[from, to] : Short) {
		if (name == from)
			return std::string(to);
	}
	constexpr std::string_view KeypadNumber = "KEYPADNUM ";
	if (name.substr(0, KeypadNumber.size()) == KeypadNumber)
		return fmt::format("Num{}", name.substr(KeypadNumber.size()));
	constexpr std::string_view Keypad = "KEYPAD ";
	if (name.substr(0, Keypad.size()) == Keypad)
		return fmt::format("Pad{}", name.substr(Keypad.size(), 1));
	return std::string(name);
}

/** The Keymapper stores letters in upper case. */
uint32_t NormalizeKey(uint32_t key)
{
	return key >= 'a' && key <= 'z' ? key - ('a' - 'A') : key;
}

std::vector<StructureRef> selection;

struct BoxDrag {
	bool active = false;
	Point start;
	float cameraX = 0.F;
	float cameraY = 0.F;
};
BoxDrag boxDrag;

} // namespace

bool IsSettingShown(const OptionCategoryBase &category, const OptionEntryBase &entry)
{
	Options &options = GetOptions();
	if (&category == &options.Mods || &category == &options.GameMode || &category == &options.StartUp)
		return false;
	// All that is left of Gameplay once the hero's pickups, quests, belts and equipment are gone.
	if (&category == &options.Gameplay)
		return &entry == &options.Gameplay.grabInput || &entry == &options.Gameplay.pauseOnFocusLoss;
	if (&category == &options.Keymapper || &category == &options.Padmapper) {
		const std::string_view name = entry.GetName();
		if (name.substr(0, KeepActionPrefix.size()) == KeepActionPrefix)
			return true;
		return std::any_of(SharedKeyActions.begin(), SharedKeyActions.end(), [&](const char *action) { return name == _(action); });
	}
	return true;
}

void AddKeymapActions()
{
	Options &options = GetOptions();
	for (size_t i = 0; i < NumCardSlotKeys; i++) {
		options.Keymapper.AddAction(
		    "KeeperCard{}",
		    N_("Keep: card slot {}"),
		    N_("Picks this card of the tab being shown."),
		    static_cast<uint32_t>("1234567890[]"[i]),
		    [i] { PerformKeeperAction(KeeperAction::CardSlot, i); },
		    nullptr,
		    CanUseKeeperKeys,
		    static_cast<unsigned>(i + 1));
	}
	for (size_t i = 0; i < NumSpellSlots; i++) {
		options.Keymapper.AddAction(
		    "KeeperSpell{}",
		    N_("Keep: spell slot {}"),
		    N_("Casts the spell in this slot of the spell bar at the cursor."),
		    SpellDefaults[i],
		    [i] { PerformKeeperAction(KeeperAction::Spell, i); },
		    nullptr,
		    CanUseKeeperKeys,
		    static_cast<unsigned>(i + 1));
	}
	for (const ActionInfo &info : SimpleActions) {
		const KeeperAction action = info.action;
		options.Keymapper.AddAction(
		    info.key,
		    info.name,
		    info.description,
		    info.defaultKey,
		    [action] { PerformKeeperAction(action, 0); },
		    nullptr,
		    CanUseKeeperKeys);
	}
}

void ReapplyKeyBindings()
{
	KeymapperOptions &keymapper = GetOptions().Keymapper;
	for (OptionEntryBase *entry : keymapper.GetEntries()) {
		if (entry->key.substr(0, ActionPrefix.size()) == ActionPrefix)
			entry->LoadFromIni(keymapper.GetKey());
	}
}

bool DispatchBoundKey(uint32_t key)
{
	const KeymapperOptions::Action *action = GetOptions().Keymapper.findAction(NormalizeKey(key));
	if (action == nullptr || action->key.substr(0, ActionPrefix.size()) != ActionPrefix || !action->isEnabled() || !action->actionPressed)
		return false;
	action->actionPressed();
	return true;
}

bool IsKeyBoundTo(KeeperAction action, uint32_t key)
{
	return GetOptions().Keymapper.KeyForAction(ActionKey(action, 0)) == NormalizeKey(key);
}

std::string KeyLabel(KeeperAction action, size_t index)
{
	return ShortKeyName(GetOptions().Keymapper.KeyNameForAction(ActionKey(action, index)));
}

bool IsKeyHeld(KeeperAction action)
{
	uint32_t key = GetOptions().Keymapper.KeyForAction(ActionKey(action, 0));
	if (key == SDLK_UNKNOWN || (key & KeymapperMouseButtonMask) != 0)
		return false;
	if (key >= 'A' && key <= 'Z')
		key += 'a' - 'A';
	const auto keycode = static_cast<SDL_Keycode>(key);
#ifdef USE_SDL3
	return SDL_GetKeyboardState(nullptr)[SDL_GetScancodeFromKey(keycode, nullptr)];
#elif !defined(USE_SDL1)
	return SDL_GetKeyboardState(nullptr)[SDL_GetScancodeFromKey(keycode)];
#else
	return SDL_GetKeyState(nullptr)[keycode];
#endif
}

const std::vector<StructureRef> &Selection()
{
	selection.erase(std::remove_if(selection.begin(), selection.end(), [](const StructureRef &ref) { return !IsStructureActive(ref); }), selection.end());
	return selection;
}

void SetSelection(std::vector<StructureRef> newSelection)
{
	selection = std::move(newSelection);
}

void ClearSelection()
{
	selection.clear();
}

std::vector<size_t> SelectedLairs()
{
	std::vector<size_t> lairs;
	for (const StructureRef &ref : Selection()) {
		if (ref.type == StructureType::Lair)
			lairs.push_back(ref.slot);
	}
	return lairs;
}

int SelectionRefund()
{
	int total = 0;
	for (const StructureRef &ref : Selection())
		total += RefundFor(ref);
	return total;
}

std::vector<Point> SpreadTiles(Point center, size_t count)
{
	std::vector<Point> tiles { center };
	std::deque<Point> frontier { center };
	while (tiles.size() < count && !frontier.empty()) {
		const Point from = frontier.front();
		frontier.pop_front();
		for (const Direction dir : { Direction::SouthEast, Direction::SouthWest, Direction::NorthEast, Direction::NorthWest }) {
			const Point next = from + dir;
			if (!InDungeonBounds(next) || next.WalkingDistance(center) > GroupSpreadRadius || !IsTileWalkable(next, /*ignoreDoors=*/true))
				continue;
			if (std::find(tiles.begin(), tiles.end(), next) != tiles.end())
				continue;
			tiles.push_back(next);
			frontier.push_back(next);
			if (tiles.size() == count)
				break;
		}
	}
	return tiles;
}

void BeginBoxDrag(Point mouse, float cameraX, float cameraY)
{
	boxDrag = { true, mouse, cameraX, cameraY };
}

void CancelBoxDrag()
{
	boxDrag.active = false;
}

bool IsBoxDragging()
{
	return boxDrag.active;
}

std::optional<Rectangle> BoxDragRect(Point mouse, float cameraX, float cameraY, int zoom)
{
	if (!boxDrag.active)
		return std::nullopt;
	// Moving the camera one tile along world x or y moves the keep this much the other way on screen.
	const float dx = cameraX - boxDrag.cameraX;
	const float dy = cameraY - boxDrag.cameraY;
	const Point start {
		boxDrag.start.x + static_cast<int>(std::lround((dy - dx) * (TILE_WIDTH / 2) * zoom)),
		boxDrag.start.y - static_cast<int>(std::lround((dx + dy) * (TILE_HEIGHT / 2) * zoom)),
	};
	if (std::abs(mouse.x - start.x) < BoxDragThreshold && std::abs(mouse.y - start.y) < BoxDragThreshold)
		return std::nullopt;
	const Point topLeft { std::min(start.x, mouse.x), std::min(start.y, mouse.y) };
	return Rectangle { topLeft, { std::abs(mouse.x - start.x) + 1, std::abs(mouse.y - start.y) + 1 } };
}

} // namespace devilution::keeper
