/**
 * @file keeper/keeper_editor.cpp
 *
 * See keeper_editor.hpp. The editor's only model is a KeepMap; every change reruns the tileset's
 * joining rules over the whole layout and rebuilds the level, so what you see is what plays.
 */
#include "keeper/keeper_editor.hpp"
#include "plrmsg.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>
#endif

#include <fmt/format.h>

#include "control/control.hpp"
#include "controls/control_mode.hpp"
#include "controls/controller_buttons.h"
#include "controls/game_controls.h"
#include "cursor.h"
#include "diablo.h"
#include "engine/palette.h"
#include "engine/render/primitive_render.hpp"
#include "engine/render/scrollrt.h"
#include "engine/render/text_render.hpp"
#include "game_mode.hpp"
#include "items.h"
#include "keeper/keeper.hpp"
#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_dress.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_lighting.hpp"
#include "keeper/keeper_map.hpp"
#include "keeper/keeper_pad.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "objects.h"
#include "options.h"
#include "utils/file_util.h"
#include "utils/is_of.hpp"
#include "utils/sdl_compat.h"
#include "utils/ui_fwd.h"

#ifdef __SWITCH__
#include "platform/switch/keyboard.h"
#endif

namespace devilution::keeper {

namespace {

enum class EditTool : uint8_t {
	Floor,
	Rock,
	Wall,
	Room,
	Corridor,
	Door,
	Arch,
	Entrance,
	Vault,
	Object,
	Erase,
	Stamp,
};
constexpr size_t NumEditTools = 12;

struct ToolInfo {
	std::string_view name;
	std::string_view key;
	std::string_view help;
};

constexpr std::array<ToolInfo, NumEditTools> Tools { {
	{ "Floor", "1", "Drag to paint floor; walls join up by themselves. Right-drag: rock. [ ]: brush size." },
	{ "Rock", "2", "Drag to fill with solid rock. Right-drag: floor. [ ]: brush size." },
	{ "Wall", "3", "Drag a straight wall across a room; it stays one tile thick. Right-drag: remove." },
	{ "Room", "4", "Drag a rectangle of floor. Shift: walled off from what's around it. Right-drag: fill with rock." },
	{ "Hall", "5", "Drag from one end to the other. Shift: turn the other way. [ ]: width." },
	{ "Door", "6", "Click a straight wall with floor on both sides. Right-click: back to wall." },
	{ "Arch", "7", "An open archway: click a straight wall with floor on both sides. Right-click: back to wall." },
	{ "Entrance", "8", "Click to place the heroes' stairs. Stairs built into a wall snap to the nearest one. Right-click: remove." },
	{ "Vault", "9", "Click to place the vault. Stairs built into a wall snap to the nearest one. Right-click: remove." },
	{ "Object", "0", "Click floor to place one; click this tool again to pick another. Click one to change its look. Right-click: remove." },
	{ "Erase", "E", "Drag to remove doors, arches, objects and stairs. Paints nothing." },
	{ "Stamp", "T", "Click in a room for a themed group of props (Drag: a rectangle). Click this tool again to pick another. [ ]: next stamp." },
} };

enum class ActionKind : uint8_t {
	Tool,
	Undo,
	Redo,
	New,
	Save,
	SaveAs,
	Load,
	Tileset,
	Author,
	Zoom,
	Grid,
	Decor,
	Dress,
	Test,
};

struct Button {
	Rectangle rect;
	std::string label;
	ActionKind action;
	size_t tool = 0;
	bool selected = false;
	bool enabled = true;
};

enum class Modal : uint8_t {
	None,
	SaveName,
	Author,
	LoadList,
	TilesetList,
	PropList,
	StampList,
	ConfirmNew,
};

struct Snapshot {
	MapTileset tileset;
	std::array<std::array<uint8_t, DMAXY>, DMAXX> cells;
	Point entrance;
	Point vault;
	std::vector<MapObject> objects;

	bool operator==(const Snapshot &other) const
	{
		return tileset == other.tileset && cells == other.cells && entrance == other.entrance && vault == other.vault && objects == other.objects;
	}
};

constexpr size_t MaxUndo = 100;
constexpr int MaxBrush = 6;
constexpr int RightColumnWidth = 210;
constexpr int ButtonHeight = 18;
constexpr int ModalWidth = 360;
constexpr int ModalRows = 12;
constexpr int ModalRowHeight = 16;
constexpr std::string_view TestMapFile = "_test.dkmap";
constexpr std::string_view AutosaveFile = "_autosave.dkmap";

bool sessionStarted = false;
KeepMap model;
std::string fileName;
bool dirty = false;
bool levelStale = false;
bool decorPreview = false;
bool showGrid = true;
EditTool tool = EditTool::Floor;
int brushSize = 1;
MapObjectKind objectKind = MapObjectKind::Barrel;
std::vector<MapProblem> problems;
/** A row of the prop list: a group to open, a kind to pick, or the way back to the groups. */
struct PropRow {
	enum class Type : uint8_t {
		Group,
		Kind,
		Back,
	};
	Type type;
	MapObjectGroup group;
	MapObjectKind kind;
};
/** The prop list's rows, as it was opened, and the group it shows (none: the list of groups). */
std::vector<PropRow> propListRows;
std::optional<MapObjectGroup> propListGroup;
std::vector<Snapshot> undoStack;
std::vector<Snapshot> redoStack;

struct Stroke {
	bool active = false;
	bool right = false;
	bool shift = false;
	Point anchor;
	Point last;
	Snapshot before;
};
Stroke stroke;

/** The stamp the Stamp tool places: an index into RoomStamps(). */
size_t stampIndex = 0;
/** The stamps in the stamp list, as it was opened. */
std::vector<const RoomStamp *> stampListStamps;

/**
 * A stamp on show but not kept yet: its props are in the model, and `before` is the model without them. Kept,
 * it is one Undo; dropped, the model goes back to `before`.
 */
struct StampPreview {
	bool active = false;
	Snapshot before;
	bool dirtyBefore = false;
	std::vector<Point> cells;
	uint32_t seed = 0;
	StampResult result;
};
StampPreview stampPreview;
/** Each stamp shown gets the next seed, so a reroll (or the next room) is a new layout. */
uint32_t stampSeed = 0;

void SettleStamp();

Modal modal = Modal::None;
std::string modalText;
std::string modalNote;
std::vector<std::string> modalList;
int modalScroll = 0;
/** The list row the keyboard is on. */
int modalRow = 0;

std::string feedback;
bool feedbackIsError = false;
uint32_t feedbackUntil = 0;

/** A test run is in progress; these bring back the editor. */
bool testing = false;
/** ReloadKeepLevel has been asked for and hasn't happened: the level on screen is on its way out. */
bool levelReloading = false;
KeepSetup setupBeforeTest;
std::optional<Point> cameraToRestore;

void SetFeedback(std::string text, bool error = false)
{
	feedback = std::move(text);
	feedbackIsError = error;
	feedbackUntil = SDL_GetTicks() + 4000;
}

Snapshot TakeSnapshot()
{
	return { model.tileset, model.cells, model.entrance, model.vault, model.objects };
}

void ReloadLevel()
{
	levelReloading = true;
	stroke.active = false;
	ReloadKeepLevel();
}

void RestoreSnapshot(const Snapshot &snapshot)
{
	// The stairs in a snapshot only fit the tileset it was taken in, so the two go back together.
	const bool sameTileset = snapshot.tileset == model.tileset;
	model.tileset = snapshot.tileset;
	model.cells = snapshot.cells;
	model.entrance = snapshot.entrance;
	model.vault = snapshot.vault;
	model.objects = snapshot.objects;
	dirty = true;
	levelStale = true;
	if (!sameTileset) {
		cameraToRestore = ViewPosition;
		ReloadLevel();
	}
}

void PushUndo(const Snapshot &snapshot)
{
	undoStack.push_back(snapshot);
	if (undoStack.size() > MaxUndo)
		undoStack.erase(undoStack.begin());
	redoStack.clear();
}

/** Rebuilds the real level from the model. Fast enough to do on every change. */
void RefreshLevel()
{
	levelStale = false;
	LightingBeforeLevelRebuild();
	ApplyLayoutToLevel(model, decorPreview);
	std::memset(dObject, 0, sizeof(dObject));
	InitObjects();
	// The stairs' own spots, roughly: the triggers that would say exactly aren't rebuilt here.
	const Point entrance = model.entrance != NoMapPosition ? MapSpawnTile(model) : NoMapPosition;
	const Point vault = model.vault != NoMapPosition ? model.vault.megaToWorld() + Displacement { 1, 1 } : NoMapPosition;
	LightingAfterLevelRebuild(entrance, vault);
	problems = ValidateMap(model);
}

bool HasBlockingProblem()
{
	return std::any_of(problems.begin(), problems.end(), [](const MapProblem &problem) { return problem.blocking; });
}

void InitNewMap()
{
	const MapTileset tileset = model.tileset;
	const std::string author = model.author;
	model = {};
	model.tileset = tileset;
	model.author = author;
	model.decorSeed = SDL_GetTicks() | 1U;
	// A first room, so there's something to look at and to start from.
	for (int x = DMAXX / 2 - 6; x < DMAXX / 2 + 6; x++) {
		for (int y = DMAXY / 2 - 6; y < DMAXY / 2 + 6; y++)
			model.setCell(x, y, MapCell::Floor);
	}
	fileName.clear();
	dirty = false;
	undoStack.clear();
	redoStack.clear();
	stampPreview.active = false;
}

std::string WriteModel(const std::string &path)
{
	BakePlayableMap(model);
	RefreshLevel(); // back to what was on screen
	const tl::expected<void, std::string> written = WriteMapBytes(path, SerializeMap(model));
	return written.has_value() ? std::string() : written.error();
}

void Autosave()
{
	SettleStamp();
	if (!dirty)
		return;
	const std::string path = MapsFolder() + std::string(AutosaveFile);
	std::string error;
	if (IsMapTilesetLoaded(model.tileset)) {
		error = WriteModel(path);
	} else {
		// The level on screen is another tileset's, so it can't be baked: keep the layout, unfinished.
		model.playable = false;
		const tl::expected<void, std::string> written = WriteMapBytes(path, SerializeMap(model));
		if (!written.has_value())
			error = written.error();
	}
	if (!error.empty())
		SetFeedback(fmt::format("Autosave failed: {}", error), true);
}

void SaveToCurrentFile()
{
	const std::string error = WriteModel(MapsFolder() + fileName);
	if (!error.empty()) {
		SetFeedback(fmt::format("Could not save: {}", error), true);
		return;
	}
	dirty = false;
	if (model.playable)
		SetFeedback(fmt::format("Saved {}", fileName));
	else
		SetFeedback(fmt::format("Saved {}, but it can't be played yet", fileName), true);
}

void OpenTextModal(Modal kind, std::string text)
{
	modal = kind;
	modalText = std::move(text);
	modalNote.clear();
}

void Save()
{
	SettleStamp();
	if (fileName.empty())
		OpenTextModal(Modal::SaveName, model.name);
	else
		SaveToCurrentFile();
}

void ChangeTileset(MapTileset tileset)
{
	if (tileset == model.tileset)
		return;
	SettleStamp();
	if (!IsMapTilesetInstalled(tileset)) {
		SetFeedback(fmt::format("The game data here has no {} tiles", MapTilesetName(tileset)), true);
		return;
	}
	PushUndo(TakeSnapshot());
	model.tileset = tileset;
	if (!MapObjectFitsTileset(objectKind, tileset))
		objectKind = MapObjectKind::Barrel;
	if (model.entrance.x >= 0 && !MapStairsFit(tileset, true, model.entrance))
		model.entrance = NoMapPosition;
	if (model.vault.x >= 0 && !MapStairsFit(tileset, false, model.vault))
		model.vault = NoMapPosition;
	dirty = true;
	cameraToRestore = ViewPosition;
	ReloadLevel();
}

void LoadMapNamed(const std::string &name)
{
	const tl::expected<std::vector<std::byte>, std::string> bytes = ReadMapBytes(MapsFolder() + name);
	if (!bytes.has_value()) {
		SetFeedback(fmt::format("Could not load {}: {}", name, bytes.error()), true);
		return;
	}
	tl::expected<KeepMap, std::string> loaded = ParseMap(bytes->data(), bytes->size());
	if (!loaded.has_value()) {
		SetFeedback(fmt::format("Could not load {}: {}", name, loaded.error()), true);
		return;
	}
	if (!IsMapTilesetInstalled(loaded->tileset)) {
		SetFeedback(fmt::format("Could not load {}: the game data here has no {} tiles", name, MapTilesetName(loaded->tileset)), true);
		return;
	}
	Autosave();
	stampPreview.active = false;
	const bool sameTileset = loaded->tileset == model.tileset;
	model = std::move(*loaded);
	// The editor's own scratch files get overwritten behind the author's back: saving asks for a name.
	const bool scratch = name.empty() || name[0] == '_';
	fileName = scratch ? std::string() : name;
	dirty = scratch;
	undoStack.clear();
	redoStack.clear();
	SetFeedback(fmt::format("Loaded {}", name));
	// Maps from before the stairs had to go in a wall still play: say so, and leave moving them to the author.
	for (const bool entrance : { true, false }) {
		if (const std::optional<std::string> why = MapStairsWallProblem(model, entrance, entrance ? model.entrance : model.vault); why) {
			SetFeedback(fmt::format("Loaded {}. {}", name, *why), true);
			break;
		}
	}
	if (sameTileset) {
		levelStale = true;
		CenterCamera(MapSpawnTile(model));
	} else {
		cameraToRestore = MapSpawnTile(model);
		ReloadLevel();
	}
}

void NewMapNow()
{
	const bool hadChanges = dirty;
	Autosave();
	InitNewMap();
	levelStale = true;
	CenterCamera(MapSpawnTile(model));
	SetFeedback(hadChanges ? "New map (your unsaved changes are in _autosave)" : "New map");
}

void NewMap()
{
	SettleStamp();
	if (dirty) {
		modal = Modal::ConfirmNew;
		modalNote.clear();
	} else {
		NewMapNow();
	}
}

void StartTest()
{
	SettleStamp();
	if (levelStale)
		RefreshLevel();
	if (HasBlockingProblem()) {
		SetFeedback(fmt::format("Can't test yet: {}", problems.front().text), true);
		return;
	}
	const std::string path = MapsFolder() + std::string(TestMapFile);
	const std::string error = WriteModel(path);
	if (!error.empty()) {
		SetFeedback(fmt::format("Could not write the test map: {}", error), true);
		return;
	}
	if (!model.playable) {
		// Every roll of the decoration plugged something. The test would quietly play a random keep.
		SetFeedback("Can't test yet: the wall and floor variants keep blocking the way. Widen the narrow halls.", true);
		return;
	}
	setupBeforeTest = CurrentSetup();
	KeepSetup setup = setupBeforeTest;
	setup.mode = GameMode::CustomMap;
	setup.mapFile = path;
	SetCurrentSetup(setup);
	testing = true;
	cameraToRestore = ViewPosition;
	ReloadLevel();
}

// ---- geometry ----------------------------------------------------------------------------------

std::optional<Point> CursorCell()
{
	const Point tile = cursPosition;
	if (tile.x < 16 || tile.y < 16)
		return std::nullopt;
	const Point cell { (tile.x - 16) / 2, (tile.y - 16) / 2 };
	if (cell.x >= DMAXX || cell.y >= DMAXY)
		return std::nullopt;
	return cell;
}

int PanelTop()
{
	return gnScreenHeight / UiPixelScale() - UiBarHeight();
}

bool Contains(const Rectangle &rect, Point p)
{
	return p.x >= rect.position.x && p.y >= rect.position.y && p.x < rect.position.x + rect.size.width && p.y < rect.position.y + rect.size.height;
}

bool IsMouseOverPanel()
{
	return modal != Modal::None || UiMousePosition().y >= PanelTop();
}

bool IsShiftHeld()
{
	return (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
}

bool IsCtrlHeld()
{
	return (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
}

/** Top-left of a staircase centred on `cell`, pushed back inside the map. */
Point StairsPositionAt(Point cell, bool entrance)
{
	const Size size = MapStairsSize(model.tileset, entrance);
	return {
		std::clamp(cell.x - size.width / 2, 1, DMAXX - 1 - size.width),
		std::clamp(cell.y - size.height / 2, 1, DMAXY - 1 - size.height),
	};
}

/** How far a click may be from the wall stairs that need one snap to. */
constexpr int StairsSnapRadius = 5;

/**
 * Where a click with the Entrance or Vault tool puts the stairs: centred on the click, or for stairs built into a
 * wall, the nearest spot in a wall, as the generator would only put them. Nothing if there's no wall near.
 */
std::optional<Point> StairsSpotFor(Point cell, bool entrance)
{
	const Point wanted = StairsPositionAt(cell, entrance);
	if (!MapStairsNeedWall(model.tileset, entrance))
		return wanted;
	return FindMapStairsSpot(model, entrance, wanted, StairsSnapRadius);
}

void PlaceStairs(Point cell, bool entrance, bool remove)
{
	Point &stairs = entrance ? model.entrance : model.vault;
	if (remove) {
		stairs = NoMapPosition;
		levelStale = dirty = true;
		return;
	}
	const std::optional<Point> spot = StairsSpotFor(cell, entrance);
	if (!spot) {
		SetFeedback(MapStairsWallHint(model.tileset, entrance), true);
		return;
	}
	if (*spot != StairsPositionAt(cell, entrance))
		SetFeedback(entrance ? "The entrance went into the nearest wall" : "The vault went into the nearest wall");
	stairs = *spot;
	levelStale = dirty = true;
}

/** Cells a drag would change (Wall, Room, Corridor), as a list. */
std::vector<Point> DraggedCells()
{
	std::vector<Point> cells;
	const Point a = stroke.anchor;
	const Point b = stroke.last;
	const auto add = [&](int x, int y) {
		if (IsPaintableCell(x, y))
			cells.push_back({ x, y });
	};
	switch (tool) {
	case EditTool::Wall:
		if (std::abs(b.x - a.x) >= std::abs(b.y - a.y)) {
			for (int x = std::min(a.x, b.x); x <= std::max(a.x, b.x); x++)
				add(x, a.y);
		} else {
			for (int y = std::min(a.y, b.y); y <= std::max(a.y, b.y); y++)
				add(a.x, y);
		}
		break;
	case EditTool::Room:
		for (int x = std::min(a.x, b.x); x <= std::max(a.x, b.x); x++) {
			for (int y = std::min(a.y, b.y); y <= std::max(a.y, b.y); y++)
				add(x, y);
		}
		break;
	case EditTool::Corridor: {
		const Point elbow = stroke.shift ? Point { a.x, b.y } : Point { b.x, a.y };
		const auto addRun = [&](Point from, Point to) {
			const int width = std::max(brushSize, 1);
			for (int x = std::min(from.x, to.x); x <= std::max(from.x, to.x); x++) {
				for (int y = std::min(from.y, to.y); y <= std::max(from.y, to.y); y++) {
					for (int dx = 0; dx < width; dx++) {
						for (int dy = 0; dy < width; dy++)
							add(x + dx, y + dy);
					}
				}
			}
		};
		addRun(a, elbow);
		addRun(elbow, b);
		break;
	}
	default:
		break;
	}
	return cells;
}

// ---- editing --------------------------------------------------------------------------------------

void RemoveObjectsInCell(Point cell)
{
	const Point first = cell.megaToWorld();
	model.objects.erase(std::remove_if(model.objects.begin(), model.objects.end(), [&](const MapObject &object) {
		return object.position.x >= first.x && object.position.y >= first.y && object.position.x < first.x + 2 && object.position.y < first.y + 2;
	}),
	    model.objects.end());
}

void SetCell(int x, int y, MapCell value)
{
	if (!IsPaintableCell(x, y) || model.cell(x, y) == value)
		return;
	model.setCell(x, y, value);
	if (value != MapCell::Floor)
		RemoveObjectsInCell({ x, y });
	levelStale = true;
	dirty = true;
}

void PaintBrushAt(Point cell, MapCell value)
{
	for (int dx = 0; dx < brushSize; dx++) {
		for (int dy = 0; dy < brushSize; dy++)
			SetCell(cell.x + dx - (brushSize - 1) / 2, cell.y + dy - (brushSize - 1) / 2, value);
	}
}

bool IsInStairs(Point cell, Point stairs, bool entrance)
{
	if (stairs.x < 0)
		return false;
	const Size size = MapStairsSize(model.tileset, entrance);
	return cell.x >= stairs.x && cell.y >= stairs.y && cell.x < stairs.x + size.width && cell.y < stairs.y + size.height;
}

/** Back to wall, or in a cave, where a gate stands on open floor, back to floor. */
MapCell ClearedOpening()
{
	return model.tileset == MapTileset::Caves ? MapCell::Floor : MapCell::Rock;
}

void EraseAt(Point cell)
{
	if (IsInStairs(cell, model.entrance, true)) {
		model.entrance = NoMapPosition;
		levelStale = dirty = true;
	}
	if (IsInStairs(cell, model.vault, false)) {
		model.vault = NoMapPosition;
		levelStale = dirty = true;
	}
	if (IsPaintableCell(cell.x, cell.y) && IsAnyOf(model.cell(cell.x, cell.y), MapCell::Door, MapCell::Arch))
		SetCell(cell.x, cell.y, ClearedOpening());
	const size_t objects = model.objects.size();
	RemoveObjectsInCell(cell);
	if (model.objects.size() != objects)
		levelStale = dirty = true;
}

void PlaceOpening(Point cell, MapCell kind, bool remove)
{
	if (remove) {
		if (IsPaintableCell(cell.x, cell.y) && model.cell(cell.x, cell.y) == kind)
			SetCell(cell.x, cell.y, ClearedOpening());
		return;
	}
	if (kind == MapCell::Door && !MapTilesetHasDoors(model.tileset)) {
		SetFeedback(fmt::format("{} has no door graphics", MapTilesetName(model.tileset)), true);
		return;
	}
	if (kind == MapCell::Arch && !MapTilesetHasArches(model.tileset)) {
		SetFeedback(fmt::format("{} has no arch tiles", MapTilesetName(model.tileset)), true);
		return;
	}
	SetCell(cell.x, cell.y, kind);
}

/** The object a click on `tile` means: one standing there (on any of a big one's tiles), or a wall prop on the wall beside it. */
std::vector<MapObject>::iterator FindObjectForClick(Point tile)
{
	const auto found = std::find_if(model.objects.begin(), model.objects.end(), [&](const MapObject &object) {
		const std::vector<Displacement> footprint = MapObjectFootprint(object.kind);
		return std::any_of(footprint.begin(), footprint.end(), [&](Displacement offset) { return object.position + offset == tile; });
	});
	if (found != model.objects.end())
		return found;
	return std::find_if(model.objects.begin(), model.objects.end(), [&](const MapObject &object) {
		const MapObjectPlacement placement = GetMapObjectTraits(object.kind).placement;
		if (placement == MapObjectPlacement::Floor)
			return false;
		// SnapMapObject's reach: a wall prop up to two tiles back, a prop against the wall one.
		const Displacement facing = object.facesY() ? Displacement { 0, 1 } : Displacement { 1, 0 };
		return object.position == tile - facing || (placement == MapObjectPlacement::OnWall && object.position == tile - facing * 2);
	});
}

/** A look for a new prop: varied by where it goes, the same every time for the same spot. */
uint8_t DefaultStyle(MapObjectKind kind, Point tile)
{
	const uint8_t styles = GetMapObjectTraits(kind).styles;
	return static_cast<uint8_t>((tile.x * 7 + tile.y * 3) % styles);
}

/** Where a click would put the selected kind, and why it can't go there if it can't. */
std::pair<std::optional<MapObject>, std::string> ObjectForClick(Point tile)
{
	const std::optional<MapObject> snapped = SnapMapObject(objectKind, tile, DefaultStyle(objectKind, tile));
	if (!snapped) {
		const bool behind = GetMapObjectTraits(objectKind).placement == MapObjectPlacement::AgainstWall;
		return { std::nullopt, behind ? "It goes on the floor with its back to a straight wall" : "It goes on a straight wall, beside the floor" };
	}
	if (!InDungeonBounds(snapped->position) || (!IsTileNotSolid(snapped->position) && GetMapObjectTraits(objectKind).placement != MapObjectPlacement::OnWall))
		return { snapped, "Objects go on the floor" };
	if (std::optional<std::string> why = WhyMapObjectCantStand(model, *snapped); why)
		return { snapped, *why };
	return { snapped, {} };
}

void PlaceObject(Point tile, bool remove)
{
	if (levelStale)
		RefreshLevel(); // the rules look at the level, which has to be this model's
	const auto found = FindObjectForClick(tile);
	if (remove) {
		if (found != model.objects.end()) {
			model.objects.erase(found);
			levelStale = dirty = true;
		}
		return;
	}
	if (found != model.objects.end()) {
		// Click one again for its next look.
		const uint8_t styles = GetMapObjectTraits(found->kind).styles;
		if (found->kind == objectKind && styles > 1) {
			found->variant = static_cast<uint8_t>((found->variant & MapObjectFacesY) | ((found->style() + 1) % styles));
			levelStale = dirty = true;
		}
		return;
	}
	if (model.objects.size() >= MaxMapObjects) {
		SetFeedback(fmt::format("A map can hold {} objects", MaxMapObjects), true);
		return;
	}
	if (MapObjectFileCount(model, objectKind) > MaxLevelObjectFiles && MapObjectFileCount(model) <= MaxLevelObjectFiles) {
		SetFeedback("A level can't load the art of more kinds of prop: use kinds it already has", true);
		return;
	}
	if (!IsLegacyMapObject(objectKind) && ActiveObjectCount >= MaxLevelObjectsAtLoad) {
		SetFeedback(fmt::format("A level holds {} objects with its doors", MaxLevelObjectsAtLoad), true);
		return;
	}
	const auto [object, why] = ObjectForClick(tile);
	if (!why.empty()) {
		SetFeedback(why, true);
		return;
	}
	model.objects.push_back(*object);
	levelStale = dirty = true;
}

/**
 * The prop list: the groups (Light, Wall, Furniture, ...) with what each holds for the tileset, or one
 * group's kinds with a row back to the groups first. Mouse, keys and pad all work it the same way.
 */
void OpenPropList(std::optional<MapObjectGroup> group)
{
	modal = Modal::PropList;
	propListGroup = group;
	propListRows.clear();
	modalList.clear();
	modalRow = 0;
	const std::vector<MapObjectKind> palette = MapObjectPalette(model.tileset);
	const MapObjectGroup currentGroup = GetMapObjectTraits(objectKind).group;
	if (!group) {
		for (size_t i = 0; i < NumMapObjectGroups; i++) {
			const auto each = static_cast<MapObjectGroup>(i);
			const auto count = std::count_if(palette.begin(), palette.end(), [&](MapObjectKind kind) { return GetMapObjectTraits(kind).group == each; });
			if (count == 0)
				continue;
			if (each == currentGroup)
				modalRow = static_cast<int>(propListRows.size());
			propListRows.push_back({ PropRow::Type::Group, each, MapObjectKind::Barrel });
			modalList.push_back(fmt::format("{} ({})", MapObjectGroupName(each), count));
		}
	} else {
		propListRows.push_back({ PropRow::Type::Back, *group, MapObjectKind::Barrel });
		modalList.emplace_back("< All groups");
		for (const MapObjectKind kind : palette) {
			const MapObjectTraits traits = GetMapObjectTraits(kind);
			if (traits.group != *group)
				continue;
			std::string label(MapObjectName(kind));
			if (traits.light)
				label += ", light";
			if (traits.placement == MapObjectPlacement::OnWall)
				label += ", on a wall";
			if (traits.placement == MapObjectPlacement::AgainstWall)
				label += ", against a wall";
			if (traits.tiles > 1)
				label += fmt::format(", {} tiles", traits.tiles);
			if (traits.styles > 1)
				label += fmt::format(", {} looks", traits.styles);
			if (kind == objectKind)
				modalRow = static_cast<int>(propListRows.size());
			propListRows.push_back({ PropRow::Type::Kind, *group, kind });
			modalList.push_back(std::move(label));
		}
	}
	modalScroll = std::clamp(modalRow - ModalRows + 1, 0, std::max(0, static_cast<int>(modalList.size()) - ModalRows));
	modalNote.clear();
}

/** Opens on the chosen kind's group, so picking another of the same sort is one click. */
void OpenPropList()
{
	const MapObjectGroup group = GetMapObjectTraits(objectKind).group;
	const std::vector<MapObjectKind> palette = MapObjectPalette(model.tileset);
	const bool inPalette = std::find(palette.begin(), palette.end(), objectKind) != palette.end();
	OpenPropList(inPalette ? std::optional<MapObjectGroup>(group) : std::nullopt);
}

const RoomStamp &CurrentStamp()
{
	const std::vector<RoomStamp> &stamps = RoomStamps();
	if (stampIndex >= stamps.size() || !RoomStampSuits(stamps[stampIndex], model.tileset)) {
		const std::vector<const RoomStamp *> suits = RoomStampsFor(model.tileset);
		stampIndex = suits.empty() ? 0 : static_cast<size_t>(suits.front() - stamps.data());
	}
	return stamps[stampIndex];
}

/** The next or previous stamp meant for the tileset. */
void CycleStamp(int delta)
{
	const std::vector<const RoomStamp *> suits = RoomStampsFor(model.tileset);
	if (suits.empty())
		return;
	const RoomStamp *current = &CurrentStamp();
	const auto at = std::find(suits.begin(), suits.end(), current);
	const int index = at == suits.end() ? 0 : static_cast<int>(at - suits.begin());
	const int count = static_cast<int>(suits.size());
	stampIndex = static_cast<size_t>(suits[(index + delta + count) % count] - RoomStamps().data());
}

std::string StampKeysHint()
{
	if (IsPadInUse())
		return fmt::format("{} on it: keep   {}+{}: another layout   {}: drop", InputName(PadAction::Confirm), InputName(PadAction::Modifier), InputName(PadAction::Confirm), InputName(PadAction::Order));
	return "Click it or Enter: keep   R or Shift-click: another layout   Right-click or Esc: drop";
}

void DropStamp()
{
	if (!stampPreview.active)
		return;
	stampPreview.active = false;
	model.objects = stampPreview.before.objects;
	dirty = stampPreview.dirtyBefore;
	levelStale = true;
}

void KeepStamp()
{
	if (!stampPreview.active)
		return;
	stampPreview.active = false;
	PushUndo(stampPreview.before);
	dirty = true;
	SetFeedback(fmt::format("Kept the {}: {} props. Undo takes it out", CurrentStamp().label, stampPreview.result.placed));
}

void SettleStamp()
{
	KeepStamp();
}

/** Shows the current stamp on `cells`, in place of any stamp on show. */
void PreviewStamp(const std::vector<Point> &cells)
{
	DropStamp();
	if (model.entrance == NoMapPosition || model.vault == NoMapPosition) {
		SetFeedback("Place the entrance and the vault first: a stamp keeps off the heroes' way", true);
		return;
	}
	if (cells.empty()) {
		SetFeedback("That isn't a room: click inside one, or drag a rectangle", true);
		return;
	}
	const RoomStamp &stamp = CurrentStamp();
	if (model.objects.size() >= MaxMapObjects) {
		SetFeedback(fmt::format("A map can hold {} objects: there's no room for the {}", MaxMapObjects, stamp.label), true);
		return;
	}
	const Snapshot before = TakeSnapshot();
	const bool dirtyBefore = dirty;
	StampOptions options;
	options.seed = stampSeed++;
	const StampResult result = StampRoom(model, stamp, cells, options);
	levelStale = true;
	if (result.placed == 0) {
		model.objects = before.objects;
		if (!result.notes.empty())
			SetFeedback(result.notes.front(), true);
		else if (result.capped)
			SetFeedback(fmt::format("The object cap is reached ({} on a map, {} on a level with its doors)", MaxMapObjects, MaxLevelObjectsAtLoad), true);
		else
			SetFeedback(fmt::format("No {} fits there, off the heroes' way", stamp.label), true);
		return;
	}
	stampPreview = { true, before, dirtyBefore, cells, options.seed, result };
	dirty = true;
	if (result.capped)
		SetFeedback(fmt::format("The object cap stopped the {} at {} of {} props ({} on a map, {} on a level). {}", stamp.label, result.placed, result.wanted, MaxMapObjects, MaxLevelObjectsAtLoad, StampKeysHint()), true);
	else
		SetFeedback(fmt::format("{}: {} props, {} lights. {}", stamp.label, result.placed, result.lights, StampKeysHint()));
}

void RerollStamp()
{
	if (!stampPreview.active) {
		SetFeedback("Stamp a room first, then this tries another layout", true);
		return;
	}
	const std::vector<Point> cells = stampPreview.cells;
	PreviewStamp(cells);
}

/** The cells a click or drag of the Stamp tool means. */
std::vector<Point> StampCells()
{
	if (stroke.anchor == stroke.last)
		return RoomCellsAt(model, stroke.anchor);
	return FloorCellsIn(model, stroke.anchor, stroke.last);
}

void FinishStampStroke()
{
	const bool click = stroke.anchor == stroke.last;
	if (click && stampPreview.active && std::find(stampPreview.cells.begin(), stampPreview.cells.end(), stroke.anchor) != stampPreview.cells.end()) {
		// On the stamp on show: keep it, or with Shift try another.
		if (stroke.shift)
			RerollStamp();
		else
			KeepStamp();
		return;
	}
	PreviewStamp(StampCells());
}

void OpenStampList()
{
	modal = Modal::StampList;
	stampListStamps = RoomStampsFor(model.tileset);
	modalList.clear();
	modalRow = 0;
	const RoomStamp *current = &CurrentStamp();
	for (size_t i = 0; i < stampListStamps.size(); i++) {
		const RoomStamp &stamp = *stampListStamps[i];
		// What it has here: the first kind each entry names that this build and tileset have.
		std::vector<std::string> kinds;
		const std::vector<MapObjectKind> palette = MapObjectPalette(model.tileset);
		for (const RoomStampEntry &entry : stamp.entries) {
			for (std::string_view names = entry.kinds; !names.empty();) {
				const size_t bar = names.find('|');
				const std::optional<MapObjectKind> kind = MapObjectKindNamed(names.substr(0, bar));
				names = bar == std::string_view::npos ? std::string_view() : names.substr(bar + 1);
				if (kind && !IsLegacyMapObject(*kind) && std::find(palette.begin(), palette.end(), *kind) != palette.end()) {
					std::string name(MapObjectName(*kind));
					if (std::find(kinds.begin(), kinds.end(), name) == kinds.end())
						kinds.push_back(std::move(name));
					break;
				}
			}
		}
		std::string label(stamp.label);
		// The list is narrow: the first two say what it is.
		for (size_t k = 0; k < kinds.size() && k < 2; k++)
			label += fmt::format("{} {}", k == 0 ? ":" : ",", kinds[k]);
		modalList.push_back(std::move(label));
		if (stampListStamps[i] == current)
			modalRow = static_cast<int>(i);
	}
	modalScroll = std::clamp(modalRow - ModalRows + 1, 0, std::max(0, static_cast<int>(modalList.size()) - ModalRows));
	modalNote = modalList.empty() ? "No stamps for this tileset" : "";
}

/** A new stamp chosen: the one on show becomes it, in the same room. */
void ChoseStamp()
{
	if (!stampPreview.active)
		return;
	const std::vector<Point> cells = stampPreview.cells;
	PreviewStamp(cells);
}

void FinishStroke()
{
	const bool committed = stroke.active;
	stroke.active = false;
	if (!committed)
		return;
	if (tool == EditTool::Stamp) {
		if (!stroke.right)
			FinishStampStroke();
		return;
	}
	switch (tool) {
	case EditTool::Wall:
		for (const Point cell : DraggedCells())
			SetCell(cell.x, cell.y, stroke.right ? MapCell::Floor : MapCell::Rock);
		break;
	case EditTool::Room: {
		const std::vector<Point> cells = DraggedCells();
		for (const Point cell : cells)
			SetCell(cell.x, cell.y, stroke.right ? MapCell::Rock : MapCell::Floor);
		if (!stroke.right && stroke.shift && !cells.empty()) {
			// Walled off: one ring of rock, so it doesn't merge with its neighbours.
			const int left = std::min(stroke.anchor.x, stroke.last.x) - 1;
			const int right = std::max(stroke.anchor.x, stroke.last.x) + 1;
			const int top = std::min(stroke.anchor.y, stroke.last.y) - 1;
			const int bottom = std::max(stroke.anchor.y, stroke.last.y) + 1;
			for (int x = left; x <= right; x++) {
				SetCell(x, top, MapCell::Rock);
				SetCell(x, bottom, MapCell::Rock);
			}
			for (int y = top; y <= bottom; y++) {
				SetCell(left, y, MapCell::Rock);
				SetCell(right, y, MapCell::Rock);
			}
		}
		break;
	}
	case EditTool::Corridor:
		if (!stroke.right) {
			for (const Point cell : DraggedCells())
				SetCell(cell.x, cell.y, MapCell::Floor);
		}
		break;
	default:
		break;
	}
	if (!(TakeSnapshot() == stroke.before))
		PushUndo(stroke.before);
}

/** Applies the freehand tools along the cursor's path, so a quick drag leaves no gaps. */
void ContinueStroke(Point cell)
{
	const auto apply = [&](Point at) {
		switch (tool) {
		case EditTool::Floor:
			PaintBrushAt(at, stroke.right ? MapCell::Rock : MapCell::Floor);
			break;
		case EditTool::Rock:
			PaintBrushAt(at, stroke.right ? MapCell::Floor : MapCell::Rock);
			break;
		case EditTool::Erase:
			EraseAt(at);
			break;
		default:
			break;
		}
	};
	Point at = stroke.last;
	const int steps = std::max(std::abs(cell.x - at.x), std::abs(cell.y - at.y));
	for (int i = 1; i <= steps; i++) {
		apply({ at.x + (cell.x - at.x) * i / steps, at.y + (cell.y - at.y) * i / steps });
	}
	if (steps == 0)
		apply(cell);
	stroke.last = cell;
}

void BeginStroke(bool right)
{
	const std::optional<Point> cell = CursorCell();
	if (!cell)
		return;
	stroke.active = true;
	stroke.right = right;
	stroke.shift = IsShiftHeld();
	stroke.anchor = *cell;
	stroke.last = *cell;
	stroke.before = TakeSnapshot();

	switch (tool) {
	case EditTool::Floor:
	case EditTool::Rock:
	case EditTool::Erase:
		ContinueStroke(*cell);
		break;
	case EditTool::Wall:
	case EditTool::Room:
		break;
	case EditTool::Corridor:
		if (right) {
			// Nothing to undo with a right-drag: it just cancels.
			stroke.active = false;
		}
		break;
	case EditTool::Door:
		PlaceOpening(*cell, MapCell::Door, right);
		break;
	case EditTool::Arch:
		PlaceOpening(*cell, MapCell::Arch, right);
		break;
	case EditTool::Entrance:
	case EditTool::Vault:
		PlaceStairs(*cell, tool == EditTool::Entrance, right);
		break;
	case EditTool::Object:
		PlaceObject(cursPosition, right);
		break;
	case EditTool::Stamp:
		if (right) {
			// Right-click drops the stamp on show; nothing else to undo.
			DropStamp();
			stroke.active = false;
		}
		break;
	}
}

void Undo()
{
	if (stampPreview.active) {
		// The stamp on show isn't kept yet: Undo is dropping it.
		DropStamp();
		return;
	}
	if (undoStack.empty())
		return;
	redoStack.push_back(TakeSnapshot());
	RestoreSnapshot(undoStack.back());
	undoStack.pop_back();
}

void Redo()
{
	SettleStamp();
	if (redoStack.empty())
		return;
	undoStack.push_back(TakeSnapshot());
	RestoreSnapshot(redoStack.back());
	redoStack.pop_back();
}

void SelectTool(EditTool next)
{
	if (stroke.active)
		FinishStroke(); // it finishes as the tool it began with
	if (next == EditTool::Object && tool == EditTool::Object)
		OpenPropList();
	if (next == EditTool::Stamp && tool == EditTool::Stamp)
		OpenStampList();
	else
		SettleStamp();
	tool = next;
	if (next == EditTool::Corridor && brushSize < 2)
		brushSize = 2;
}

/**
 * Lights and scenery for the tileset, by the same pass as the campaign's maps (keeper_dress.hpp): off the
 * heroes' way, and never changing their route. Pressed again it adds only what's missing; Undo takes it out.
 */
void DressLevel()
{
	if (model.entrance == NoMapPosition || model.vault == NoMapPosition) {
		SetFeedback("Place the entrance and the vault first: the dressing keeps off the heroes' way", true);
		return;
	}
	SettleStamp();
	const Snapshot before = TakeSnapshot();
	const DressResult result = DressMap(model, DressOptions {});
	levelStale = true;
	if (result.lightsAdded + result.sceneryAdded == 0) {
		SetFeedback(result.notes.empty() ? "Nothing more fits: it's dressed" : result.notes.front(), !result.notes.empty());
		return;
	}
	PushUndo(before);
	dirty = true;
	SetFeedback(fmt::format("Added {} lights and {} props, off the heroes' way. Undo takes them out", result.lightsAdded, result.sceneryAdded));
}

void ToggleDecor()
{
	decorPreview = !decorPreview;
	levelStale = true;
	SetFeedback(decorPreview ? "Showing wall and floor variants, as a saved map will have them" : "Showing plain walls while editing");
}

// ---- the panel ----------------------------------------------------------------------------------

std::vector<Button> PanelButtons()
{
	std::vector<Button> buttons;
	const int uiWidth = gnScreenWidth / UiPixelScale();
	const int left = 6;
	const int right = uiWidth - RightColumnWidth - 12;
	int x = left;
	int y = PanelTop() + 5;
	const auto add = [&](std::string label, ActionKind action, size_t index, bool selected, bool enabled = true) {
		const int width = std::max<int>(GetLineWidth(label) + 10, 34);
		if (x + width > right) {
			x = left;
			y += ButtonHeight + 3;
		}
		buttons.push_back({ { { x, y }, { width, ButtonHeight } }, std::move(label), action, index, selected, enabled });
		x += width + 3;
	};
	for (size_t i = 0; i < NumEditTools; i++) {
		const auto thisTool = static_cast<EditTool>(i);
		// A controller has no number keys to go with the labels.
		const std::string key = IsPadInUse() ? std::string() : fmt::format("{} ", Tools[i].key);
		std::string label = fmt::format("{}{}", key, Tools[i].name);
		if (thisTool == EditTool::Object)
			label = fmt::format("{}{}", key, MapObjectName(objectKind));
		if (thisTool == EditTool::Stamp)
			label = fmt::format("{}{}", key, CurrentStamp().label);
		bool enabled = true;
		if (thisTool == EditTool::Door)
			enabled = MapTilesetHasDoors(model.tileset);
		if (thisTool == EditTool::Arch)
			enabled = MapTilesetHasArches(model.tileset);
		add(std::move(label), ActionKind::Tool, i, tool == thisTool, enabled);
	}
	x = left;
	y += ButtonHeight + 7;
	add("Undo", ActionKind::Undo, 0, false, !undoStack.empty() || stampPreview.active);
	add("Redo", ActionKind::Redo, 0, false, !redoStack.empty());
	add("New", ActionKind::New, 0, false);
	add("Save", ActionKind::Save, 0, false);
	add("Save As", ActionKind::SaveAs, 0, false);
	add("Load", ActionKind::Load, 0, false);
	add(fmt::format("Set: {}", MapTilesetName(model.tileset)), ActionKind::Tileset, 0, false);
	add("Author", ActionKind::Author, 0, false);
	add(IsViewZoomed() ? "Zoom: 2x" : "Zoom: 1x", ActionKind::Zoom, 0, IsViewZoomed());
	add("Grid", ActionKind::Grid, 0, showGrid);
	add("Variants", ActionKind::Decor, 0, decorPreview);
	add("Dress", ActionKind::Dress, 0, false, model.entrance != NoMapPosition && model.vault != NoMapPosition);
	add(IsPadInUse() ? "Test" : "F5 Test", ActionKind::Test, 0, false, !HasBlockingProblem());
	return buttons;
}

void DoAction(const Button &button)
{
	switch (button.action) {
	case ActionKind::Tool:
		SelectTool(static_cast<EditTool>(button.tool));
		break;
	case ActionKind::Undo:
		Undo();
		break;
	case ActionKind::Redo:
		Redo();
		break;
	case ActionKind::New:
		NewMap();
		break;
	case ActionKind::Save:
		Save();
		break;
	case ActionKind::SaveAs:
		OpenTextModal(Modal::SaveName, model.name);
		break;
	case ActionKind::Load:
		modal = Modal::LoadList;
		modalList = ListMapFiles(/*includeScratch=*/true);
		modalScroll = 0;
		modalRow = 0;
		modalNote = modalList.empty() ? "No maps saved yet" : "";
		break;
	case ActionKind::Tileset:
		modal = Modal::TilesetList;
		modalList.clear();
		for (size_t i = 0; i < NumMapTilesets; i++)
			modalList.emplace_back(MapTilesetName(static_cast<MapTileset>(i)));
		modalScroll = 0;
		modalRow = 0;
		modalNote.clear();
		break;
	case ActionKind::Author:
		OpenTextModal(Modal::Author, model.author);
		break;
	case ActionKind::Zoom:
		SetViewZoom(!IsViewZoomed());
		break;
	case ActionKind::Grid:
		showGrid = !showGrid;
		break;
	case ActionKind::Decor:
		ToggleDecor();
		break;
	case ActionKind::Dress:
		DressLevel();
		break;
	case ActionKind::Test:
		StartTest();
		break;
	}
}

/** A tool's help is written for a mouse: says it in buttons when a controller is in use. */
std::string ToolHelp(std::string_view help)
{
	std::string text(help);
	if (!IsPadInUse())
		return text;
	const auto replace = [&](std::string_view from, const std::string &to) {
		for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
			text.replace(at, from.size(), to);
	};
	const std::string confirm = InputName(PadAction::Confirm);
	const std::string order = InputName(PadAction::Order);
	replace("Right-drag", fmt::format("{}-drag", order));
	replace("Right-click", order);
	replace("Drag", fmt::format("{}-drag", confirm));
	replace("Click", confirm);
	replace("click", confirm);
	replace("Shift", InputName(PadAction::Modifier));
	replace("[ ]", fmt::format("{} {}", PadButtonName(ControllerButton_BUTTON_LEFTSHOULDER), PadButtonName(ControllerButton_BUTTON_RIGHTSHOULDER)));
	return text;
}

void DrawFrame(const Surface &out, const Rectangle &rect, uint8_t color)
{
	DrawHorizontalLine(out, rect.position, rect.size.width, color);
	DrawHorizontalLine(out, rect.position + Displacement { 0, rect.size.height - 1 }, rect.size.width, color);
	DrawVerticalLine(out, rect.position, rect.size.height, color);
	DrawVerticalLine(out, rect.position + Displacement { rect.size.width - 1, 0 }, rect.size.height, color);
}

Rectangle ModalRect()
{
	const int uiWidth = gnScreenWidth / UiPixelScale();
	int height = 86;
	if (modal == Modal::LoadList || modal == Modal::TilesetList || modal == Modal::PropList || modal == Modal::StampList)
		height = 44 + ModalRows * ModalRowHeight;
	return { { (uiWidth - ModalWidth) / 2, std::max(10, PanelTop() - height - 10) }, { ModalWidth, height } };
}

std::string_view ModalTitle()
{
	switch (modal) {
	case Modal::SaveName:
		return "Save the map as";
	case Modal::Author:
		return "Author";
	case Modal::LoadList:
		return "Load a map";
	case Modal::TilesetList:
		return "Tileset";
	case Modal::PropList:
		return propListGroup ? MapObjectGroupName(*propListGroup) : "Object";
	case Modal::StampList:
		return "Room stamp";
	case Modal::ConfirmNew:
		return "Start a new map?";
	case Modal::None:
		break;
	}
	return "";
}

std::optional<size_t> ModalRowAt(Point mouse)
{
	if (modal != Modal::LoadList && modal != Modal::TilesetList && modal != Modal::PropList && modal != Modal::StampList)
		return std::nullopt;
	const Rectangle box = ModalRect();
	const Rectangle rows { box.position + Displacement { 8, 24 }, { box.size.width - 16, ModalRows * ModalRowHeight } };
	if (!Contains(rows, mouse))
		return std::nullopt;
	const size_t row = static_cast<size_t>((mouse.y - rows.position.y) / ModalRowHeight + modalScroll);
	return row < modalList.size() ? std::optional<size_t> { row } : std::nullopt;
}

void DrawModal(const Surface &out)
{
	const Rectangle box = ModalRect();
	FillRect(out, box.position.x, box.position.y, box.size.width, box.size.height, PAL16_GRAY + 14);
	DrawFrame(out, box, PAL16_YELLOW + 4);
	const Point text = box.position + Displacement { 8, 6 };
	const int width = box.size.width - 16;
	DrawString(out, ModalTitle(), { text, { width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });

	if (modal == Modal::SaveName || modal == Modal::Author) {
		const Rectangle field { text + Displacement { 0, 20 }, { width, 18 } };
		FillRect(out, field.position.x, field.position.y, field.size.width, field.size.height, 0);
		DrawFrame(out, field, PAL16_GRAY + 6);
		const bool caretOn = (SDL_GetTicks() / 400) % 2 == 0;
		DrawString(out, fmt::format("{}{}", modalText, caretOn ? "_" : ""), { field.position + Displacement { 4, 2 }, { width - 8, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite });
		const std::string keys = IsPadInUse() ? fmt::format("{}: accept   {}: cancel", InputName(PadAction::Confirm), InputName(PadAction::Cancel)) : std::string("Enter: accept   Esc: cancel");
		DrawString(out, modalNote.empty() ? std::string_view(keys) : std::string_view(modalNote), { text + Displacement { 0, 44 }, { width, 14 } },
		    { .flags = UiFlags::FontSize12 | (modalNote.empty() ? UiFlags::ColorWhite : UiFlags::ColorRed) });
		return;
	}

	if (modal == Modal::ConfirmNew) {
		DrawString(out, "Unsaved changes: a copy goes to _autosave.", { text + Displacement { 0, 20 }, { width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite });
		DrawString(out, "Enter: start a new map   Esc: keep this one", { text + Displacement { 0, 44 }, { width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
		return;
	}

	const std::optional<size_t> hovered = ModalRowAt(UiMousePosition());
	for (int row = 0; row < ModalRows; row++) {
		const size_t index = static_cast<size_t>(row + modalScroll);
		if (index >= modalList.size())
			break;
		const Point at = text + Displacement { 0, 18 + row * ModalRowHeight };
		if (hovered ? *hovered == index : index == static_cast<size_t>(modalRow))
			FillRect(out, at.x, at.y, width, ModalRowHeight, PAL16_RED + 12);
		bool current = modalList[index] == fileName;
		if (modal == Modal::TilesetList)
			current = index == static_cast<size_t>(model.tileset);
		else if (modal == Modal::PropList && index < propListRows.size()) {
			const PropRow &propRow = propListRows[index];
			current = propRow.type == PropRow::Type::Kind ? propRow.kind == objectKind : (propRow.type == PropRow::Type::Group && propRow.group == GetMapObjectTraits(objectKind).group);
		}
		else if (modal == Modal::StampList)
			current = index < stampListStamps.size() && stampListStamps[index] == &CurrentStamp();
		DrawString(out, modalList[index], { at + Displacement { 4, 1 }, { width - 8, 14 } }, { .flags = UiFlags::FontSize12 | (current ? UiFlags::ColorGold : UiFlags::ColorWhite) });
	}
	if (!modalNote.empty())
		DrawString(out, modalNote, { text + Displacement { 0, 20 }, { width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorRed });
}

void CommitTextModal()
{
	switch (modal) {
	case Modal::SaveName: {
		std::string name = modalText;
		const std::string file = MapFileName(name);
		// One Enter to warn, a second with the same name to overwrite.
		const std::string warning = fmt::format("{} exists. {} again to replace it.", file, IsPadInUse() ? InputName(PadAction::Confirm) : std::string("Enter"));
		if (file != fileName && FileExists(MapsFolder() + file) && modalNote != warning) {
			modalNote = warning;
			return;
		}
		model.name = std::move(name);
		fileName = file;
		modal = Modal::None;
		SaveToCurrentFile();
		break;
	}
	case Modal::Author:
		model.author = modalText;
		dirty = true;
		modal = Modal::None;
		break;
	default:
		modal = Modal::None;
		break;
	}
}

bool HandleTextKey(SDL_Keycode key)
{
	switch (key) {
	case SDLK_ESCAPE:
		modal = Modal::None;
		return true;
	case SDLK_RETURN:
	case SDLK_KP_ENTER:
		CommitTextModal();
		return true;
	case SDLK_BACKSPACE:
		if (!modalText.empty())
			modalText.pop_back();
		modalNote.clear();
		return true;
	default:
		break;
	}
	const size_t limit = modal == Modal::Author ? MapMaxAuthor : MapMaxName;
	if (modalText.size() >= limit)
		return true;
	const bool shift = IsShiftHeld();
	char typed = 0;
	if (key >= SDLK_A && key <= SDLK_Z)
		typed = static_cast<char>((shift ? 'A' : 'a') + (key - SDLK_A));
	else if (key >= SDLK_0 && key <= SDLK_9)
		typed = static_cast<char>('0' + (key - SDLK_0));
	else if (key == SDLK_SPACE)
		typed = ' ';
	else if (key == SDLK_MINUS)
		typed = shift ? '_' : '-';
	if (typed != 0) {
		modalText.push_back(typed);
		modalNote.clear();
	}
	return true;
}

void ClickModalRow(size_t row)
{
	const Modal kind = modal;
	modal = Modal::None;
	if (kind == Modal::LoadList) {
		const std::string name = modalList[row];
		LoadMapNamed(name);
	} else if (kind == Modal::TilesetList) {
		ChangeTileset(static_cast<MapTileset>(row));
	} else if (kind == Modal::PropList && row < propListRows.size()) {
		const PropRow picked = propListRows[row];
		switch (picked.type) {
		case PropRow::Type::Group:
			OpenPropList(picked.group);
			break;
		case PropRow::Type::Back:
			OpenPropList(std::nullopt);
			break;
		case PropRow::Type::Kind:
			objectKind = picked.kind;
			break;
		}
	} else if (kind == Modal::StampList && row < stampListStamps.size()) {
		stampIndex = static_cast<size_t>(stampListStamps[row] - RoomStamps().data());
		ChoseStamp();
	}
}

// ---- world overlay ---------------------------------------------------------------------------------

struct CellDiamond {
	Point top, right, bottom, left;
};

/** Screen corners of a megatile, whose four world tiles form one big diamond. */
CellDiamond DiamondOf(Point cell)
{
	const int zoom = IsViewZoomed() ? 2 : 1;
	const Point first = cell.megaToWorld();
	const auto centre = [&](int dx, int dy) {
		const Point half = GetScreenPosition(first + Displacement { dx, dy }) + Displacement { TILE_WIDTH / 2, -TILE_HEIGHT / 2 };
		return Point { half.x * zoom, half.y * zoom };
	};
	const int hw = TILE_WIDTH / 2 * zoom;
	const int hh = TILE_HEIGHT / 2 * zoom;
	return {
		centre(0, 0) + Displacement { 0, -hh },
		centre(1, 0) + Displacement { hw, 0 },
		centre(1, 1) + Displacement { 0, hh },
		centre(0, 1) + Displacement { -hw, 0 },
	};
}

void DrawWorldLine(const Surface &out, Point a, Point b, uint8_t color, int maxY)
{
	// Off-screen lines are common with a whole map's grid: skip them cheaply.
	if ((a.x < 0 && b.x < 0) || (a.y < 0 && b.y < 0) || (a.x >= out.w() && b.x >= out.w()) || (a.y >= maxY && b.y >= maxY))
		return;
	int dx = std::abs(b.x - a.x);
	int dy = -std::abs(b.y - a.y);
	const int sx = a.x < b.x ? 1 : -1;
	const int sy = a.y < b.y ? 1 : -1;
	int err = dx + dy;
	Point p = a;
	while (true) {
		if (p.x >= 0 && p.y >= 0 && p.x < out.w() && p.y < maxY)
			out.SetPixel(p, color);
		if (p == b)
			break;
		const int e2 = 2 * err;
		if (e2 >= dy) {
			err += dy;
			p.x += sx;
		}
		if (e2 <= dx) {
			err += dx;
			p.y += sy;
		}
	}
}

void DrawCellOutline(const Surface &out, Point cell, uint8_t color, int maxY)
{
	const CellDiamond d = DiamondOf(cell);
	DrawWorldLine(out, d.left, d.top, color, maxY);
	DrawWorldLine(out, d.top, d.right, color, maxY);
	DrawWorldLine(out, d.right, d.bottom, color, maxY);
	DrawWorldLine(out, d.bottom, d.left, color, maxY);
}

/** One world tile's diamond. */
void DrawTileOutline(const Surface &out, Point tile, uint8_t color, int maxY)
{
	const int zoom = IsViewZoomed() ? 2 : 1;
	const Point half = GetScreenPosition(tile) + Displacement { TILE_WIDTH / 2, -TILE_HEIGHT / 2 };
	const Point centre { half.x * zoom, half.y * zoom };
	const int hw = TILE_WIDTH / 2 * zoom;
	const int hh = TILE_HEIGHT / 2 * zoom;
	const Point top = centre + Displacement { 0, -hh };
	const Point right = centre + Displacement { hw, 0 };
	const Point bottom = centre + Displacement { 0, hh };
	const Point left = centre + Displacement { -hw, 0 };
	DrawWorldLine(out, left, top, color, maxY);
	DrawWorldLine(out, top, right, color, maxY);
	DrawWorldLine(out, right, bottom, color, maxY);
	DrawWorldLine(out, bottom, left, color, maxY);
}

void DrawRectOutline(const Surface &out, Point topLeft, Size size, uint8_t color, int maxY)
{
	const CellDiamond a = DiamondOf(topLeft);
	const CellDiamond b = DiamondOf(topLeft + Displacement { size.width - 1, 0 });
	const CellDiamond c = DiamondOf(topLeft + Displacement { size.width - 1, size.height - 1 });
	const CellDiamond d = DiamondOf(topLeft + Displacement { 0, size.height - 1 });
	DrawWorldLine(out, a.top, b.right, color, maxY);
	DrawWorldLine(out, b.right, c.bottom, color, maxY);
	DrawWorldLine(out, c.bottom, d.left, color, maxY);
	DrawWorldLine(out, d.left, a.top, color, maxY);
}

void DrawLabel(const Surface &out, Point cell, std::string_view text, UiFlags color, int maxY)
{
	const CellDiamond d = DiamondOf(cell);
	const Point at { d.top.x - 60, d.top.y + (d.bottom.y - d.top.y) / 2 - 7 };
	if (at.y < 0 || at.y + 14 >= maxY || at.x < -60 || at.x >= out.w())
		return;
	DrawString(out, text, { at, { 120, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | color | UiFlags::Outlined });
}

void DrawStairsMarker(const Surface &out, Point position, bool entrance, int maxY)
{
	if (position.x < 0)
		return;
	const Size size = MapStairsSize(model.tileset, entrance);
	const uint8_t color = entrance ? PAL16_BLUE + 2 : PAL16_YELLOW + 2;
	DrawRectOutline(out, position, size, color, maxY);
	DrawLabel(out, position + Displacement { size.width / 2, size.height / 2 }, entrance ? "Entrance" : "Vault", entrance ? UiFlags::ColorBlue : UiFlags::ColorGold, maxY);
}

} // namespace

// ---- public -----------------------------------------------------------------------------------------

bool IsEditorActive()
{
	return IsActive() && CurrentSetup().mode == GameMode::MapEditor && !gbIsMultiplayer;
}

bool EditorCapturesKeys()
{
	if (!IsEditorActive())
		return false;
	return modal != Modal::None || IsCtrlHeld();
}

void EditorPrepareSession()
{
	if (sessionStarted)
		return;
	sessionStarted = true;
	testing = false;
	model = {};
	InitNewMap();
	const std::string &path = CurrentSetup().mapFile;
	if (!path.empty()) {
		const tl::expected<std::vector<std::byte>, std::string> bytes = ReadMapBytes(path);
		if (bytes.has_value()) {
			tl::expected<KeepMap, std::string> loaded = ParseMap(bytes->data(), bytes->size());
			if (loaded.has_value()) {
				model = std::move(*loaded); // saving asks for a name: `path` needn't be in the maps folder
			} else {
				SetFeedback(fmt::format("Could not load {}: {}", path, loaded.error()), true);
			}
		} else {
			SetFeedback(fmt::format("Could not load {}: {}", path, bytes.error()), true);
		}
	}
	SetLiveMap(&model, &decorPreview);
}

void EditorOnLevelLoaded()
{
	levelReloading = false;
	if (CurrentSetup().mode != GameMode::MapEditor || gbIsMultiplayer || currlevel != KeepLevel || setlevel)
		return;
	// InitItems scatters loot; a map being edited has none, and nothing else clears it when walls move.
	std::memset(dItem, 0, sizeof(dItem));
	problems = ValidateMap(model);
	levelStale = false;
	stroke.active = false;
	if (cameraToRestore) {
		CenterCamera(*cameraToRestore);
		cameraToRestore.reset();
	} else {
		CenterCamera(MapSpawnTile(model));
	}
}

void EditorOnGameEnded()
{
	if (testing) {
		SetCurrentSetup(setupBeforeTest);
		testing = false;
	}
	levelReloading = false;
	SetLiveMap(nullptr, nullptr);
	sessionStarted = false;
	modal = Modal::None;
	stroke.active = false;
}

void EditorBeforeGameFreed()
{
	if (sessionStarted)
		Autosave();
}

void EditorUpdateFrame()
{
	if (!IsEditorActive() || levelReloading)
		return;
	if (stroke.active) {
		const clicktype button = stroke.right ? CLICK_RIGHT : CLICK_LEFT;
		if (sgbMouseDown != button) {
			FinishStroke();
		} else if (const std::optional<Point> cell = CursorCell(); cell && *cell != stroke.last) {
			if (IsAnyOf(tool, EditTool::Floor, EditTool::Rock, EditTool::Erase))
				ContinueStroke(*cell);
			else
				stroke.last = *cell;
			stroke.shift = IsShiftHeld();
		}
	}
	SetWantedMapObjectArt(objectKind);
	if (levelStale)
		RefreshLevel();
	// A prop whose art the level didn't load (placed, dressed, undone back): the art loads with the level.
	if (!stroke.active && MapNeedsArtReload(model)) {
		cameraToRestore = ViewPosition;
		ReloadLevel();
	}
}

bool EditorOnTestKey(SDL_Keycode key)
{
	if (!testing || key != SDLK_F10)
		return false;
	EditorEndTestRun();
	return true;
}

#ifdef KEEPER_DEVCTL
/** The middle of the last room `editor stampat` stamped, for `editor stampcam` in a test run. */
Point devStampMiddle = NoMapPosition;

std::string DevEditor(const std::string &what, const std::string &arg)
{
	if (what == "save")
		Save();
	else if (what == "load")
		LoadMapNamed(arg);
	else if (what == "new")
		NewMap();
	else if (what == "undo")
		Undo();
	else if (what == "dress")
		DressLevel();
	else if (what == "kind") {
		// editor kind <id>: the Object tool, with this kind
		if (const std::optional<MapObjectKind> kind = MapObjectKindNamed(arg); kind) {
			objectKind = *kind;
			tool = EditTool::Object;
		}
	} else if (what == "proplist") {
		// editor proplist [group number]: the list, as a click on the Object tool opens it
		tool = EditTool::Object;
		if (arg.empty())
			OpenPropList();
		else
			OpenPropList(static_cast<MapObjectGroup>(std::clamp(std::atoi(arg.c_str()), 0, static_cast<int>(NumMapObjectGroups) - 1)));
	}
	else if (what == "stamp") {
		// `editor stamp <name>`: the Stamp tool with that stamp
		if (const RoomStamp *stamp = RoomStampNamed(arg); stamp != nullptr) {
			if (tool != EditTool::Stamp)
				SelectTool(EditTool::Stamp);
			stampIndex = static_cast<size_t>(stamp - RoomStamps().data());
			ChoseStamp();
		}
	} else if (what == "stampat") {
		// `editor stampat <x>,<y>`: as a click on that world tile with the Stamp tool
		const size_t comma = arg.find(',');
		if (comma != std::string::npos) {
			const Point tile { std::atoi(arg.c_str()), std::atoi(arg.c_str() + comma + 1) };
			const std::vector<Point> cells = RoomCellsAt(model, { (tile.x - 16) / 2, (tile.y - 16) / 2 });
			PreviewStamp(cells);
			// The camera on the room stamped, for a screenshot (a test run starts where the editor's camera is).
			if (!cells.empty()) {
				Point sum { 0, 0 };
				for (const Point cell : cells)
					sum += Displacement { cell.x, cell.y };
				devStampMiddle = Point { sum.x / static_cast<int>(cells.size()), sum.y / static_cast<int>(cells.size()) }.megaToWorld() + Displacement { 1, 1 };
				CenterCamera(devStampMiddle);
			}
		}
	} else if (what == "stairs" || what == "vaultstairs") {
		// `editor stairs <x>,<y>` (or vaultstairs): as a click on that megatile with the Entrance (Vault) tool
		const size_t comma = arg.find(',');
		if (comma != std::string::npos) {
			const bool entrance = what == "stairs";
			feedback.clear();
			PlaceStairs({ std::atoi(arg.c_str()), std::atoi(arg.c_str() + comma + 1) }, entrance, false);
			const Point at = entrance ? model.entrance : model.vault;
			return fmt::format("stairs {} at {},{}: {}", entrance ? "entrance" : "vault", at.x, at.y, feedback);
		}
	} else if (what == "stairsinfo") {
		std::string said = fmt::format("stairs entrance {},{} vault {},{}; problems:", model.entrance.x, model.entrance.y, model.vault.x, model.vault.y);
		for (const MapProblem &problem : problems)
			said += fmt::format(" [{}{}]", problem.blocking ? "" : "warning: ", problem.text);
		return said + "; feedback: " + feedback;
	} else if (what == "stampcam") {
		if (devStampMiddle != NoMapPosition)
			CenterCamera(devStampMiddle);
	} else if (what == "stampkeep")
		KeepStamp();
	else if (what == "stampreroll")
		RerollStamp();
	else if (what == "stampdrop")
		DropStamp();
	else if (what == "stampinfo")
	{
		std::string added;
		for (const MapObject &object : stampPreview.result.added)
			added += fmt::format(" {}@{},{}{}", MapObjectName(object.kind), object.position.x, object.position.y, object.facesY() ? "y" : "");
		return fmt::format("stamp {} active={} placed={} wanted={} lights={} capped={} objects={} undo={} middle={},{} added:{}", CurrentStamp().name, stampPreview.active, stampPreview.result.placed, stampPreview.result.wanted, stampPreview.result.lights, stampPreview.result.capped, model.objects.size(), undoStack.size(), devStampMiddle.x, devStampMiddle.y, added);
	}
	return {};
}
#endif

bool IsEditorTestRun()
{
	return testing;
}

void EditorEndTestRun()
{
	if (!testing || levelReloading)
		return;
	testing = false;
	SetCurrentSetup(setupBeforeTest);
	// Off with the raid while the level is still whole; the reload replaces everything else.
	ResetGame(State.entrance, State.vault);
	InitPlrMsg(); // the test's raid log is over with it
	ReloadLevel();
}

bool EditorOnKeyDown(SDL_Keycode key)
{
	if (levelReloading)
		return true;
	if (modal == Modal::SaveName || modal == Modal::Author)
		return HandleTextKey(key);
	if (modal == Modal::ConfirmNew && (key == SDLK_RETURN || key == SDLK_KP_ENTER)) {
		modal = Modal::None;
		NewMapNow();
		return true;
	}
	if (modal != Modal::None) {
		if (key == SDLK_ESCAPE)
			modal = Modal::None;
		if ((modal == Modal::LoadList || modal == Modal::TilesetList || modal == Modal::PropList || modal == Modal::StampList) && !modalList.empty()) {
			const int last = static_cast<int>(modalList.size()) - 1;
			if (key == SDLK_DOWN || key == SDLK_UP) {
				modalRow = std::clamp(modalRow + (key == SDLK_DOWN ? 1 : -1), 0, last);
				modalScroll = std::clamp(modalScroll, std::max(modalRow - ModalRows + 1, 0), modalRow);
			} else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
				ClickModalRow(ModalRowAt(UiMousePosition()).value_or(static_cast<size_t>(modalRow)));
			}
		}
		return true;
	}

	if (IsCtrlHeld()) {
		// Whatever this does, it shouldn't happen to half a drag.
		if (stroke.active)
			FinishStroke();
		switch (key) {
		case SDLK_Z:
			if (IsShiftHeld())
				Redo();
			else
				Undo();
			return true;
		case SDLK_Y:
			Redo();
			return true;
		case SDLK_S:
			if (IsShiftHeld())
				OpenTextModal(Modal::SaveName, model.name);
			else
				Save();
			return true;
		case SDLK_N:
			NewMap();
			return true;
		case SDLK_O:
		case SDLK_L:
			DoAction({ {}, {}, ActionKind::Load });
			return true;
		default:
			return true;
		}
	}

	if (key >= SDLK_1 && key <= SDLK_9) {
		SelectTool(static_cast<EditTool>(key - SDLK_1));
		return true;
	}
	switch (key) {
	case SDLK_0:
		SelectTool(EditTool::Object);
		return true;
	case SDLK_E:
		SelectTool(EditTool::Erase);
		return true;
	case SDLK_T:
		SelectTool(EditTool::Stamp);
		return true;
	case SDLK_R:
		if (tool == EditTool::Stamp)
			RerollStamp();
		return true;
	case SDLK_RETURN:
	case SDLK_KP_ENTER:
		KeepStamp();
		return true;
	case SDLK_LEFTBRACKET:
	case SDLK_RIGHTBRACKET:
		if (tool == EditTool::Stamp) {
			CycleStamp(key == SDLK_LEFTBRACKET ? -1 : 1);
			ChoseStamp();
			if (!stampPreview.active)
				SetFeedback(fmt::format("Stamp: {}", CurrentStamp().label));
		} else {
			brushSize = std::clamp(brushSize + (key == SDLK_LEFTBRACKET ? -1 : 1), 1, MaxBrush);
		}
		return true;
	case SDLK_G:
		showGrid = !showGrid;
		return true;
	case SDLK_P:
		ToggleDecor();
		return true;
	case SDLK_F5:
		if (stroke.active)
			FinishStroke();
		StartTest();
		return true;
	case SDLK_SPACE:
	case SDLK_HOME:
		CenterCamera(MapSpawnTile(model));
		return true;
	case SDLK_V:
		if (model.vault.x >= 0)
			CenterCamera(model.vault.megaToWorld());
		return true;
	case SDLK_EQUALS:
	case SDLK_KP_PLUS:
		SetViewZoom(true);
		return true;
	case SDLK_MINUS:
	case SDLK_KP_MINUS:
		SetViewZoom(false);
		return true;
	case SDLK_ESCAPE:
		if (stroke.active) {
			// Wall, Room and Hall only change anything on release; freehand tools roll back.
			RestoreSnapshot(stroke.before);
			stroke.active = false;
			return true;
		}
		if (stampPreview.active) {
			DropStamp();
			return true;
		}
		return false; // the game menu
	default:
		// Everything else would be a hero hotkey.
		return true;
	}
}

bool EditorOnMouseDown(bool right)
{
	if (levelReloading)
		return true;
	// A press and release inside one frame: the last stroke hasn't been closed off yet.
	if (stroke.active)
		FinishStroke();
	const Point mouse = UiMousePosition();
	if (modal != Modal::None) {
		if (!right) {
			if (const std::optional<size_t> row = ModalRowAt(mouse); row) {
				ClickModalRow(*row);
				return true;
			}
		}
		if (!Contains(ModalRect(), mouse) || right)
			modal = Modal::None;
		return true;
	}
	if (mouse.y >= PanelTop()) {
		if (right)
			return true;
		for (const Button &button : PanelButtons()) {
			if (button.enabled && Contains(button.rect, mouse)) {
				DoAction(button);
				return true;
			}
		}
		return true;
	}
	BeginStroke(right);
	return true;
}

bool EditorOnMouseWheel(int deltaY)
{
	if (levelReloading)
		return true;
	if (modal == Modal::LoadList || modal == Modal::TilesetList || modal == Modal::PropList || modal == Modal::StampList) {
		const int maxScroll = std::max(0, static_cast<int>(modalList.size()) - ModalRows);
		modalScroll = std::clamp(modalScroll + (deltaY > 0 ? -1 : 1), 0, maxScroll);
		return true;
	}
	if (modal != Modal::None)
		return true;
	SetViewZoom(deltaY > 0);
	return true;
}

void EditorDrawWorld(const Surface &out)
{
	const int maxY = PanelTop() * UiPixelScale();

	if (showGrid) {
		for (int x = 0; x < DMAXX; x++) {
			for (int y = 0; y < DMAXY; y++)
				DrawCellOutline(out, { x, y }, PAL16_GRAY + 11, maxY);
		}
	}
	DrawRectOutline(out, { 1, 1 }, { DMAXX - 2, DMAXY - 2 }, PAL16_RED + 6, maxY);

	for (int x = 0; x < DMAXX; x++) {
		for (int y = 0; y < DMAXY; y++) {
			const MapCell cell = model.cell(x, y);
			if (IsAnyOf(cell, MapCell::Door, MapCell::Arch))
				DrawCellOutline(out, { x, y }, cell == MapCell::Door ? PAL16_ORANGE + 2 : PAL16_BLUE + 6, maxY);
		}
	}
	DrawStairsMarker(out, model.entrance, true, maxY);
	DrawStairsMarker(out, model.vault, false, maxY);
	// The stamp on show: what it added.
	if (stampPreview.active) {
		for (const MapObject &object : stampPreview.result.added)
			DrawTileOutline(out, object.position, PAL16_YELLOW + 2, maxY);
	}

	if (IsMouseOverPanel())
		return;
	const std::optional<Point> hovered = CursorCell();
	if (!hovered)
		return;

	switch (tool) {
	case EditTool::Floor:
	case EditTool::Rock: {
		const uint8_t color = tool == EditTool::Floor ? PAL16_YELLOW + 2 : PAL16_GRAY + 2;
		const Point first { hovered->x - (brushSize - 1) / 2, hovered->y - (brushSize - 1) / 2 };
		DrawRectOutline(out, first, { brushSize, brushSize }, color, maxY);
		break;
	}
	case EditTool::Wall:
	case EditTool::Room:
	case EditTool::Corridor:
		if (stroke.active) {
			const uint8_t color = stroke.right ? PAL16_RED + 2 : PAL16_YELLOW + 2;
			for (const Point cell : DraggedCells())
				DrawCellOutline(out, cell, color, maxY);
		} else {
			DrawCellOutline(out, *hovered, PAL16_YELLOW + 2, maxY);
		}
		break;
	case EditTool::Entrance:
	case EditTool::Vault: {
		const bool entrance = tool == EditTool::Entrance;
		// Where a click would put them (the nearest wall, for those built into one), or red: no wall near.
		const std::optional<Point> spot = StairsSpotFor(*hovered, entrance);
		DrawRectOutline(out, spot.value_or(StairsPositionAt(*hovered, entrance)), MapStairsSize(model.tileset, entrance), !spot ? PAL16_RED + 2 : (entrance ? PAL16_BLUE + 4 : PAL16_YELLOW + 4), maxY);
		break;
	}
	case EditTool::Door:
	case EditTool::Arch:
	case EditTool::Erase:
		DrawCellOutline(out, *hovered, tool == EditTool::Erase ? PAL16_RED + 2 : PAL16_ORANGE + 4, maxY);
		break;
	case EditTool::Object: {
		const auto [object, why] = ObjectForClick(cursPosition);
		const Point at = object ? object->position : cursPosition;
		const bool onExisting = FindObjectForClick(cursPosition) != model.objects.end();
		DrawTileOutline(out, at, why.empty() || onExisting ? PAL16_YELLOW + 6 : PAL16_RED + 2, maxY);
		break;
	}
	case EditTool::Stamp:
		if (stroke.active && !stroke.right && stroke.anchor != stroke.last) {
			const Point first { std::min(stroke.anchor.x, stroke.last.x), std::min(stroke.anchor.y, stroke.last.y) };
			DrawRectOutline(out, first, { std::abs(stroke.last.x - stroke.anchor.x) + 1, std::abs(stroke.last.y - stroke.anchor.y) + 1 }, PAL16_YELLOW + 2, maxY);
		} else {
			// The room a click would stamp, red where there's none; nothing over the stamp on show, so it can be seen.
			const bool onPreview = stampPreview.active && std::find(stampPreview.cells.begin(), stampPreview.cells.end(), *hovered) != stampPreview.cells.end();
			const std::vector<Point> cells = onPreview ? std::vector<Point> {} : RoomCellsAt(model, *hovered);
			for (const Point cell : cells)
				DrawCellOutline(out, cell, PAL16_ORANGE + 4, maxY);
			if (cells.empty())
				DrawCellOutline(out, *hovered, onPreview ? PAL16_YELLOW + 2 : PAL16_RED + 2, maxY);
		}
		break;
	}
}

void EditorDrawUi(const Surface &out)
{
	const int top = PanelTop();
	FillRect(out, 0, top, out.w(), out.h() - top, 0);
	DrawHorizontalLine(out, { 0, top }, out.w(), PAL16_RED + 8);

	const Point mouse = UiMousePosition();
	const std::vector<Button> buttons = PanelButtons();
	int contentBottom = top;
	for (const Button &button : buttons) {
		const Rectangle &r = button.rect;
		FillRect(out, r.position.x, r.position.y, r.size.width, r.size.height, button.selected ? PAL16_RED + 12 : PAL16_GRAY + 14);
		uint8_t frame = PAL16_GRAY + 8;
		if (button.selected)
			frame = PAL16_YELLOW + 2;
		else if (button.enabled && Contains(r, mouse))
			frame = PAL16_GRAY;
		DrawFrame(out, r, frame);
		UiFlags color = UiFlags::ColorWhite;
		if (!button.enabled)
			color = UiFlags::ColorBlack;
		else if (button.selected)
			color = UiFlags::ColorGold;
		DrawString(out, button.label, { r.position + Displacement { 0, 2 }, { r.size.width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | color });
		contentBottom = std::max(contentBottom, r.position.y + r.size.height);
	}

	// What the tool does (or what's under the mouse), under the buttons.
	const int leftWidth = out.w() - RightColumnWidth - 18;
	std::string hint = ToolHelp(Tools[static_cast<size_t>(tool)].help);
	if (tool == EditTool::Door && model.tileset == MapTileset::Caves)
		hint = "Click floor in a straight stretch two or more wide: a fence crosses the cave there, with a gate in it. Right-click: remove.";
	if (tool == EditTool::Floor || tool == EditTool::Rock || tool == EditTool::Corridor)
		hint += fmt::format("  Size {}", brushSize);
	if (tool == EditTool::Stamp) {
		hint = fmt::format("{}: {}{}", CurrentStamp().label, hint, stampPreview.active ? "  " + StampKeysHint() : std::string());
	}
	if (tool == EditTool::Object && !IsMouseOverPanel()) {
		if (const std::string why = ObjectForClick(cursPosition).second; !why.empty() && FindObjectForClick(cursPosition) == model.objects.end())
			hint = fmt::format("{}: {}", MapObjectName(objectKind), why);
	}
	DrawString(out, WordWrapString(hint, leftWidth), { { 6, contentBottom + 5 }, { leftWidth, out.h() - contentBottom - 6 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite, .lineHeight = 13 });

	// Right column: the map and what's wrong with it.
	const int columnX = out.w() - RightColumnWidth - 6;
	const int columnWidth = RightColumnWidth;
	FillRect(out, columnX, top + 4, columnWidth, out.h() - top - 8, PAL16_GRAY + 14);
	DrawFrame(out, { { columnX, top + 4 }, { columnWidth, out.h() - top - 8 } }, PAL16_GRAY + 10);
	const std::string title = fmt::format("{}{}", model.name.empty() ? std::string_view("Untitled") : std::string_view(model.name), dirty ? " *" : "");
	DrawString(out, title, { { columnX + 6, top + 7 }, { columnWidth - 12, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
	const std::string by = model.author.empty() ? std::string(MapTilesetName(model.tileset)) : fmt::format("{}, by {}", MapTilesetName(model.tileset), model.author);
	DrawString(out, by, { { columnX + 6, top + 21 }, { columnWidth - 12, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite });

	int y = top + 38;
	if (problems.empty()) {
		DrawString(out, "Ready to play", { { columnX + 6, y }, { columnWidth - 12, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
	}
	for (const MapProblem &problem : problems) {
		if (y + 13 > out.h() - 6)
			break;
		const std::string wrapped = WordWrapString(problem.text, columnWidth - 12);
		DrawString(out, wrapped, { { columnX + 6, y }, { columnWidth - 12, out.h() - y - 6 } },
		    { .flags = UiFlags::FontSize12 | (problem.blocking ? UiFlags::ColorRed : UiFlags::ColorOrange), .lineHeight = 13 });
		y += 13 * static_cast<int>(1 + std::count(wrapped.begin(), wrapped.end(), '\n'));
	}

	if (!feedback.empty() && SDL_GetTicks() < feedbackUntil) {
		const int width = std::min<int>(GetLineWidth(feedback) + 16, out.w());
		const Rectangle line { { (out.w() - width) / 2, top - 22 }, { width, 18 } };
		DrawHalfTransparentRectTo(out, line.position.x, line.position.y, line.size.width, line.size.height);
		DrawString(out, feedback, { line.position + Displacement { 0, 2 }, { line.size.width, 14 } },
		    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (feedbackIsError ? UiFlags::ColorRed : UiFlags::ColorGold) });
	}

	if (modal != Modal::None)
		DrawModal(out);
}

// ---- controller ------------------------------------------------------------------------------------

namespace {

bool softKeyboardOpen = false;

bool IsTextModal()
{
	return modal == Modal::SaveName || modal == Modal::Author;
}

bool IsListModal()
{
	return modal == Modal::LoadList || modal == Modal::TilesetList || modal == Modal::PropList || modal == Modal::StampList;
}

Rectangle ModalRowRect(int row)
{
	const Rectangle box = ModalRect();
	return { box.position + Displacement { 8, 24 + row * ModalRowHeight }, { box.size.width - 16, ModalRowHeight } };
}

/** What a name can be spelled with, for a controller. */
constexpr std::string_view NameLetters = "abcdefghijklmnopqrstuvwxyz0123456789 -_ABCDEFGHIJKLMNOPQRSTUVWXYZ";

void OpenSoftKeyboard()
{
#ifdef __SWITCH__
	softKeyboardOpen = true;
	switch_start_text_input(ModalTitle(), modalText, static_cast<unsigned>(modal == Modal::Author ? MapMaxAuthor : MapMaxName) + 1);
#elif defined(KEEPER_DEVCTL)
	softKeyboardOpen = true; // the test script sends the text
	modalText.clear();
#endif
}

void PickNextTool(int delta)
{
	const int next = (static_cast<int>(tool) + delta + static_cast<int>(NumEditTools)) % static_cast<int>(NumEditTools);
	SelectTool(static_cast<EditTool>(next));
}

} // namespace

void EditorPadTargets(std::vector<Rectangle> &targets)
{
	if (IsTextModal())
		return;
	if (IsListModal()) {
		const int rows = std::min(ModalRows, static_cast<int>(modalList.size()) - modalScroll);
		for (int row = 0; row < rows; row++)
			targets.push_back(ModalRowRect(row));
		return;
	}
	for (const Button &button : PanelButtons()) {
		if (button.enabled)
			targets.push_back(button.rect);
	}
}

bool EditorPadNeedsFocus()
{
	return IsListModal() && !modalList.empty();
}

bool EditorPadScroll(const Rectangle &focused, AxisDirection dir)
{
	if (!IsListModal() || dir.y == AxisDirectionY_NONE)
		return false;
	const int before = modalScroll;
	const int maxScroll = std::max(0, static_cast<int>(modalList.size()) - ModalRows);
	if (dir.y == AxisDirectionY_DOWN && focused.position.y == ModalRowRect(ModalRows - 1).position.y)
		modalScroll = std::min(modalScroll + 1, maxScroll);
	else if (dir.y == AxisDirectionY_UP && focused.position.y == ModalRowRect(0).position.y)
		modalScroll = std::max(modalScroll - 1, 0);
	return modalScroll != before;
}

bool EditorPadButton(ControllerButton button, bool modifier, bool spellButton)
{
	if (levelReloading)
		return true;
	if (IsTextModal()) {
		switch (button) {
		case ControllerButton_BUTTON_A:
			softKeyboardOpen = false;
			HandleTextKey(SDLK_RETURN);
			break;
		case ControllerButton_BUTTON_B:
			softKeyboardOpen = false;
			HandleTextKey(SDLK_ESCAPE);
			break;
		case ControllerButton_BUTTON_X:
			HandleTextKey(SDLK_BACKSPACE);
			break;
		case ControllerButton_BUTTON_Y:
			OpenSoftKeyboard();
			break;
		default:
			break;
		}
		return true;
	}
	if (modal != Modal::None) {
		if (button == ControllerButton_BUTTON_B)
			modal = Modal::None;
		return button != ControllerButton_BUTTON_A;
	}
	switch (button) {
	case ControllerButton_BUTTON_Y:
		// Whatever this does, it shouldn't happen to half a drag.
		if (stroke.active)
			FinishStroke();
		if (modifier)
			Redo();
		else
			Undo();
		return true;
	case ControllerButton_BUTTON_LEFTSHOULDER:
		if (spellButton)
			PickNextTool(-1);
		else
			EditorOnKeyDown(SDLK_LEFTBRACKET);
		return true;
	case ControllerButton_BUTTON_RIGHTSHOULDER:
		if (spellButton)
			PickNextTool(1);
		else
			EditorOnKeyDown(SDLK_RIGHTBRACKET);
		return true;
	case ControllerButton_BUTTON_BACK:
		EditorOnKeyDown(SDLK_F5);
		return true;
	case ControllerButton_BUTTON_LEFTSTICK:
		SetViewZoom(!IsViewZoomed());
		return true;
	case ControllerButton_BUTTON_RIGHTSTICK:
		EditorOnKeyDown(modifier ? SDLK_V : SDLK_SPACE);
		return true;
	default:
		return false;
	}
}

bool EditorPadInText()
{
	return IsTextModal();
}

void EditorPadTextStep(AxisDirection dir)
{
	modalNote.clear();
	const size_t limit = modal == Modal::Author ? MapMaxAuthor : MapMaxName;
	if (dir.x == AxisDirectionX_LEFT) {
		if (!modalText.empty())
			modalText.pop_back();
		return;
	}
	if (dir.x == AxisDirectionX_RIGHT || modalText.empty()) {
		if (modalText.size() < limit)
			modalText.push_back(NameLetters.front());
		return;
	}
	if (dir.y == AxisDirectionY_NONE)
		return;
	const size_t count = NameLetters.size();
	const size_t found = NameLetters.find(modalText.back());
	const size_t at = found == std::string_view::npos ? 0 : found;
	modalText.back() = NameLetters[(at + (dir.y == AxisDirectionY_DOWN ? 1 : count - 1)) % count];
}

bool EditorPadOnText(const SDL_Event &event)
{
	if (!softKeyboardOpen || !IsTextModal())
		return false;
	// It clears the old text with synthetic keys, which still have to arrive as keys, but they aren't a keyboard taking over.
	if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP)
		return IsAnyOf(SDLC_EventKey(event), SDLK_BACKSPACE, SDLK_DELETE);
	if (event.type != SDL_EVENT_TEXT_INPUT)
		return false;
	const size_t limit = modal == Modal::Author ? MapMaxAuthor : MapMaxName;
	for (const char *c = event.text.text; *c != 0 && modalText.size() < limit; c++) {
		if (NameLetters.find(*c) != std::string_view::npos)
			modalText.push_back(*c);
	}
	return true;
}

std::string EditorPadPrompt(bool focused)
{
	const std::string confirm = InputName(PadAction::Confirm);
	const std::string cancel = InputName(PadAction::Cancel);
	if (IsTextModal()) {
#ifdef __SWITCH__
		const std::string keyboard = fmt::format("   {}: keyboard", PadButtonName(ControllerButton_BUTTON_Y));
#else
		const std::string keyboard;
#endif
		return fmt::format("{}: up/down letter, right add, left remove   {}: accept   {}: cancel{}", InputName(PadAction::Navigate), confirm, cancel, keyboard);
	}
	if (IsListModal())
		return fmt::format("{}: move   {}: choose   {}: cancel", InputName(PadAction::Navigate), confirm, cancel);
	if (focused)
		return fmt::format("{}: move   {}: press   {}: back to the map", InputName(PadAction::Navigate), confirm, cancel);
	const auto name = PadButtonName;
	if (tool == EditTool::Stamp) {
		return fmt::format("{}: stamp a room (hold: drag), again: keep   {}+{}: another layout   {}: drop   {} {}: stamp   {}: tools   {}: menu",
		    confirm, InputName(PadAction::Modifier), confirm, InputName(PadAction::Order),
		    name(ControllerButton_BUTTON_LEFTSHOULDER), name(ControllerButton_BUTTON_RIGHTSHOULDER), InputName(PadAction::Navigate), InputName(PadAction::Menu));
	}
	return fmt::format("{}: paint (hold: drag)   {}: other way   {}: shift   {}: undo   {} {}: size   {}: tools   {}: test   {}: menu",
	    confirm, InputName(PadAction::Order), InputName(PadAction::Modifier), name(ControllerButton_BUTTON_Y),
	    name(ControllerButton_BUTTON_LEFTSHOULDER), name(ControllerButton_BUTTON_RIGHTSHOULDER), InputName(PadAction::Navigate), name(ControllerButton_BUTTON_BACK), InputName(PadAction::Menu));
}

void EditorDrawTestBanner(const Surface &out)
{
	if (!testing)
		return;
	const std::string Text = fmt::format("Testing your map.  {}: back to the editor", InputName(PadAction::EndTest));
	const int width = GetLineWidth(Text) + 16;
	const Rectangle line { { (out.w() - width) / 2, 4 }, { width, 18 } };
	DrawHalfTransparentRectTo(out, line.position.x, line.position.y, line.size.width, line.size.height);
	DrawString(out, Text, { line.position + Displacement { 0, 2 }, { line.size.width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorGold });
}

} // namespace devilution::keeper
