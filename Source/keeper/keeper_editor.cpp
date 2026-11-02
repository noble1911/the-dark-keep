/**
 * @file keeper/keeper_editor.cpp
 *
 * See keeper_editor.hpp. The editor's only model is a KeepMap; every change reruns the tileset's
 * joining rules over the whole layout and rebuilds the level, so what you see is what plays.
 */
#include "keeper/keeper_editor.hpp"

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
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_map.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "objects.h"
#include "options.h"
#include "utils/file_util.h"
#include "utils/sdl_compat.h"
#include "utils/ui_fwd.h"

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
};
constexpr size_t NumEditTools = 11;

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
	{ "Entrance", "8", "Click to place the heroes' stairs. It clears the floor it needs. Right-click: remove." },
	{ "Vault", "9", "Click to place the vault. It clears the floor it needs. Right-click: remove." },
	{ "Object", "0", "Click floor to place one; click this tool again to change what. Right-click: remove." },
	{ "Erase", "E", "Drag to remove doors, arches, objects and stairs. Paints nothing." },
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
		if (tileset != other.tileset || cells != other.cells || entrance != other.entrance || vault != other.vault || objects.size() != other.objects.size())
			return false;
		for (size_t i = 0; i < objects.size(); i++) {
			if (objects[i].kind != other.objects[i].kind || objects[i].position != other.objects[i].position)
				return false;
		}
		return true;
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

Modal modal = Modal::None;
std::string modalText;
std::string modalNote;
std::vector<std::string> modalList;
int modalScroll = 0;

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
	ApplyLayoutToLevel(model, decorPreview);
	std::memset(dObject, 0, sizeof(dObject));
	InitObjects();
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
	if (fileName.empty())
		OpenTextModal(Modal::SaveName, model.name);
	else
		SaveToCurrentFile();
}

void ChangeTileset(MapTileset tileset)
{
	if (tileset == model.tileset)
		return;
	if (!IsMapTilesetInstalled(tileset)) {
		SetFeedback(fmt::format("The game data here has no {} tiles", MapTilesetName(tileset)), true);
		return;
	}
	PushUndo(TakeSnapshot());
	model.tileset = tileset;
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
	const bool sameTileset = loaded->tileset == model.tileset;
	model = std::move(*loaded);
	// The editor's own scratch files get overwritten behind the author's back: saving asks for a name.
	const bool scratch = name.empty() || name[0] == '_';
	fileName = scratch ? std::string() : name;
	dirty = scratch;
	undoStack.clear();
	redoStack.clear();
	SetFeedback(fmt::format("Loaded {}", name));
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
	if (dirty) {
		modal = Modal::ConfirmNew;
		modalNote.clear();
	} else {
		NewMapNow();
	}
}

void StartTest()
{
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

void PlaceObject(Point tile, bool remove)
{
	const auto found = std::find_if(model.objects.begin(), model.objects.end(), [&](const MapObject &object) { return object.position == tile; });
	if (remove) {
		if (found != model.objects.end()) {
			model.objects.erase(found);
			levelStale = dirty = true;
		}
		return;
	}
	if (found != model.objects.end())
		return;
	if (model.objects.size() >= MaxMapObjects) {
		SetFeedback(fmt::format("A map can hold {} objects", MaxMapObjects), true);
		return;
	}
	if (!MapObjectFitsTileset(objectKind, model.tileset)) {
		SetFeedback(fmt::format("{} isn't available in {}", MapObjectName(objectKind), MapTilesetName(model.tileset)), true);
		return;
	}
	if (!InDungeonBounds(tile) || !IsTileNotSolid(tile)) {
		SetFeedback("Objects go on the floor", true);
		return;
	}
	model.objects.push_back({ objectKind, tile });
	levelStale = dirty = true;
}

void FinishStroke()
{
	const bool committed = stroke.active;
	stroke.active = false;
	if (!committed)
		return;
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
		model.entrance = right ? NoMapPosition : StairsPositionAt(*cell, true);
		levelStale = dirty = true;
		break;
	case EditTool::Vault:
		model.vault = right ? NoMapPosition : StairsPositionAt(*cell, false);
		levelStale = dirty = true;
		break;
	case EditTool::Object:
		PlaceObject(cursPosition, right);
		break;
	}
}

void Undo()
{
	if (undoStack.empty())
		return;
	redoStack.push_back(TakeSnapshot());
	RestoreSnapshot(undoStack.back());
	undoStack.pop_back();
}

void Redo()
{
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
		objectKind = static_cast<MapObjectKind>((static_cast<size_t>(objectKind) + 1) % NumMapObjectKinds);
	tool = next;
	if (next == EditTool::Corridor && brushSize < 2)
		brushSize = 2;
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
		std::string label = fmt::format("{} {}", Tools[i].key, Tools[i].name);
		if (thisTool == EditTool::Object)
			label = fmt::format("{} {}", Tools[i].key, MapObjectName(objectKind));
		bool enabled = true;
		if (thisTool == EditTool::Door)
			enabled = MapTilesetHasDoors(model.tileset);
		if (thisTool == EditTool::Arch)
			enabled = MapTilesetHasArches(model.tileset);
		add(std::move(label), ActionKind::Tool, i, tool == thisTool, enabled);
	}
	x = left;
	y += ButtonHeight + 7;
	add("Undo", ActionKind::Undo, 0, false, !undoStack.empty());
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
	add("F5 Test", ActionKind::Test, 0, false, !HasBlockingProblem());
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
		modalNote = modalList.empty() ? "No maps saved yet" : "";
		break;
	case ActionKind::Tileset:
		modal = Modal::TilesetList;
		modalList.clear();
		for (size_t i = 0; i < NumMapTilesets; i++)
			modalList.emplace_back(MapTilesetName(static_cast<MapTileset>(i)));
		modalScroll = 0;
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
	case ActionKind::Test:
		StartTest();
		break;
	}
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
	if (modal == Modal::LoadList || modal == Modal::TilesetList)
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
	case Modal::ConfirmNew:
		return "Start a new map?";
	case Modal::None:
		break;
	}
	return "";
}

std::optional<size_t> ModalRowAt(Point mouse)
{
	if (modal != Modal::LoadList && modal != Modal::TilesetList)
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
		DrawString(out, modalNote.empty() ? std::string_view("Enter: accept   Esc: cancel") : std::string_view(modalNote), { text + Displacement { 0, 44 }, { width, 14 } },
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
		if (hovered == index)
			FillRect(out, at.x, at.y, width, ModalRowHeight, PAL16_RED + 12);
		const bool current = modal == Modal::TilesetList ? index == static_cast<size_t>(model.tileset) : modalList[index] == fileName;
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
		const std::string warning = fmt::format("{} exists. Enter again to replace it.", file);
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
	if (levelStale)
		RefreshLevel();
}

bool EditorOnTestKey(SDL_Keycode key)
{
	if (!testing || key != SDLK_F10)
		return false;
	EditorEndTestRun();
	return true;
}

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
	case SDLK_LEFTBRACKET:
		brushSize = std::max(brushSize - 1, 1);
		return true;
	case SDLK_RIGHTBRACKET:
		brushSize = std::min(brushSize + 1, MaxBrush);
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
	if (modal == Modal::LoadList || modal == Modal::TilesetList) {
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
		DrawRectOutline(out, StairsPositionAt(*hovered, entrance), MapStairsSize(model.tileset, entrance), entrance ? PAL16_BLUE + 4 : PAL16_YELLOW + 4, maxY);
		break;
	}
	case EditTool::Door:
	case EditTool::Arch:
	case EditTool::Erase:
		DrawCellOutline(out, *hovered, tool == EditTool::Erase ? PAL16_RED + 2 : PAL16_ORANGE + 4, maxY);
		break;
	case EditTool::Object:
		DrawCellOutline(out, *hovered, PAL16_YELLOW + 6, maxY);
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
	std::string hint(Tools[static_cast<size_t>(tool)].help);
	if (tool == EditTool::Door && model.tileset == MapTileset::Caves)
		hint = "Click floor in a straight stretch two or more wide: a fence crosses the cave there, with a gate in it. Right-click: remove.";
	if (tool == EditTool::Floor || tool == EditTool::Rock || tool == EditTool::Corridor)
		hint += fmt::format("  Size {}", brushSize);
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

void EditorDrawTestBanner(const Surface &out)
{
	if (!testing)
		return;
	constexpr std::string_view Text = "Testing your map.  F10: back to the editor";
	const int width = GetLineWidth(Text) + 16;
	const Rectangle line { { (out.w() - width) / 2, 4 }, { width, 18 } };
	DrawHalfTransparentRectTo(out, line.position.x, line.position.y, line.size.width, line.size.height);
	DrawString(out, Text, { line.position + Displacement { 0, 2 }, { line.size.width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorGold });
}

} // namespace devilution::keeper
