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
#include <optional>
#include <queue>

#include <fmt/format.h>

#include "engine/assets.hpp"
#include "engine/load_file.hpp"
#include "engine/path.h"
#include "engine/random.hpp"
#include "keeper/keeper.hpp"
#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_editor.hpp"
#include "keeper/keeper_game.hpp"
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
		{ 170, 167, 548, 0 }, false, false },
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
		SetMaskFromCells(grid);
		KeeperL3Build(map.entrance, map.vault, decorate);
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

// ---- the file --------------------------------------------------------------------------------

constexpr std::array<char, 4> FileMagic { 'D', 'K', 'M', 'P' };
constexpr uint16_t FileVersion = 1;
constexpr uint16_t FlagPlayable = 1;
constexpr size_t HeaderSize = 16;
/** Well over any v1 file; stops a bogus size asking for gigabytes. */
constexpr size_t MaxMapFileSize = 60 * 1024;

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

/** ReloadKeepLevel asked for the keep to be built anew. */
bool wantFreshLevel = false;

void ForgetFileMap()
{
	fileMap.reset();
	fileMapPath.clear();
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

using Reach = std::array<std::array<bool, MAXDUNY>, MAXDUNX>;

/** Flood from the vault, by the same rules BuildField gives the heroes. */
void FloodReach(Reach &reach, Point start)
{
	for (auto &column : reach)
		column.fill(false);
	if (!InDungeonBounds(start))
		return;
	reach[start.x][start.y] = true;
	std::queue<Point> open;
	open.push(start);
	while (!open.empty()) {
		const Point tile = open.front();
		open.pop();
		for (const Direction dir : { Direction::South, Direction::SouthWest, Direction::West, Direction::NorthWest, Direction::North, Direction::NorthEast, Direction::East, Direction::SouthEast }) {
			const Displacement step(dir);
			const Point next = tile + step;
			if (!InDungeonBounds(next) || reach[next.x][next.y] || !IsTileWalkable(next, /*ignoreDoors=*/true))
				continue;
			if (step.deltaX != 0 && step.deltaY != 0 && (IsTileSolid(tile + Displacement { step.deltaX, 0 }) || IsTileSolid(tile + Displacement { 0, step.deltaY })))
				continue;
			reach[next.x][next.y] = true;
			open.push(next);
		}
	}
}

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
	switch (kind) {
	case MapObjectKind::Barrel:
		return "Barrel";
	case MapObjectKind::Chest:
		return "Chest";
	case MapObjectKind::Brazier:
		return "Brazier";
	}
	return "";
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
	return kind != MapObjectKind::Brazier || tileset == MapTileset::Cathedral;
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
	const size_t objectCount = std::min(map.objects.size(), MaxMapObjects);
	out.u16(static_cast<uint16_t>(objectCount));
	for (size_t i = 0; i < objectCount; i++) {
		out.u8(static_cast<uint8_t>(map.objects[i].kind));
		out.u8(static_cast<uint8_t>(map.objects[i].position.x));
		out.u8(static_cast<uint8_t>(map.objects[i].position.y));
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
	headerOut.u16(FileVersion);
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
	if (objectCount > MaxMapObjects)
		return tl::make_unexpected("it has too many objects");
	for (size_t i = 0; i < objectCount; i++) {
		MapObject object;
		const uint8_t kind = in.u8();
		const uint8_t x = in.u8();
		const uint8_t y = in.u8();
		if (kind >= NumMapObjectKinds)
			return tl::make_unexpected("it has an unknown object");
		object.kind = static_cast<MapObjectKind>(kind);
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

tl::expected<std::vector<std::byte>, std::string> ReadMapBytes(const std::string &path)
{
	std::string found;
	for (const std::string &candidate : { path, MapsFolder() + path, paths::AssetsPath() + path }) {
		if (FileExists(candidate)) {
			found = candidate;
			break;
		}
	}
	if (found.empty()) {
		// Campaign maps ship inside devilutionx.mpq where the build makes one.
		size_t assetSize = 0;
		AssetHandle asset = OpenAsset(path, assetSize);
		if (!asset.ok())
			return tl::make_unexpected("the file wasn't found");
		if (assetSize > MaxMapFileSize)
			return tl::make_unexpected("the file is too big");
		std::vector<std::byte> assetBytes(assetSize);
		if (assetSize > 0 && !asset.read(assetBytes.data(), assetSize))
			return tl::make_unexpected("the file couldn't be read");
		return assetBytes;
	}
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
			else if (!IsOpeningSound(grid, x, y))
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
				static Reach reach;
				FloodReach(reach, *vaultTile);
				// Heroes appear on the nearest free tile, so that's the one that has to get there: any tile
				// nearby would pass a sealed entrance room with the rest of the keep behind its wall.
				const std::optional<Point> arrival = FindClosestValidPosition([](Point tile) { return InDungeonBounds(tile) && IsTileWalkable(tile) && !IsObjectAtPosition(tile); }, spawn, 0, 6);
				if (!arrival || !reach[arrival->x][arrival->y])
					problems.push_back({ "Heroes can't reach the vault from the entrance" });
			}
		}
	}
	if (!TriggerCountIsSafe(map))
		problems.push_back({ "Too many staircases" });
	if (badOpenings > 0)
		problems.push_back({ fmt::format("{} door(s) not in a straight wall between floors", badOpenings), false });
	if (unavailableOpenings > 0)
		problems.push_back({ fmt::format("{} door(s) can't be shown in {}: they'll be plain wall", unavailableOpenings, MapTilesetName(map.tileset)), false });
	for (const MapObject &object : map.objects) {
		if (!MapObjectFitsTileset(object.kind, map.tileset)) {
			problems.push_back({ fmt::format("{} won't appear in {}", MapObjectName(object.kind), MapTilesetName(map.tileset)), false });
			break;
		}
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
	default:
		break;
	}
	for (const MapObject &placed : map->objects) {
		if (!MapObjectFitsTileset(placed.kind, map->tileset) || !IsTileNotSolid(placed.position) || IsObjectAtPosition(placed.position))
			continue;
		switch (placed.kind) {
		case MapObjectKind::Barrel:
			AddObject(OBJ_BARREL, placed.position);
			break;
		case MapObjectKind::Chest:
			AddObject(OBJ_CHEST1, placed.position);
			break;
		case MapObjectKind::Brazier:
			AddObject(OBJ_L1LIGHT, placed.position);
			break;
		}
	}
	return true;
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
