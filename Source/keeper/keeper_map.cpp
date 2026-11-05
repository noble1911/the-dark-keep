/**
 * @file keeper/keeper_map.cpp
 *
 * See keeper_map.hpp and docs/MAP-FORMAT.md.
 */
#include "keeper/keeper_map.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <optional>

#include <fmt/format.h>

#include "engine/assets.hpp"
#include "engine/load_file.hpp"
#include "engine/path.h"
#include "engine/random.hpp"
#include "headless_mode.hpp"
#include "keeper/keeper.hpp"
#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_editor.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_heroes.hpp"
#include "levels/drlg_l1.h"
#include "levels/drlg_l2.h"
#include "levels/drlg_l3.h"
#include "levels/drlg_l4.h"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "msg.h"
#include "multi.h"
#include "objects.h"
#include "player.h"
#include "plrmsg.h"
#include "tables/objdat.h"
#include "utils/endian_swap.hpp"
#include "utils/file_util.h"
#include "utils/log.hpp"
#include "utils/paths.h"

namespace devilution::keeper {

namespace {

// ---- what each tileset needs ---------------------------------------------------------------

/**
 * How a staircase reshapes the layout so its generator's miniset sits properly: `rows` (top to
 * bottom, '.' floor, '#' rock) replaces the painted cells before the walls are joined. These mirror
 * the `search` patterns of the minisets in levels/drlg_l*.cpp, crypt.cpp.
 */
struct StairsShape {
	int width;
	int height;
	const char *rows;
	/** The generator's spawn point (entrance) relative to the top-left, in world tiles. */
	Displacement spawn;
};

struct TilesetInfo {
	std::string_view name;
	dungeon_type levelType;
	const char *tilFile;
	StairsShape up;
	StairsShape down;
	/** Each InitLxTriggers's dPiece values, so a hostile tile grid can't overflow trigs[]. */
	std::array<uint16_t, 4> triggerPieces;
	/** Real door objects. Caves': fenced gates. */
	bool doors;
	bool arches;
};

// clang-format off
const std::array<TilesetInfo, NumMapTilesets> Tilesets = { {
	{ "Cathedral", DTYPE_CATHEDRAL, "levels\\l1data\\l1.til",
		{ 4, 4, "....####........", { 3, 4 } },
		{ 4, 3, "............", { 3, 3 } },
		{ 128, 114, 0, 0 }, true, true },
	{ "Catacombs", DTYPE_CATACOMBS, "levels\\l2data\\l2.til",
		{ 4, 4, "................", { 5, 4 } },
		{ 4, 4, "................", { 4, 6 } },
		{ 266, 558, 270, 0 }, true, false },
	{ "Caves", DTYPE_CAVES, "levels\\l3data\\l3.til",
		{ 3, 4, "######......", { 1, 3 } },
		{ 4, 3, "##..##..##..", { 3, 1 } },
		{ 170, 167, 548, 0 }, true, false },
	{ "Hell", DTYPE_HELL, "levels\\l4data\\l4.til",
		{ 4, 5, "....................", { 6, 6 } },
		{ 5, 5, ".........................", { 7, 5 } },
		{ 82, 421, 119, 369 }, false, false },
	{ "Nest", DTYPE_NEST, "nlevels\\l6data\\l6.til",
		{ 3, 4, "######......", { 1, 3 } },
		{ 4, 3, "##..##..##..", { 3, 1 } },
		{ 65, 62, 79, 0 }, false, false },
	{ "Crypt", DTYPE_CRYPT, "nlevels\\l5data\\l5.til",
		{ 4, 5, "############........", { 3, 5 } },
		{ 4, 5, "....................", { 3, 7 } },
		{ 183, 157, 125, 0 }, true, true },
} };
// clang-format on

const TilesetInfo &Info(MapTileset tileset)
{
	return Tilesets[static_cast<size_t>(tileset)];
}

const StairsShape &Shape(MapTileset tileset, bool entrance)
{
	return entrance ? Info(tileset).up : Info(tileset).down;
}

constexpr bool HasPosition(Point position)
{
	return position.x >= 0;
}

bool InRect(Point p, Point topLeft, int width, int height)
{
	return p.x >= topLeft.x && p.y >= topLeft.y && p.x < topLeft.x + width && p.y < topLeft.y + height;
}

// ---- the layout after the staircases have had their say ----------------------------------------

using CellGrid = std::array<std::array<uint8_t, DMAXY>, DMAXX>;

void StampStairs(CellGrid &grid, const KeepMap &map, bool entrance)
{
	const Point position = entrance ? map.entrance : map.vault;
	if (!HasPosition(position))
		return;
	const StairsShape &shape = Shape(map.tileset, entrance);
	for (int row = 0; row < shape.height; row++) {
		for (int column = 0; column < shape.width; column++) {
			const int x = position.x + column;
			const int y = position.y + row;
			if (x < 0 || y < 0 || x >= DMAXX || y >= DMAXY)
				continue;
			const bool floor = shape.rows[row * shape.width + column] == '.';
			grid[x][y] = static_cast<uint8_t>(floor ? MapCell::Floor : MapCell::Rock);
		}
	}
}

CellGrid EffectiveCells(const KeepMap &map)
{
	CellGrid grid = map.cells;
	StampStairs(grid, map, true);
	StampStairs(grid, map, false);
	// The joining rules index one past each cell, so the ring must stay rock whatever a file says.
	for (int i = 0; i < DMAXX; i++) {
		grid[i][0] = grid[i][DMAXY - 1] = static_cast<uint8_t>(MapCell::Rock);
		grid[0][i] = grid[DMAXX - 1][i] = static_cast<uint8_t>(MapCell::Rock);
	}
	return grid;
}

bool IsFloorCell(const CellGrid &grid, int x, int y)
{
	return grid[x][y] == static_cast<uint8_t>(MapCell::Floor);
}

/** A door or arch needs a passage through it: floor on two opposite sides, wall on the other two. */
bool IsOpeningSound(const CellGrid &grid, int x, int y)
{
	if (x < 1 || y < 1 || x >= DMAXX - 1 || y >= DMAXY - 1)
		return false;
	const bool alongX = IsFloorCell(grid, x - 1, y) && IsFloorCell(grid, x + 1, y) && !IsFloorCell(grid, x, y - 1) && !IsFloorCell(grid, x, y + 1);
	const bool alongY = IsFloorCell(grid, x, y - 1) && IsFloorCell(grid, x, y + 1) && !IsFloorCell(grid, x - 1, y) && !IsFloorCell(grid, x + 1, y);
	return alongX || alongY;
}

void SetMaskFromCells(const CellGrid &grid)
{
	DungeonMask.reset();
	for (int x = 0; x < DMAXX; x++) {
		for (int y = 0; y < DMAXY; y++) {
			if (IsFloorCell(grid, x, y))
				DungeonMask.set(x, y);
		}
	}
}

void BuildCathedralOrCrypt(const KeepMap &map, const CellGrid &grid, bool decorate)
{
	uint8_t openings[DMAXX][DMAXY] = {};
	for (int x = 0; x < DMAXX; x++) {
		for (int y = 0; y < DMAXY; y++) {
			const auto cell = static_cast<MapCell>(grid[x][y]);
			if ((cell == MapCell::Door || cell == MapCell::Arch) && IsOpeningSound(grid, x, y))
				openings[x][y] = cell == MapCell::Door ? 1 : 2;
		}
	}
	SetMaskFromCells(grid);
	KeeperL1Build(map.entrance, map.vault, openings, decorate);
}

void BuildCatacombs(const KeepMap &map, const CellGrid &grid, bool decorate)
{
	char layout[DMAXX][DMAXY];
	for (int x = 0; x < DMAXX; x++) {
		for (int y = 0; y < DMAXY; y++) {
			if (IsFloorCell(grid, x, y)) {
				layout[x][y] = '.';
				continue;
			}
			if (static_cast<MapCell>(grid[x][y]) == MapCell::Door && IsOpeningSound(grid, x, y)) {
				layout[x][y] = 'D';
				continue;
			}
			bool nearFloor = false;
			for (int dx = -1; dx <= 1; dx++) {
				for (int dy = -1; dy <= 1; dy++) {
					const int nx = x + dx;
					const int ny = y + dy;
					if (nx >= 0 && ny >= 0 && nx < DMAXX && ny < DMAXY && IsFloorCell(grid, nx, ny))
						nearFloor = true;
				}
			}
			layout[x][y] = nearFloor ? '#' : ' ';
		}
	}
	KeeperL2Build(layout, map.entrance, map.vault, decorate);
}

void BuildCavesOrNest(const KeepMap &map, const CellGrid &grid, bool decorate)
{
	// A cave's walls aren't cells, so a door isn't in one: the cell is floor, and a fence crosses the cave there.
	CellGrid floorPlan = grid;
	uint8_t gates[DMAXX][DMAXY] = {};
	if (map.tileset == MapTileset::Caves) {
		for (int x = 0; x < DMAXX; x++) {
			for (int y = 0; y < DMAXY; y++) {
				if (static_cast<MapCell>(grid[x][y]) != MapCell::Door || x + 1 >= DMAXX || y + 1 >= DMAXY)
					continue;
				// A gate needs a whole floor tile, which is cut on these four corners. In solid wall the cell stays wall.
				const auto open = [&grid](int cx, int cy) { return IsAnyOf(static_cast<MapCell>(grid[cx][cy]), MapCell::Floor, MapCell::Door); };
				if (open(x + 1, y) && open(x, y + 1) && open(x + 1, y + 1)) {
					floorPlan[x][y] = static_cast<uint8_t>(MapCell::Floor);
					gates[x][y] = 1;
				}
			}
		}
	}
	SetMaskFromCells(floorPlan);
	KeeperL3Build(map.entrance, map.vault, gates, decorate);
}

void BuildDungeonGrid(const KeepMap &map, bool decorate)
{
	const CellGrid grid = EffectiveCells(map);
	SetRndSeed(map.decorSeed);
	DRLG_InitTrans();
	themeCount = 0;
	switch (map.tileset) {
	case MapTileset::Cathedral:
	case MapTileset::Crypt:
		BuildCathedralOrCrypt(map, grid, decorate);
		break;
	case MapTileset::Catacombs:
		BuildCatacombs(map, grid, decorate);
		break;
	case MapTileset::Caves:
	case MapTileset::Nest:
		BuildCavesOrNest(map, grid, decorate);
		break;
	case MapTileset::Hell:
		SetMaskFromCells(grid);
		KeeperL4Build(map.entrance, map.vault, decorate);
		break;
	}
	// TransVal is an int8_t and the flood counts every separate room: a map cut into hundreds of
	// pockets would wrap it, and a negative value indexes off the front of TransList.
	for (auto &column : dTransVal) {
		for (int8_t &value : column) {
			if (value < 0)
				value = 127;
		}
	}
}

void RunPass3(MapTileset tileset)
{
	std::memset(dSpecial, 0, sizeof(dSpecial));
	switch (tileset) {
	case MapTileset::Cathedral:
	case MapTileset::Crypt:
		KeeperL1Pass3();
		break;
	case MapTileset::Catacombs:
		KeeperL2Pass3();
		break;
	case MapTileset::Caves:
	case MapTileset::Nest:
		KeeperL3Pass3();
		break;
	case MapTileset::Hell:
		KeeperL4Pass3();
		break;
	}
}

bool HasDoorObject(Point cell)
{
	const Point first = cell.megaToWorld();
	for (int dx = 0; dx < 2; dx++) {
		for (int dy = 0; dy < 2; dy++) {
			const Object *object = FindObjectAtPosition(first + Displacement { dx, dy });
			if (object != nullptr && object->isDoor())
				return true;
		}
	}
	return false;
}

// ---- objects and props -----------------------------------------------------------------------

constexpr uint8_t TilesetBit(MapTileset tileset)
{
	return static_cast<uint8_t>(1U << static_cast<unsigned>(tileset));
}
constexpr uint8_t InCathedral = TilesetBit(MapTileset::Cathedral);
constexpr uint8_t InCatacombs = TilesetBit(MapTileset::Catacombs);
constexpr uint8_t InCaves = TilesetBit(MapTileset::Caves);
constexpr uint8_t InHell = TilesetBit(MapTileset::Hell);
constexpr uint8_t InNest = TilesetBit(MapTileset::Nest);
constexpr uint8_t InCrypt = TilesetBit(MapTileset::Crypt);
constexpr uint8_t InAll = InCathedral | InCatacombs | InCaves | InHell | InNest | InCrypt;

constexpr size_t MaxPropStyles = 8;

/**
 * One kind of object a map can hold. The research behind the choices is in docs/MAP-FORMAT.md: these are
 * the vanilla and Hellfire decorations that can be made to do nothing, and the tilesets whose art they suit.
 */
struct PropSpec {
	std::string_view name;
	MapObjectTraits traits;
	/** The object for each style (for a wall kind, facing +x). */
	std::array<_object_id, MaxPropStyles> types;
	/** A wall kind facing +y. */
	std::array<_object_id, MaxPropStyles> typesY;
	/** The frame each style shows; 0 leaves the object's own. */
	std::array<uint8_t, MaxPropStyles> frames;
	uint8_t tilesets;
	/** On a wall facing +x, the art is drawn from the floor tile in front of the wall, not the wall's own. */
	bool standsInFrontFacingX = false;
};

constexpr MapObjectTraits Solid { MapObjectPlacement::Floor, true, false, false, 1 };
constexpr MapObjectTraits SolidLight { MapObjectPlacement::Floor, true, true, false, 1 };
constexpr MapObjectTraits Styles(uint8_t styles)
{
	return { MapObjectPlacement::Floor, true, false, false, styles };
}

// clang-format off
const std::array<PropSpec, NumMapObjectKinds> Props = { {
	{ "Barrel", { MapObjectPlacement::Floor, true, false, true, 1 }, { OBJ_BARREL }, {}, {}, InAll },
	{ "Chest", { MapObjectPlacement::Floor, true, false, true, 1 }, { OBJ_CHEST1 }, {}, {}, InAll },
	{ "Brazier", SolidLight, { OBJ_L1LIGHT }, {}, {}, InCathedral },
	{ "Candles", SolidLight, { OBJ_CANDLE2 }, {}, {}, InAll },
	{ "Skull fire", SolidLight, { OBJ_SKFIRE }, {}, {}, InAll },
	// The Nest's walls are never straight.
	{ "Wall torch", { MapObjectPlacement::OnWall, false, true, false, 1 }, { OBJ_TORCHL }, { OBJ_TORCHR }, {}, InAll & ~InNest },
	{ "Banner", Styles(3), { OBJ_BANNERL, OBJ_BANNERM, OBJ_BANNERR }, {}, {}, InCathedral | InCatacombs | InHell | InCrypt },
	{ "Skull pile", Solid, { OBJ_SKPILE }, {}, {}, InAll },
	{ "Crucified skeleton", Styles(3), { OBJ_CRUX1, OBJ_CRUX2, OBJ_CRUX3 }, {}, {}, InAll },
	{ "Book stand", Styles(2), { OBJ_BOOKSTAND, OBJ_BOOKSTAND }, {}, { 1, 3 }, InCathedral | InCatacombs | InCrypt },
	{ "Bookcase", { MapObjectPlacement::AgainstWall, true, false, false, 1 }, { OBJ_BOOKCASEL }, { OBJ_BOOKCASER }, {}, InCathedral | InCatacombs | InCrypt },
	{ "Weapon rack", Styles(2), { OBJ_WEAPONRACKN, OBJ_WEAPONRACKN }, {}, { 1, 2 }, InCatacombs | InCaves | InHell | InCrypt },
	{ "Armour stand", Styles(2), { OBJ_ARMORSTANDN, OBJ_ARMORSTANDN }, {}, { 1, 2 }, InCatacombs | InCaves | InHell | InCrypt },
	{ "Headless body", Styles(8), { OBJ_DECAP, OBJ_DECAP, OBJ_DECAP, OBJ_DECAP, OBJ_DECAP, OBJ_DECAP, OBJ_DECAP, OBJ_DECAP }, {}, { 1, 2, 3, 4, 5, 6, 7, 8 }, InAll },
	{ "Tortured body", Styles(7), { OBJ_TNUDEM1, OBJ_TNUDEM2, OBJ_TNUDEM3, OBJ_TNUDEM4, OBJ_TNUDEW1, OBJ_TNUDEW2, OBJ_TNUDEW3 }, {}, {}, InAll },
	{ "Impaled body", { MapObjectPlacement::OnWall, false, false, false, 2 }, { OBJ_TORTURE1, OBJ_TORTURE2 }, { OBJ_TORTURE3, OBJ_TORTURE4 }, {}, InAll & ~InNest, true },
	{ "Blood fountain", Solid, { OBJ_BLOODFTN }, {}, {}, InAll },
	{ "Crypt candles", SolidLight, { OBJ_L5CANDLE }, {}, {}, InCrypt },
	{ "Angel statue", Solid, { OBJ_ANGEL }, {}, {}, InCathedral | InCatacombs | InCrypt },
} };
// clang-format on

const PropSpec &Spec(MapObjectKind kind)
{
	return Props[static_cast<size_t>(kind)];
}

_object_id PropObjectType(const MapObject &object)
{
	const PropSpec &spec = Spec(object.kind);
	const size_t style = std::min<size_t>(object.style(), spec.traits.styles - 1U);
	return spec.traits.placement != MapObjectPlacement::Floor && object.facesY() ? spec.typesY[style] : spec.types[style];
}

/** Where the object goes in the level: the stored tile, or for some wall art the floor in front. */
Point PropObjectTile(const MapObject &object)
{
	const PropSpec &spec = Spec(object.kind);
	if (spec.traits.placement == MapObjectPlacement::OnWall && spec.standsInFrontFacingX && !object.facesY())
		return object.position + Displacement { 1, 0 };
	return object.position;
}

/** Every object type a kind can become. */
std::vector<_object_id> PropObjectTypes(MapObjectKind kind)
{
	const PropSpec &spec = Spec(kind);
	std::vector<_object_id> types;
	for (size_t style = 0; style < spec.traits.styles; style++) {
		types.push_back(spec.types[style]);
		if (spec.traits.placement != MapObjectPlacement::Floor)
			types.push_back(spec.typesY[style]);
	}
	return types;
}

/** The game data has the kind's art. Hellfire's, or the full game's, may be missing. */
bool IsPropArtInstalled(MapObjectKind kind)
{
	static std::array<std::optional<bool>, NumMapObjectKinds> known;
	std::optional<bool> &installed = known[static_cast<size_t>(kind)];
	if (!installed) {
		installed = true;
		for (const _object_id type : PropObjectTypes(kind)) {
			const std::string &file = ObjMasterLoadList[AllObjects[type].ofindex];
			if (!FindAsset(fmt::format("objects\\{}.cel", file)).ok() && !FindAsset(fmt::format("objects\\{}.clx", file)).ok())
				installed = false;
		}
	}
	return *installed;
}

/** Which kinds InitObjectGFX loaded the art for, on the keep level as it stands. */
std::array<bool, NumMapObjectKinds> propArtLoaded {};

std::vector<std::pair<MapObject, std::string>> leftOutObjects;

constexpr Displacement Facing(const MapObject &object)
{
	return object.facesY() ? Displacement { 0, 1 } : Displacement { 1, 0 };
}

/** Along the wall: across the facing. */
constexpr Displacement Along(const MapObject &object)
{
	return object.facesY() ? Displacement { 1, 0 } : Displacement { 0, 1 };
}

bool IsDoorTile(Point tile)
{
	const Object *object = FindObjectAtPosition(tile);
	return object != nullptr && object->isDoor();
}

/** Floor a prop can stand on or face: walkable and nothing standing on it. */
bool IsBareFloor(Point tile)
{
	return InDungeonBounds(tile) && IsTileNotSolid(tile) && dObject[tile.x][tile.y] == 0;
}

/** A straight wall: the tile and its neighbours along it are solid, and none of them is a door. */
bool IsStraightWall(Point tile, Displacement along)
{
	for (const Point at : { tile, tile + along, tile - along }) {
		if (!InDungeonBounds(at) || !IsTileSolid(at) || IsDoorTile(at))
			return false;
	}
	return true;
}

bool IsNearStairs(const KeepMap &map, Point tile)
{
	for (const bool entrance : { true, false }) {
		const Point stairs = entrance ? map.entrance : map.vault;
		if (!HasPosition(stairs))
			continue;
		const Size size = MapStairsSize(map.tileset, entrance);
		const Point first = stairs.megaToWorld() - Displacement { 1, 1 };
		if (InRect(tile, first, 2 * size.width + 2, 2 * size.height + 2))
			return true;
	}
	return false;
}

/**
 * Heroes could still get everywhere they could before, if `tile` were blocked: every tile round it that
 * they can stand on still reaches the others without it, by the same steps BuildField takes. Then any
 * way through the tile has a way round within one step of it.
 */
bool KeepsConnectivity(Point tile)
{
	constexpr std::array<Displacement, 8> Ring { { { -1, -1 }, { 0, -1 }, { 1, -1 }, { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 } } };
	std::array<bool, 8> open {};
	int openCount = 0;
	for (size_t i = 0; i < Ring.size(); i++) {
		open[i] = IsPassable(tile + Ring[i]);
		openCount += open[i] ? 1 : 0;
	}
	if (openCount == 0)
		return true;
	std::array<bool, 8> reached {};
	std::array<size_t, 8> queue {};
	size_t head = 0;
	size_t tail = 0;
	for (size_t i = 0; i < Ring.size() && tail == 0; i++) {
		if (open[i]) {
			reached[i] = true;
			queue[tail++] = i;
		}
	}
	int reachedCount = 1;
	while (head < tail) {
		const size_t from = queue[head++];
		const Point a = tile + Ring[from];
		for (size_t to = 0; to < Ring.size(); to++) {
			if (!open[to] || reached[to])
				continue;
			const Point b = tile + Ring[to];
			const Displacement step = b - a;
			if (std::abs(step.deltaX) > 1 || std::abs(step.deltaY) > 1 || !CanStepDiagonal(a, step))
				continue;
			reached[to] = true;
			queue[tail++] = to;
			reachedCount++;
		}
	}
	return reachedCount == openCount;
}

/** Tiles heroes could walk in a straight line from `tile`, not counting it, up to two. */
int OpenRun(Point tile, Displacement step)
{
	int run = 0;
	for (Point at = tile + step; run < 2 && IsPassable(at) && !IsDoorTile(at); at += step)
		run++;
	return run;
}

/** Blocking `tile` leaves a way two tiles wide beside it, across both axes. */
bool LeavesWideWay(Point tile)
{
	for (const Displacement axis : { Displacement { 1, 0 }, Displacement { 0, 1 } }) {
		if (std::max(OpenRun(tile, axis), OpenRun(tile, -axis)) < 2)
			return false;
	}
	return true;
}

/** A prop goes in as scenery: nobody can use it, break it or shoot through it, so it never changes a fight. */
void MakeInert(Object &object, const MapObject &placed)
{
	const PropSpec &spec = Spec(placed.kind);
	object.selectionRegion = SelectionRegion::None;
	object._oBreak = 0;
	object._oTrapFlag = false;
	object._oMissFlag = true;
	object._oSolidFlag = spec.traits.solid;
	const uint8_t frame = spec.frames[std::min<size_t>(placed.style(), MaxPropStyles - 1)];
	if (frame != 0) {
		object._oAnimFlag = false;
		object._oAnimFrame = frame;
	}
}

// ---- the file --------------------------------------------------------------------------------

constexpr std::array<char, 4> FileMagic { 'D', 'K', 'M', 'P' };
/** The newest version this reader knows. A map that needs nothing newer is still written as version 1. */
constexpr uint16_t FileVersion = 2;
constexpr uint16_t FlagPlayable = 1;
constexpr size_t HeaderSize = 16;
/** Well over any v1 file; stops a bogus size asking for gigabytes. */
constexpr size_t MaxMapFileSize = 60 * 1024;
/** What a tileset needs besides its .til, by the same stem. */
constexpr std::string_view LevelDataExtensions[] = { ".til", ".cel", ".sol" };

uint32_t Crc32(const std::byte *data, size_t size)
{
	uint32_t crc = 0xFFFFFFFFU;
	for (size_t i = 0; i < size; i++) {
		crc ^= static_cast<uint8_t>(data[i]);
		for (int bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
	}
	return ~crc;
}

class ByteWriter {
public:
	explicit ByteWriter(std::vector<std::byte> &out)
	    : out_(out)
	{
	}
	void u8(uint8_t value) { out_.push_back(static_cast<std::byte>(value)); }
	void u16(uint16_t value)
	{
		u8(static_cast<uint8_t>(value));
		u8(static_cast<uint8_t>(value >> 8));
	}
	void u32(uint32_t value)
	{
		u16(static_cast<uint16_t>(value));
		u16(static_cast<uint16_t>(value >> 16));
	}
	void text(std::string_view value)
	{
		u8(static_cast<uint8_t>(value.size()));
		for (const char c : value)
			u8(static_cast<uint8_t>(c));
	}

private:
	std::vector<std::byte> &out_;
};

/** Every read is bounds-checked; `ok` goes false for good on the first one that runs off the end. */
class ByteReader {
public:
	ByteReader(const std::byte *data, size_t size)
	    : data_(data)
	    , size_(size)
	{
	}
	uint8_t u8()
	{
		if (pos_ >= size_) {
			ok = false;
			return 0;
		}
		return static_cast<uint8_t>(data_[pos_++]);
	}
	uint16_t u16()
	{
		const uint8_t low = u8();
		const uint8_t high = u8();
		return static_cast<uint16_t>(low | (high << 8));
	}
	uint32_t u32()
	{
		const uint16_t low = u16();
		const uint16_t high = u16();
		return low | (static_cast<uint32_t>(high) << 16);
	}
	std::string text(size_t maxLength)
	{
		const size_t length = u8();
		std::string value;
		for (size_t i = 0; i < length; i++) {
			const uint8_t c = u8();
			if (i < maxLength)
				value.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?'); // the fonts and shared files
		}
		return value;
	}
	[[nodiscard]] bool atEnd() const { return pos_ == size_; }
	bool ok = true;

private:
	const std::byte *data_;
	size_t size_;
	size_t pos_ = 0;
};

void WriteMapPosition(ByteWriter &out, Point position)
{
	out.u8(HasPosition(position) ? static_cast<uint8_t>(position.x) : 255);
	out.u8(HasPosition(position) ? static_cast<uint8_t>(position.y) : 255);
}

Point ReadMapPosition(ByteReader &in)
{
	const uint8_t x = in.u8();
	const uint8_t y = in.u8();
	return x == 255 || y == 255 ? NoMapPosition : Point { x, y };
}

// ---- the map in play ---------------------------------------------------------------------------

const KeepMap *liveMap = nullptr;
const bool *liveDecorate = nullptr;

/** The map file behind the current level, if any. */
std::optional<KeepMap> fileMap;
std::string fileMapPath;
/** What PrepareMapLevel couldn't load, for NoteMapLevelLoaded. */
std::string mapLoadError;
bool levelFromMap = false;

bool IsKeepLevel()
{
	return currlevel == KeepLevel && !setlevel;
}

bool IsSetupEditor()
{
	return CurrentSetup().mode == GameMode::MapEditor && !gbIsMultiplayer;
}

constexpr std::string_view CampaignMapFolder = "keeper\\campaign\\";

/** ReloadKeepLevel asked for the keep to be built anew. */
bool wantFreshLevel = false;

void ForgetFileMap()
{
	fileMap.reset();
	fileMapPath.clear();
}

bool IsCampaignMapAsset(std::string_view path)
{
	return path.rfind(CampaignMapFolder, 0) == 0;
}

tl::expected<void, std::string> LoadFileMap(const std::string &path)
{
	if (fileMap && fileMapPath == path)
		return {};
	ForgetFileMap();
	tl::expected<std::vector<std::byte>, std::string> bytes = ReadMapBytes(path);
	if (!bytes.has_value())
		return tl::make_unexpected(bytes.error());
	tl::expected<KeepMap, std::string> map = ParseMap(bytes->data(), bytes->size());
	if (!map.has_value())
		return tl::make_unexpected(map.error());
	if (!map->playable)
		return tl::make_unexpected("it is unfinished: fix it in the editor");
	if (!HasPosition(map->entrance) || !HasPosition(map->vault))
		return tl::make_unexpected("it has no entrance or no vault");
	if (!IsMapTilesetInstalled(map->tileset))
		return tl::make_unexpected(fmt::format("the game data here has no {} tiles", Info(map->tileset).name));
	fileMap = std::move(*map);
	fileMapPath = path;
	return {};
}

/** The tile grid came from a file: check it against the tileset actually loaded. */
tl::expected<void, std::string> CheckTilesFitLoadedTileset(const KeepMap &map)
{
	size_t megaTileCount = 0;
	tl::expected<std::unique_ptr<MegaTile[]>, std::string> til = LoadFileInMemWithStatus<MegaTile>(Info(map.tileset).tilFile, &megaTileCount);
	if (!til.has_value())
		return tl::make_unexpected(til.error());
	for (const auto &column : map.tiles) {
		for (const uint8_t tile : column) {
			if (tile == 0 || tile > megaTileCount)
				return tl::make_unexpected(fmt::format("tile {} isn't in the {} tileset", tile, Info(map.tileset).name));
		}
	}
	return {};
}

/** dPiece is built: too many staircases would overflow trigs[] in InitLxTriggers. */
bool TriggerCountIsSafe(const KeepMap &map)
{
	int count = 0;
	for (const auto &column : dPiece) {
		for (const uint16_t piece : column) {
			for (const uint16_t trigger : Info(map.tileset).triggerPieces) {
				if (trigger != 0 && piece == trigger)
					count++;
			}
		}
	}
	return count <= MAXTRIGGERS;
}

// ---- reachability ------------------------------------------------------------------------------

/** The heroes' staircase tile: the trigger the game would read as the vault, via its dPiece value. */
std::optional<Point> FindStairsTrigger(MapTileset tileset, Point stairs, const StairsShape &shape)
{
	// Which of triggerPieces marks "down" differs per tileset, but within the stamp there is only
	// one trigger tile, so any of them will do.
	const Point first { 16 + 2 * stairs.x, 16 + 2 * stairs.y };
	for (int x = 0; x < 2 * shape.width; x++) {
		for (int y = 0; y < 2 * shape.height; y++) {
			const Point tile = first + Displacement { x, y };
			if (!InDungeonBounds(tile))
				continue;
			for (const uint16_t trigger : Info(tileset).triggerPieces) {
				if (trigger != 0 && dPiece[tile.x][tile.y] == trigger)
					return tile;
			}
		}
	}
	return std::nullopt;
}

} // namespace

// ---- names ---------------------------------------------------------------------------------------

std::string_view MapTilesetName(MapTileset tileset)
{
	return Info(tileset).name;
}

std::string_view MapObjectName(MapObjectKind kind)
{
	return Spec(kind).name;
}

MapObjectTraits GetMapObjectTraits(MapObjectKind kind)
{
	return Spec(kind).traits;
}

bool MapTilesetHasDoors(MapTileset tileset)
{
	return Info(tileset).doors;
}

bool MapTilesetHasArches(MapTileset tileset)
{
	return Info(tileset).arches;
}

bool MapObjectFitsTileset(MapObjectKind kind, MapTileset tileset)
{
	return (Spec(kind).tilesets & TilesetBit(tileset)) != 0;
}

std::vector<MapObjectKind> MapObjectPalette(MapTileset tileset)
{
	std::vector<MapObjectKind> kinds;
	for (size_t i = 0; i < NumMapObjectKinds; i++) {
		const auto kind = static_cast<MapObjectKind>(i);
		if (MapObjectFitsTileset(kind, tileset) && (IsLegacyMapObject(kind) || IsPropArtInstalled(kind)))
			kinds.push_back(kind);
	}
	return kinds;
}

Size MapStairsSize(MapTileset tileset, bool entrance)
{
	const StairsShape &shape = Shape(tileset, entrance);
	return { shape.width, shape.height };
}

bool MapStairsFit(MapTileset tileset, bool entrance, Point position)
{
	const Size size = MapStairsSize(tileset, entrance);
	return IsPaintableCell(position.x, position.y) && IsPaintableCell(position.x + size.width - 1, position.y + size.height - 1);
}

// ---- files -----------------------------------------------------------------------------------------

std::vector<std::byte> SerializeMap(const KeepMap &map)
{
	std::vector<std::byte> bytes(HeaderSize);
	ByteWriter out(bytes);
	out.u8(static_cast<uint8_t>(map.tileset));
	out.u8(DMAXX);
	out.u8(DMAXY);
	out.u8(0);
	WriteMapPosition(out, map.entrance);
	WriteMapPosition(out, map.vault);
	out.u32(map.decorSeed);
	out.text(std::string_view(map.name).substr(0, MapMaxName));
	out.text(std::string_view(map.author).substr(0, MapMaxAuthor));
	// Version 1 whenever it can hold the map, so a map without props still opens in an older game.
	const bool needsProps = map.objects.size() > MaxMapObjectsV1 || std::any_of(map.objects.begin(), map.objects.end(), [](const MapObject &object) {
		return !IsLegacyMapObject(object.kind) || object.variant != 0;
	});
	const uint16_t version = needsProps ? 2 : 1;
	const size_t objectCount = std::min(map.objects.size(), needsProps ? MaxMapObjects : MaxMapObjectsV1);
	out.u16(static_cast<uint16_t>(objectCount));
	for (size_t i = 0; i < objectCount; i++) {
		out.u8(static_cast<uint8_t>(map.objects[i].kind));
		out.u8(static_cast<uint8_t>(map.objects[i].position.x));
		out.u8(static_cast<uint8_t>(map.objects[i].position.y));
		if (version >= 2)
			out.u8(map.objects[i].variant);
	}
	for (int y = 0; y < DMAXY; y++) {
		for (int x = 0; x < DMAXX; x++)
			out.u8(map.cells[x][y]);
	}
	for (int y = 0; y < DMAXY; y++) {
		for (int x = 0; x < DMAXX; x++)
			out.u8(map.tiles[x][y]);
	}
	for (int y = 0; y < 2 * DMAXY; y++) {
		for (int x = 0; x < 2 * DMAXX; x++)
			out.u8(static_cast<uint8_t>(map.trans[x][y]));
	}

	const size_t payloadSize = bytes.size() - HeaderSize;
	const uint32_t crc = Crc32(bytes.data() + HeaderSize, payloadSize);
	std::vector<std::byte> header;
	ByteWriter headerOut(header);
	for (const char c : FileMagic)
		headerOut.u8(static_cast<uint8_t>(c));
	headerOut.u16(version);
	headerOut.u16(map.playable ? FlagPlayable : 0);
	headerOut.u32(static_cast<uint32_t>(payloadSize));
	headerOut.u32(crc);
	std::copy(header.begin(), header.end(), bytes.begin());
	return bytes;
}

tl::expected<KeepMap, std::string> ParseMap(const std::byte *data, size_t size)
{
	if (size < HeaderSize)
		return tl::make_unexpected("the file is too short");
	if (size > MaxMapFileSize)
		return tl::make_unexpected("the file is too big");
	ByteReader header(data, HeaderSize);
	for (const char c : FileMagic) {
		if (header.u8() != static_cast<uint8_t>(c))
			return tl::make_unexpected("it isn't a Dark Keep map");
	}
	const uint16_t version = header.u16();
	if (version > FileVersion)
		return tl::make_unexpected("it was made by a newer version of the game");
	if (version == 0)
		return tl::make_unexpected("its version is invalid");
	const uint16_t flags = header.u16();
	const uint32_t payloadSize = header.u32();
	const uint32_t crc = header.u32();
	if (payloadSize != size - HeaderSize)
		return tl::make_unexpected("the file is truncated");
	if (Crc32(data + HeaderSize, payloadSize) != crc)
		return tl::make_unexpected("the file is corrupt");

	ByteReader in(data + HeaderSize, payloadSize);
	KeepMap map;
	map.playable = (flags & FlagPlayable) != 0;
	const uint8_t tileset = in.u8();
	if (tileset >= NumMapTilesets)
		return tl::make_unexpected("its tileset is unknown");
	map.tileset = static_cast<MapTileset>(tileset);
	if (in.u8() != DMAXX || in.u8() != DMAXY)
		return tl::make_unexpected("its size isn't supported");
	in.u8();
	map.entrance = ReadMapPosition(in);
	map.vault = ReadMapPosition(in);
	map.decorSeed = in.u32();
	map.name = in.text(MapMaxName);
	map.author = in.text(MapMaxAuthor);
	const size_t objectCount = in.u16();
	if (objectCount > (version >= 2 ? MaxMapObjects : MaxMapObjectsV1))
		return tl::make_unexpected("it has too many objects");
	for (size_t i = 0; i < objectCount; i++) {
		MapObject object;
		const uint8_t kind = in.u8();
		const uint8_t x = in.u8();
		const uint8_t y = in.u8();
		const uint8_t variant = version >= 2 ? in.u8() : 0;
		if (kind >= (version >= 2 ? NumMapObjectKinds : NumLegacyMapObjectKinds))
			return tl::make_unexpected("it has an unknown object");
		object.kind = static_cast<MapObjectKind>(kind);
		object.variant = variant;
		const MapObjectTraits traits = GetMapObjectTraits(object.kind);
		const bool facingAllowed = traits.placement != MapObjectPlacement::Floor;
		if (object.style() >= traits.styles || (object.facesY() && !facingAllowed))
			return tl::make_unexpected("it has an unknown object");
		object.position = { x, y };
		if (x < 16 || y < 16 || x >= 16 + 2 * DMAXX || y >= 16 + 2 * DMAXY)
			return tl::make_unexpected("it has an object off the map");
		map.objects.push_back(object);
	}
	for (int y = 0; y < DMAXY; y++) {
		for (int x = 0; x < DMAXX; x++) {
			const uint8_t cell = in.u8();
			if (cell > static_cast<uint8_t>(MapCell::Arch))
				return tl::make_unexpected("its layout is damaged");
			map.cells[x][y] = cell;
		}
	}
	for (int y = 0; y < DMAXY; y++) {
		for (int x = 0; x < DMAXX; x++)
			map.tiles[x][y] = in.u8();
	}
	for (int y = 0; y < 2 * DMAXY; y++) {
		for (int x = 0; x < 2 * DMAXX; x++) {
			const uint8_t value = in.u8();
			if (value > 127)
				return tl::make_unexpected("its transparency data is damaged");
			map.trans[x][y] = static_cast<int8_t>(value);
		}
	}
	if (!in.ok || !in.atEnd())
		return tl::make_unexpected("the file is truncated");
	if (HasPosition(map.entrance) && !MapStairsFit(map.tileset, true, map.entrance))
		return tl::make_unexpected("its entrance is off the map");
	if (HasPosition(map.vault) && !MapStairsFit(map.tileset, false, map.vault))
		return tl::make_unexpected("its vault is off the map");
	return map;
}

std::string MapFileName(std::string_view name)
{
	std::string file;
	for (const char c : name) {
		const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
		if (safe)
			file.push_back(c);
	}
	// No trailing spaces (Windows drops them) and never an empty name. A leading underscore marks the
	// editor's scratch files, which the map lists hide.
	while (!file.empty() && file.back() == ' ')
		file.pop_back();
	file.erase(0, file.find_first_not_of("_ "));
	if (file.empty())
		file = "Untitled";
	// Windows opens these as devices, whatever the extension.
	std::string upper = file;
	std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
	const bool numberedDevice = upper.size() == 4 && (upper.rfind("COM", 0) == 0 || upper.rfind("LPT", 0) == 0) && upper[3] >= '1' && upper[3] <= '9';
	if (numberedDevice || upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL")
		file += "_map";
	return file + ".dkmap";
}

std::string CampaignMapAsset(int level)
{
	return fmt::format("{}level_{:02}.dkmap", CampaignMapFolder, level);
}

tl::expected<std::vector<std::byte>, std::string> ReadMapBytes(const std::string &path)
{
	if (IsCampaignMapAsset(path)) {
		tl::expected<AssetData, std::string> asset = LoadAsset(path);
		if (!asset.has_value())
			return tl::make_unexpected(asset.error());
		if (asset->size > MaxMapFileSize)
			return tl::make_unexpected("the file is too big");
		const auto *first = reinterpret_cast<const std::byte *>(asset->data.get());
		return std::vector<std::byte>(first, first + asset->size);
	}
	std::string found;
	for (const std::string &candidate : { path, MapsFolder() + path }) {
		if (FileExists(candidate)) {
			found = candidate;
			break;
		}
	}
	if (found.empty())
		return tl::make_unexpected("the file wasn't found");
	std::uintmax_t size = 0;
	if (!GetFileSize(found.c_str(), &size))
		return tl::make_unexpected("the file couldn't be read");
	if (size > MaxMapFileSize)
		return tl::make_unexpected("the file is too big");
	FILE *file = OpenFile(found.c_str(), "rb");
	if (file == nullptr)
		return tl::make_unexpected("the file couldn't be opened");
	std::vector<std::byte> bytes(static_cast<size_t>(size));
	const size_t read = bytes.empty() ? 0 : std::fread(bytes.data(), 1, bytes.size(), file);
	std::fclose(file);
	if (read != bytes.size())
		return tl::make_unexpected("the file couldn't be read");
	return bytes;
}

tl::expected<void, std::string> WriteMapBytes(const std::string &path, const std::vector<std::byte> &bytes)
{
	// Via a temporary file, so a failed save can't destroy the old map.
	const std::string temporary = path + ".tmp";
	FILE *file = OpenFile(temporary.c_str(), "wb");
	if (file == nullptr)
		return tl::make_unexpected("the file couldn't be created");
	const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
	const bool closed = std::fclose(file) == 0;
	if (written != bytes.size() || !closed) {
		RemoveFile(temporary.c_str());
		return tl::make_unexpected("the disk may be full");
	}
	// RenameFile can't replace an existing file on Windows, and the copy doesn't report failure
	// (a locked file, say), so read it back. On failure the new bytes stay in the temporary file.
	CopyFileOverwrite(temporary.c_str(), path.c_str());
	const tl::expected<std::vector<std::byte>, std::string> saved = ReadMapBytes(path);
	if (!saved.has_value() || *saved != bytes)
		return tl::make_unexpected("the file couldn't be replaced; is it open somewhere else?");
	RemoveFile(temporary.c_str());
	return {};
}

// ---- building a level --------------------------------------------------------------------------------

void ApplyLayoutToLevel(const KeepMap &map, bool decorate)
{
	BuildDungeonGrid(map, decorate);
	RunPass3(map.tileset);
}

void BakeMap(KeepMap &map)
{
	BuildDungeonGrid(map, /*decorate=*/true);
	for (int x = 0; x < DMAXX; x++) {
		for (int y = 0; y < DMAXY; y++)
			map.tiles[x][y] = dungeon[x][y];
	}
	for (int x = 0; x < 2 * DMAXX; x++) {
		for (int y = 0; y < 2 * DMAXY; y++)
			map.trans[x][y] = dTransVal[16 + x][16 + y];
	}
}

bool BakePlayableMap(KeepMap &map)
{
	const auto buildAndJudge = [&map](bool decorate) {
		ApplyLayoutToLevel(map, decorate);
		std::memset(dObject, 0, sizeof(dObject));
		InitObjects();
		const std::vector<MapProblem> problems = ValidateMap(map);
		return std::none_of(problems.begin(), problems.end(), [](const MapProblem &problem) { return problem.blocking; });
	};
	const bool plainIsPlayable = buildAndJudge(/*decorate=*/false);
	map.playable = false;
	for (int attempt = 0; attempt < 16 && !map.playable; attempt++) {
		if (attempt > 0)
			map.decorSeed++;
		BakeMap(map);
		map.playable = buildAndJudge(/*decorate=*/true) && plainIsPlayable;
		if (!plainIsPlayable)
			break; // the decoration isn't what's wrong, so rerolling it won't help
	}
	return map.playable;
}

bool IsMapTilesetLoaded(MapTileset tileset)
{
	return leveltype == Info(tileset).levelType;
}

bool IsMapTilesetInstalled(MapTileset tileset)
{
	const std::string_view til = Info(tileset).tilFile;
	const std::string_view stem = til.substr(0, til.size() - std::string_view(".til").size());
	return std::all_of(std::begin(LevelDataExtensions), std::end(LevelDataExtensions), [&](std::string_view extension) {
		return FindAsset(fmt::format("{}{}", stem, extension)).ok();
	});
}

bool CanPlayMap(const std::string &path)
{
	const tl::expected<std::vector<std::byte>, std::string> bytes = ReadMapBytes(path);
	if (!bytes.has_value())
		return false;
	return CheckMapBytes(bytes->data(), bytes->size()).has_value();
}

tl::expected<void, std::string> CheckMapBytes(const std::byte *data, size_t size)
{
	const tl::expected<KeepMap, std::string> map = ParseMap(data, size);
	if (!map.has_value())
		return tl::make_unexpected(map.error());
	if (!map->playable)
		return tl::make_unexpected("it is unfinished");
	if (!HasPosition(map->entrance) || !HasPosition(map->vault))
		return tl::make_unexpected("it has no entrance or no vault");
	if (!IsMapTilesetInstalled(map->tileset))
		return tl::make_unexpected(fmt::format("the game data here has no {} tiles", Info(map->tileset).name));
	return CheckTilesFitLoadedTileset(*map);
}

tl::expected<void, std::string> LoadMapTilesetData(MapTileset tileset)
{
	// In headless mode the file loaders report a missing file as success.
	if (!IsMapTilesetInstalled(tileset))
		return tl::make_unexpected(fmt::format("the game data here has no {} tiles", Info(tileset).name));
	leveltype = Info(tileset).levelType;
	tl::expected<std::unique_ptr<MegaTile[]>, std::string> til = LoadFileInMemWithStatus<MegaTile>(Info(tileset).tilFile);
	if (!til.has_value())
		return tl::make_unexpected(til.error());
	pMegaTiles = std::move(*til);
	return LoadLevelSOLData();
}

void InitMapTriggers(MapTileset tileset)
{
	switch (tileset) {
	case MapTileset::Cathedral:
		InitL1Triggers();
		break;
	case MapTileset::Catacombs:
		InitL2Triggers();
		break;
	case MapTileset::Caves:
		InitL3Triggers();
		break;
	case MapTileset::Hell:
		InitL4Triggers();
		break;
	case MapTileset::Nest:
		InitHiveTriggers();
		break;
	case MapTileset::Crypt:
		InitCryptTriggers();
		break;
	}
}

Point FindVault(Point fallback)
{
	for (int i = 0; i < numtrigs; i++) {
		if (trigs[i]._tmsg == WM_DIABNEXTLVL)
			return trigs[i].position;
	}
	return fallback;
}

Point MapSpawnTile(const KeepMap &map)
{
	if (!HasPosition(map.entrance))
		return { 16 + DMAXX, 16 + DMAXY };
	return map.entrance.megaToWorld() + Shape(map.tileset, true).spawn;
}

std::vector<MapProblem> ValidateMap(const KeepMap &map)
{
	std::vector<MapProblem> problems;
	const CellGrid grid = EffectiveCells(map);

	int floors = 0;
	int badOpenings = 0;
	int unavailableOpenings = 0;
	for (int x = 0; x < DMAXX; x++) {
		for (int y = 0; y < DMAXY; y++) {
			if (IsFloorCell(grid, x, y))
				floors++;
			const auto cell = static_cast<MapCell>(grid[x][y]);
			if (cell != MapCell::Door && cell != MapCell::Arch)
				continue;
			if (!(cell == MapCell::Door ? Info(map.tileset).doors : Info(map.tileset).arches))
				unavailableOpenings++;
			else if (map.tileset == MapTileset::Caves ? !HasDoorObject({ x, y }) : !IsOpeningSound(grid, x, y))
				badOpenings++;
		}
	}
	if (floors < MinMapFloorCells)
		problems.push_back({ fmt::format("Too small: {} of {} floor tiles", floors, MinMapFloorCells) });
	if (!HasPosition(map.entrance))
		problems.push_back({ "No entrance placed" });
	if (!HasPosition(map.vault))
		problems.push_back({ "No vault placed" });

	if (HasPosition(map.entrance) && HasPosition(map.vault)) {
		const Size up = MapStairsSize(map.tileset, true);
		const Size down = MapStairsSize(map.tileset, false);
		const bool overlap = map.entrance.x < map.vault.x + down.width && map.vault.x < map.entrance.x + up.width
		    && map.entrance.y < map.vault.y + down.height && map.vault.y < map.entrance.y + up.height;
		if (overlap) {
			problems.push_back({ "The entrance and vault overlap" });
		} else {
			const std::optional<Point> vaultTile = FindStairsTrigger(map.tileset, map.vault, Shape(map.tileset, false));
			const Point spawn = MapSpawnTile(map);
			if (!vaultTile) {
				problems.push_back({ "The vault's stairs didn't form: give them room" });
			} else {
				static DistanceField field;
				BuildField(field, *vaultTile);
				// A party spreads out on any free floor around the stairs as they fill up, so all of it has
				// to get there: one hero in a sealed pocket would hold up the raid for good. The search never
				// succeeds: it only visits the same tiles in the same order as a real spawn.
				int openTiles = 0;
				bool allReach = true;
				FindClosestValidPosition([&](Point tile) {
					if (IsOpenFloor(tile)) {
						openTiles++;
						allReach = allReach && field[tile.x][tile.y] != Unreachable;
					}
					return false;
				}, spawn, 0, HeroSpawnRadius);
				if (openTiles == 0)
					problems.push_back({ "Heroes have nowhere to appear at the entrance" });
				else if (!allReach)
					problems.push_back({ "Some floor around the entrance can't reach the vault: heroes could appear there" });
			}
		}
	}
	if (!TriggerCountIsSafe(map))
		problems.push_back({ "Too many staircases" });
	if (badOpenings > 0)
		problems.push_back({ map.tileset == MapTileset::Caves ? fmt::format("{} gate(s) didn't form: put them in a straight stretch, two or more floor cells wide", badOpenings) : fmt::format("{} door(s) not in a straight wall between floors", badOpenings), false });
	if (unavailableOpenings > 0)
		problems.push_back({ fmt::format("{} door(s) can't be shown in {}: they'll be plain wall", unavailableOpenings, MapTilesetName(map.tileset)), false });
	for (const MapObject &object : map.objects) {
		if (IsLegacyMapObject(object.kind) && !MapObjectFitsTileset(object.kind, map.tileset)) {
			problems.push_back({ fmt::format("{} won't appear in {}", MapObjectName(object.kind), MapTilesetName(map.tileset)), false });
			break;
		}
	}
	if (!leftOutObjects.empty()) {
		const auto &[first, why] = leftOutObjects.front();
		problems.push_back({ fmt::format("{} prop(s) left out. {} at {},{}: {}", leftOutObjects.size(), MapObjectName(first.kind), first.position.x, first.position.y, why), false });
	}
	return problems;
}

void SetLiveMap(const KeepMap *map, const bool *decorate)
{
	liveMap = map;
	liveDecorate = decorate;
}

void PrepareMapLevel()
{
	if (wantFreshLevel && IsKeepLevel()) {
		wantFreshLevel = false;
		MyPlayer->_pLvlVisited[KeepLevel] = false; // a fresh build, not the saved copy of this level
	}
	levelFromMap = false;
	mapLoadError.clear();
	if (!IsKeepLevel())
		return;
	if (IsSetupEditor())
		EditorPrepareSession();
	if (liveMap != nullptr && IsSetupEditor()) {
		leveltype = Info(liveMap->tileset).levelType;
		return;
	}
	const std::string &path = CurrentSetup().mapFile;
	if (path.empty()) {
		ForgetFileMap();
		return;
	}
	if (const tl::expected<void, std::string> loaded = LoadFileMap(path); !loaded.has_value()) {
		mapLoadError = fmt::format("Map not used: {}.", loaded.error());
		LogError("Dark Keep map '{}': {}", path, loaded.error());
		ForgetFileMap();
		return;
	}
	leveltype = Info(fileMap->tileset).levelType;
}

bool BuildMapLevel()
{
	if (!IsKeepLevel())
		return false;
	if (liveMap != nullptr && IsSetupEditor()) {
		ApplyLayoutToLevel(*liveMap, liveDecorate != nullptr && *liveDecorate);
		ViewPosition = MapSpawnTile(*liveMap);
		levelFromMap = true;
		return true;
	}
	if (!fileMap)
		return false;
	if (const tl::expected<void, std::string> fits = CheckTilesFitLoadedTileset(*fileMap); !fits.has_value()) {
		mapLoadError = fmt::format("Map not used: {}.", fits.error());
		ForgetFileMap();
		return false;
	}

	const KeepMap &map = *fileMap;
	Protected.reset();
	for (int x = 0; x < DMAXX; x++) {
		for (int y = 0; y < DMAXY; y++)
			dungeon[x][y] = map.tiles[x][y];
	}
	std::memcpy(pdungeon, dungeon, sizeof(pdungeon));
	DRLG_InitTrans();
	int8_t highestTrans = 0;
	for (int x = 0; x < 2 * DMAXX; x++) {
		for (int y = 0; y < 2 * DMAXY; y++) {
			dTransVal[16 + x][16 + y] = map.trans[x][y];
			highestTrans = std::max(highestTrans, map.trans[x][y]);
		}
	}
	TransVal = highestTrans < 127 ? highestTrans + 1 : highestTrans;
	themeCount = 0;
	RunPass3(map.tileset);
	if (!TriggerCountIsSafe(map)) {
		mapLoadError = "Map not used: it has too many staircases.";
		ForgetFileMap();
		DRLG_InitTrans();
		return false;
	}
	ViewPosition = MapSpawnTile(map);
	levelFromMap = true;
	return true;
}

bool InitMapObjects()
{
	leftOutObjects.clear();
	if (!levelFromMap || !IsKeepLevel())
		return false;
	const KeepMap *map = liveMap != nullptr && IsSetupEditor() ? liveMap : (fileMap ? &*fileMap : nullptr);
	if (map == nullptr)
		return false;

	switch (map->tileset) {
	case MapTileset::Cathedral:
		AddL1Objs(0, 0, MAXDUNX, MAXDUNY);
		break;
	case MapTileset::Catacombs:
		AddL2Objs(0, 0, MAXDUNX, MAXDUNY);
		break;
	case MapTileset::Crypt:
		AddCryptObjects(0, 0, MAXDUNX, MAXDUNY);
		break;
	case MapTileset::Caves:
		AddL3Objs(0, 0, MAXDUNX, MAXDUNY);
		break;
	default:
		break;
	}
	for (const MapObject &placed : map->objects) {
		if (IsLegacyMapObject(placed.kind)) {
			// Exactly as version 1 always placed them, so its maps play as they did.
			if (!MapObjectFitsTileset(placed.kind, map->tileset) || !IsTileNotSolid(placed.position) || IsObjectAtPosition(placed.position))
				continue;
			AddObject(PropObjectType(placed), placed.position);
			continue;
		}
		std::optional<std::string> why = WhyMapObjectCantStand(*map, placed);
		if (!why && ActiveObjectCount >= MaxLevelObjectsAtLoad)
			why = fmt::format("a level holds {} objects with its doors", MaxLevelObjectsAtLoad);
		if (!why && !HeadlessMode && !propArtLoaded[static_cast<size_t>(placed.kind)])
			why = "its art isn't loaded";
		if (why) {
			leftOutObjects.emplace_back(placed, std::move(*why));
			continue;
		}
		if (Object *object = AddObject(PropObjectType(placed), PropObjectTile(placed)); object != nullptr)
			MakeInert(*object, placed);
	}
	return true;
}

void AddMapObjectGraphics(uint16_t *filesWidths, size_t fileCount, size_t maxFiles)
{
	propArtLoaded.fill(false);
	if (!IsKeepLevel())
		return;
	const auto found = std::find_if(Tilesets.begin(), Tilesets.end(), [](const TilesetInfo &info) { return info.levelType == leveltype; });
	if (found == Tilesets.end())
		return;
	const auto tileset = static_cast<MapTileset>(std::distance(Tilesets.begin(), found));
	// LoadLevelObjects adds the level type's own files (doors, the Cathedral's braziers) after this.
	std::vector<bool> levelTypeFile(fileCount);
	for (const ObjectData &data : AllObjects) {
		if (data.olvltype == leveltype && data.ofindex < fileCount)
			levelTypeFile[data.ofindex] = true;
	}
	const auto filesUsed = [&]() {
		size_t used = 0;
		for (size_t i = 0; i < fileCount; i++)
			used += filesWidths[i] != 0 || levelTypeFile[i] ? 1 : 0;
		return used;
	};
	for (const MapObjectKind kind : MapObjectPalette(tileset)) {
		if (IsLegacyMapObject(kind))
			continue;
		// The engine holds so many object files a level: a kind whose files don't all fit stays out.
		const std::vector<_object_id> types = PropObjectTypes(kind);
		size_t extra = 0;
		for (const _object_id type : types) {
			const uint8_t file = AllObjects[type].ofindex;
			if (file >= fileCount) {
				extra = maxFiles + 1; // can't be loaded at all
				break;
			}
			if (filesWidths[file] == 0 && !levelTypeFile[file] && std::none_of(types.begin(), types.end(), [&](_object_id other) { return other < type && AllObjects[other].ofindex == file; }))
				extra++;
		}
		if (filesUsed() + extra > maxFiles) {
			LogError("Dark Keep: no room to load the art for {}", MapObjectName(kind));
			continue;
		}
		for (const _object_id type : types)
			filesWidths[AllObjects[type].ofindex] = AllObjects[type].animWidth;
		propArtLoaded[static_cast<size_t>(kind)] = true;
	}
}

std::optional<MapObject> SnapMapObject(MapObjectKind kind, Point tile, uint8_t style)
{
	MapObject object { kind, tile, static_cast<uint8_t>(style & MapObjectStyleMask) };
	const MapObjectPlacement placement = GetMapObjectTraits(kind).placement;
	if (placement == MapObjectPlacement::Floor)
		return object;
	if (!InDungeonBounds(tile))
		return std::nullopt;
	// A wall faces the floor on its +x or +y side: the two walls seen from the room, in the game's view.
	for (const bool facesY : { false, true }) {
		MapObject candidate = object;
		if (facesY)
			candidate.variant |= MapObjectFacesY;
		const Displacement facing = Facing(candidate);
		const Displacement along = Along(candidate);
		if (placement == MapObjectPlacement::OnWall && IsStraightWall(tile, along) && IsBareFloor(tile + facing))
			return candidate; // clicked the wall itself
		// Clicked the floor near it: a wall's own megatile can have a strip of floor in front.
		const bool onWall = placement == MapObjectPlacement::OnWall;
		for (int back = 1; back <= 2; back++) {
			const Point floor = tile - facing * (back - 1);
			if (!IsBareFloor(floor))
				break;
			const Point wall = tile - facing * back;
			if (IsStraightWall(wall, along)) {
				candidate.position = onWall ? wall : floor;
				return candidate;
			}
		}
	}
	return std::nullopt;
}

std::optional<std::string> WhyMapObjectCantStand(const KeepMap &map, const MapObject &object)
{
	if (!MapObjectFitsTileset(object.kind, map.tileset))
		return fmt::format("{} isn't available in {}", MapObjectName(object.kind), MapTilesetName(map.tileset));
	if (!IsLegacyMapObject(object.kind) && !IsPropArtInstalled(object.kind))
		return fmt::format("The game data here has no {}", MapObjectName(object.kind));
	const Point tile = object.position;
	if (!InRect(tile, { 16, 16 }, 2 * DMAXX, 2 * DMAXY))
		return "That's off the map";
	if (IsLegacyMapObject(object.kind)) {
		if (!IsTileNotSolid(tile))
			return "Objects go on the floor";
		if (IsObjectAtPosition(tile))
			return "Something already stands there";
		return std::nullopt;
	}
	if (dObject[tile.x][tile.y] != 0)
		return "Something already stands there";
	if (IsNearStairs(map, tile))
		return "Too close to the stairs";
	const MapObjectTraits traits = GetMapObjectTraits(object.kind);
	const Displacement facing = Facing(object);
	const Displacement along = Along(object);
	switch (traits.placement) {
	case MapObjectPlacement::OnWall:
		if (!IsStraightWall(tile, along))
			return "It needs a straight stretch of wall";
		if (!IsBareFloor(tile + facing) || IsDoorTile(tile + facing))
			return "It needs open floor in front";
		return std::nullopt;
	case MapObjectPlacement::AgainstWall:
		if (!IsStraightWall(tile - facing, along))
			return "It needs a straight stretch of wall behind it";
		break;
	case MapObjectPlacement::Floor:
		break;
	}
	if (!IsTileNotSolid(tile))
		return "Props go on the floor";
	if (traits.solid) {
		if (!KeepsConnectivity(tile))
			return "It would block the way";
		if (!LeavesWideWay(tile))
			return "It would leave the way past less than two tiles wide";
	}
	return std::nullopt;
}

const std::vector<std::pair<MapObject, std::string>> &LeftOutMapObjects()
{
	return leftOutObjects;
}

void NoteMapLevelLoaded()
{
	if (!mapLoadError.empty()) {
		EventPlrMsg(mapLoadError, UiFlags::ColorRed);
		mapLoadError.clear();
	}
}

void ForgetMapCache()
{
	ForgetFileMap();
}

bool IsLevelFromMap()
{
	return levelFromMap;
}

void ReloadKeepLevel()
{
	ForgetFileMap(); // the file may have changed: a test run rewrites the same one
	// Saving the level on the way out marks it visited again, so PrepareMapLevel clears this later.
	wantFreshLevel = true;
	StartNewLvl(*MyPlayer, WM_DIABNEXTLVL, KeepLevel);
}

} // namespace devilution::keeper
