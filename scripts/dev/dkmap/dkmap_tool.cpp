/**
 * @file dkmap_tool.cpp
 *
 * Headless builder for the campaign's .dkmap files. It links the game's real map code, so every file
 * goes through the same joining rules, bake and validation as a map saved in the editor, then is read
 * back and loaded the way the game loads it. It opens no window and starts no game.
 *
 * Usage (with -DDARK_KEEP_MAP_TOOL=ON):
 *   dkmap_tool --data E:\Games\Diablo --assets build\assets --levels scripts\dev\dkmap\levels
 *              --out assets\keeper\campaign --doc docs\CAMPAIGN-MAPS.md
 *   dkmap_tool --data E:\Games\Diablo --assets build\assets --check-campaign
 *
 * A level source is text: `name:`, `tileset:`, `seed:` and `note:` lines, then 40 rows of 40 characters.
 *   # rock   . floor   D door   A arch   E entrance's top-left   V vault's top-left
 *   b barrel   c chest   i brazier (on floor; the object goes on the cell's first world tile)
 *
 * Exit code 0 only if every level passed.
 */
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "data/file.hpp" // IWYU pragma: keep
#include "engine/assets.hpp"
#include "engine/load_file.hpp"
#include "engine/path.h"
#include "headless_mode.hpp"
#include "keeper/keeper.hpp"
#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_map.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "objects.h"
#include "player.h"
#include "quests.h"
#include "tables/objdat.h"
#include "utils/paths.h"

using namespace devilution;
using namespace devilution::keeper;

namespace {

/** The hero's cost of going through a lock, from keeper_heroes.cpp: a detour shorter than this wins. */
constexpr int LockRoutePenalty = 40;
constexpr int EntranceKeepOut = 4;
constexpr int VaultKeepOut = 3;
constexpr uint16_t Unreachable = 0xFFFF;
/** The co-op join limit, from keeper_campaign.cpp. */
constexpr size_t MaxMapBytes = 60000;

using Field = std::array<std::array<uint16_t, MAXDUNY>, MAXDUNX>;

struct Source {
	KeepMap map;
	std::string note;
};

struct DoorReport {
	Point tile;
	Point cell;
	bool onRoute;
	/** Route length with this door blocked; Unreachable if there's no other way. */
	uint16_t blocked;
};

struct Report {
	std::vector<std::string> errors;
	std::vector<std::string> warnings;
	size_t fileSize = 0;
	int floorCells = 0;
	int buildableTiles = 0;
	uint16_t route = 0;
	std::vector<DoorReport> doors;
	/** Points along the route, and what happens if a 9 by 9 square there is shut. */
	int routePoints = 0;
	int necks = 0;
	std::vector<int> waysRound;
	int arches = 0;
	std::array<std::array<bool, DMAXY>, DMAXX> routeCells {};
	std::string preview;
};

const std::array<std::pair<std::string_view, MapTileset>, 6> TilesetNames { {
    { "Cathedral", MapTileset::Cathedral },
    { "Catacombs", MapTileset::Catacombs },
    { "Caves", MapTileset::Caves },
    { "Hell", MapTileset::Hell },
    { "Nest", MapTileset::Nest },
    { "Crypt", MapTileset::Crypt },
} };

std::string Trim(std::string text)
{
	while (!text.empty() && (text.back() == '\r' || text.back() == ' '))
		text.pop_back();
	return text;
}

std::optional<Source> ReadSource(const std::string &path, std::vector<std::string> &errors)
{
	std::ifstream in(path);
	if (!in) {
		errors.push_back("can't open the source");
		return std::nullopt;
	}
	Source source;
	source.map.author = "The Dark Keep";
	std::vector<std::string> rows;
	std::string line;
	while (std::getline(in, line)) {
		line = Trim(line);
		if (line.rfind("name:", 0) == 0) {
			source.map.name = Trim(line.substr(5));
			source.map.name.erase(0, source.map.name.find_first_not_of(' '));
		} else if (line.rfind("tileset:", 0) == 0) {
			std::string name = line.substr(8);
			name.erase(0, name.find_first_not_of(' '));
			bool known = false;
			for (const auto &[tilesetName, tileset] : TilesetNames) {
				if (tilesetName == name) {
					source.map.tileset = tileset;
					known = true;
				}
			}
			if (!known)
				errors.push_back("unknown tileset " + name);
		} else if (line.rfind("seed:", 0) == 0) {
			source.map.decorSeed = static_cast<uint32_t>(std::stoul(line.substr(5), nullptr, 0));
		} else if (line.rfind("note:", 0) == 0) {
			std::string note = line.substr(5);
			note.erase(0, note.find_first_not_of(' '));
			source.note += (source.note.empty() ? "" : " ") + note;
		} else if (!line.empty()) {
			rows.push_back(line);
		}
	}
	if (rows.size() != DMAXY) {
		errors.push_back(fmt::format("{} rows, not {}", rows.size(), DMAXY));
		return std::nullopt;
	}
	for (int y = 0; y < DMAXY; y++) {
		if (rows[y].size() != DMAXX) {
			errors.push_back(fmt::format("row {} is {} wide, not {}", y, rows[y].size(), DMAXX));
			return std::nullopt;
		}
		for (int x = 0; x < DMAXX; x++) {
			const Point world { 16 + 2 * x, 16 + 2 * y };
			MapCell cell = MapCell::Floor;
			switch (rows[y][x]) {
			case '#': cell = MapCell::Rock; break;
			case '.': break;
			case 'D': cell = MapCell::Door; break;
			case 'A': cell = MapCell::Arch; break;
			case 'E': source.map.entrance = { x, y }; break;
			case 'V': source.map.vault = { x, y }; break;
			case 'b': source.map.objects.push_back({ MapObjectKind::Barrel, world }); break;
			case 'c': source.map.objects.push_back({ MapObjectKind::Chest, world }); break;
			case 'i': source.map.objects.push_back({ MapObjectKind::Brazier, world }); break;
			default:
				errors.push_back(fmt::format("unknown character '{}' at {},{}", rows[y][x], x, y));
				return std::nullopt;
			}
			source.map.setCell(x, y, cell);
		}
	}
	return source;
}

bool LoadTileset(MapTileset tileset, std::string &error)
{
	static constexpr std::array<const char *, 6> TilFiles {
		"levels\\l1data\\l1.til",
		"levels\\l2data\\l2.til",
		"levels\\l3data\\l3.til",
		"levels\\l4data\\l4.til",
		"nlevels\\l6data\\l6.til",
		"nlevels\\l5data\\l5.til",
	};
	static constexpr std::array<dungeon_type, 6> LevelTypes { DTYPE_CATHEDRAL, DTYPE_CATACOMBS, DTYPE_CAVES, DTYPE_HELL, DTYPE_NEST, DTYPE_CRYPT };
	const auto index = static_cast<size_t>(tileset);
	leveltype = LevelTypes[index];
	tl::expected<std::unique_ptr<MegaTile[]>, std::string> til = LoadFileInMemWithStatus<MegaTile>(TilFiles[index]);
	if (!til.has_value()) {
		error = til.error();
		return false;
	}
	pMegaTiles = std::move(*til);
	const tl::expected<void, std::string> sol = LoadLevelSOLData();
	if (!sol.has_value()) {
		error = sol.error();
		return false;
	}
	return true;
}

void InitTriggers()
{
	switch (leveltype) {
	case DTYPE_CATHEDRAL:
		InitL1Triggers();
		break;
	case DTYPE_CATACOMBS:
		InitL2Triggers();
		break;
	case DTYPE_CAVES:
		InitL3Triggers();
		break;
	case DTYPE_HELL:
		InitL4Triggers();
		break;
	case DTYPE_NEST:
		InitHiveTriggers();
		break;
	case DTYPE_CRYPT:
		InitCryptTriggers();
		break;
	default:
		numtrigs = 0;
		break;
	}
}

bool IsPassableTile(Point tile)
{
	return InDungeonBounds(tile) && IsTileWalkable(tile, /*ignoreDoors=*/true);
}

constexpr std::array<Direction, 8> Directions { Direction::South, Direction::SouthWest, Direction::West, Direction::NorthWest, Direction::North, Direction::NorthEast, Direction::East, Direction::SouthEast };

/** BuildField in keeper_heroes.cpp, with a square of tiles optionally shut. */
void BuildField(Field &field, Point target, std::optional<Point> shut = std::nullopt, int shutRadius = 0)
{
	const auto isShut = [&](Point tile) {
		return shut && std::abs(tile.x - shut->x) <= shutRadius && std::abs(tile.y - shut->y) <= shutRadius;
	};
	for (auto &column : field)
		column.fill(Unreachable);
	field[target.x][target.y] = 0;
	std::queue<Point> open;
	open.push(target);
	while (!open.empty()) {
		const Point tile = open.front();
		open.pop();
		const uint16_t next = field[tile.x][tile.y] + 1;
		for (const Direction dir : Directions) {
			const Displacement step(dir);
			const Point neighbour = tile + step;
			if (!IsPassableTile(neighbour) || field[neighbour.x][neighbour.y] != Unreachable || isShut(neighbour))
				continue;
			if (step.deltaX != 0 && step.deltaY != 0 && (IsTileSolid(tile + Displacement { step.deltaX, 0 }) || IsTileSolid(tile + Displacement { 0, step.deltaY })))
				continue;
			field[neighbour.x][neighbour.y] = next;
			open.push(neighbour);
		}
	}
}

Point CellOf(Point tile)
{
	return { (tile.x - 16) / 2, (tile.y - 16) / 2 };
}

/** The loaded level, as the game sees it: where heroes go, what stands where. Fills `report`. */
void Analyse(const KeepMap &map, Report &report)
{
	InitTriggers();
	std::optional<Point> vault;
	std::optional<Point> up;
	for (int i = 0; i < numtrigs; i++) {
		if (trigs[i]._tmsg == WM_DIABNEXTLVL && !vault)
			vault = trigs[i].position;
		if (trigs[i]._tmsg == WM_DIABPREVLVL && !up)
			up = trigs[i].position;
	}
	if (!vault) {
		report.errors.emplace_back("no down-stairs trigger: the game would fall back to the entrance as the vault");
		return;
	}
	if (!up)
		report.warnings.emplace_back("no up-stairs trigger");
	const Point firstVaultTile = map.vault.megaToWorld();
	const Size vaultSize = MapStairsSize(map.tileset, false);
	if (vault->x < firstVaultTile.x || vault->y < firstVaultTile.y || vault->x >= firstVaultTile.x + 2 * vaultSize.width || vault->y >= firstVaultTile.y + 2 * vaultSize.height)
		report.errors.push_back(fmt::format("the game's first down trigger at {},{} isn't in the vault's stairs", vault->x, vault->y));
	if (numtrigs > 3)
		report.warnings.push_back(fmt::format("{} stair triggers on the level", numtrigs));

	const Point spawn = MapSpawnTile(map);
	const std::optional<Point> arrival = FindClosestValidPosition([](Point tile) { return InDungeonBounds(tile) && IsTileWalkable(tile) && !IsObjectAtPosition(tile); }, spawn, 0, 6);
	if (!arrival) {
		report.errors.emplace_back("heroes have nowhere to appear");
		return;
	}
	static Field field;
	BuildField(field, *vault);
	report.route = field[arrival->x][arrival->y];
	if (report.route == Unreachable) {
		report.errors.emplace_back("the vault can't be reached from the entrance by BuildField's rules");
		return;
	}

	// The way a hero walks, downhill from where they appear.
	std::vector<Point> route;
	for (Point at = *arrival; at != *vault;) {
		route.push_back(at);
		const uint16_t here = field[at.x][at.y];
		std::optional<Point> next;
		for (const Direction dir : Directions) {
			const Displacement step(dir);
			const Point neighbour = at + step;
			if (!InDungeonBounds(neighbour) || field[neighbour.x][neighbour.y] >= here)
				continue;
			if (step.deltaX != 0 && step.deltaY != 0 && (IsTileSolid(at + Displacement { step.deltaX, 0 }) || IsTileSolid(at + Displacement { 0, step.deltaY })))
				continue;
			next = neighbour;
			break;
		}
		if (!next)
			break;
		at = *next;
	}
	for (const Point tile : route) {
		const Point cell = CellOf(tile);
		if (cell.x >= 0 && cell.y >= 0 && cell.x < DMAXX && cell.y < DMAXY)
			report.routeCells[cell.x][cell.y] = true;
	}

	// Necks and ways round, which is what a door-less tileset has instead of locks.
	for (size_t i = 6; i + 6 < route.size(); i += 6) {
		static Field withoutSquare;
		BuildField(withoutSquare, *vault, route[i], 4);
		report.routePoints++;
		const uint16_t length = withoutSquare[arrival->x][arrival->y];
		if (length == Unreachable)
			report.necks++;
		else
			report.waysRound.push_back(length - report.route);
	}

	// Doors: every painted door must exist, and what a lock on it would do.
	std::array<std::array<bool, DMAXY>, DMAXX> doorCell {};
	for (int i = 0; i < ActiveObjectCount; i++) {
		const Object &object = Objects[ActiveObjects[i]];
		if (!object.isDoor())
			continue;
		const Point cell = CellOf(object.position);
		if (cell.x < 0 || cell.y < 0 || cell.x >= DMAXX || cell.y >= DMAXY) {
			report.errors.push_back(fmt::format("a door object at world {},{} is off the map", object.position.x, object.position.y));
			continue;
		}
		doorCell[cell.x][cell.y] = true;
		static Field withoutDoor;
		BuildField(withoutDoor, *vault, object.position);
		const bool onRoute = std::find(route.begin(), route.end(), object.position) != route.end();
		report.doors.push_back({ object.position, cell, onRoute, withoutDoor[arrival->x][arrival->y] });
	}
	if (report.doors.size() > MaxLocks)
		report.warnings.push_back(fmt::format("{} doors, more than the {} locks a player can have", report.doors.size(), MaxLocks));

	int deadFloor = 0;
	for (int x = 0; x < DMAXX; x++) {
		for (int y = 0; y < DMAXY; y++) {
			const MapCell cell = map.cell(x, y);
			const Point first = Point { x, y }.megaToWorld();
			bool reached = false;
			bool stairs = false;
			for (int dx = 0; dx < 2; dx++) {
				for (int dy = 0; dy < 2; dy++) {
					const Point tile = first + Displacement { dx, dy };
					reached = reached || field[tile.x][tile.y] != Unreachable;
				}
			}
			const auto inside = [&](Point position, bool entrance) {
				const Size size = MapStairsSize(map.tileset, entrance);
				return position.x >= 0 && x >= position.x && y >= position.y && x < position.x + size.width && y < position.y + size.height;
			};
			stairs = inside(map.entrance, true) || inside(map.vault, false);
			if (cell == MapCell::Floor) {
				report.floorCells++;
				if (!reached && !stairs)
					deadFloor++;
			}
			if (cell == MapCell::Door && !doorCell[x][y])
				report.errors.push_back(fmt::format("the door at cell {},{} didn't form", x, y));
			if (cell == MapCell::Arch) {
				report.arches++;
				if (!reached)
					report.errors.push_back(fmt::format("the arch at cell {},{} isn't passable", x, y));
			}
		}
	}
	if (deadFloor > 0)
		report.warnings.push_back(fmt::format("{} floor cells can't be walked to", deadFloor));

	// Open floor a player could really build on, by the rules of CanBuildAt.
	for (int x = 0; x < MAXDUNX; x++) {
		for (int y = 0; y < MAXDUNY; y++) {
			const Point tile { x, y };
			if (field[x][y] == Unreachable || !IsTileWalkable(tile) || IsObjectAtPosition(tile))
				continue;
			if (tile.WalkingDistance(*arrival) >= EntranceKeepOut && tile.WalkingDistance(*vault) >= VaultKeepOut)
				report.buildableTiles++;
		}
	}
}

std::string Preview(const KeepMap &map, const Report &report)
{
	std::vector<std::string> rows(DMAXY, std::string(DMAXX, ' '));
	const auto nearFloor = [&](int x, int y) {
		for (int dx = -1; dx <= 1; dx++) {
			for (int dy = -1; dy <= 1; dy++) {
				const int nx = x + dx;
				const int ny = y + dy;
				if (nx >= 0 && ny >= 0 && nx < DMAXX && ny < DMAXY && map.cell(nx, ny) != MapCell::Rock)
					return true;
			}
		}
		return false;
	};
	for (int y = 0; y < DMAXY; y++) {
		for (int x = 0; x < DMAXX; x++) {
			switch (map.cell(x, y)) {
			case MapCell::Rock:
				rows[y][x] = nearFloor(x, y) ? '#' : ' ';
				break;
			case MapCell::Floor:
				rows[y][x] = report.routeCells[x][y] ? '+' : '.';
				break;
			case MapCell::Door:
				rows[y][x] = 'D';
				break;
			case MapCell::Arch:
				rows[y][x] = 'A';
				break;
			}
		}
	}
	const auto stamp = [&](Point position, bool entrance, char c) {
		const Size size = MapStairsSize(map.tileset, entrance);
		for (int y = 0; y < size.height; y++) {
			for (int x = 0; x < size.width; x++)
				rows[position.y + y][position.x + x] = c;
		}
	};
	stamp(map.entrance, true, 'E');
	stamp(map.vault, false, 'V');
	for (const MapObject &object : map.objects) {
		const Point cell = CellOf(object.position);
		rows[cell.y][cell.x] = object.kind == MapObjectKind::Barrel ? 'b' : (object.kind == MapObjectKind::Chest ? 'c' : 'i');
	}
	std::string text;
	for (const std::string &row : rows) {
		std::string line = row;
		while (!line.empty() && line.back() == ' ')
			line.pop_back();
		text += line + "\n";
	}
	return text;
}

/** Bakes one level, writes it, reads it back and loads it as the game would. */
Report BuildLevel(const std::string &sourcePath, const std::string &outPath, Source &source, bool &parsed)
{
	Report report;
	parsed = false;
	std::optional<Source> read = ReadSource(sourcePath, report.errors);
	if (!read)
		return report;
	source = std::move(*read);
	parsed = true;
	KeepMap &map = source.map;
	if (map.name.empty())
		report.errors.emplace_back("no name");
	if (map.entrance.x < 0 || map.vault.x < 0) {
		report.errors.emplace_back("no entrance or no vault");
		return report;
	}
	if (!MapStairsFit(map.tileset, true, map.entrance) || !MapStairsFit(map.tileset, false, map.vault)) {
		report.errors.emplace_back("stairs off the paintable area");
		return report;
	}
	for (const MapObject &object : map.objects) {
		if (!MapObjectFitsTileset(object.kind, map.tileset))
			report.errors.push_back(fmt::format("{} isn't available in {}", MapObjectName(object.kind), MapTilesetName(map.tileset)));
	}
	std::string error;
	if (!LoadTileset(map.tileset, error)) {
		report.errors.push_back(fmt::format("can't load the {} tileset: {}", MapTilesetName(map.tileset), error));
		return report;
	}

	// As in the editor: this is the live map, and the level is built from it.
	static bool decorate = false;
	currlevel = KeepLevel;
	setlevel = false;
	gbIsMultiplayer = false;
	SetCurrentSetup(SetupForMapEditor());
	SetLiveMap(&map, &decorate);
	std::memset(dObject, 0, sizeof(dObject));
	if (!BuildMapLevel())
		report.errors.emplace_back("BuildMapLevel refused the live map");
	const bool playable = BakePlayableMap(map);
	if (!playable) {
		// Say why, from the plain level, which is what the author drew.
		ApplyLayoutToLevel(map, false);
		std::memset(dObject, 0, sizeof(dObject));
		InitObjects();
		for (const MapProblem &problem : ValidateMap(map)) {
			if (problem.blocking)
				report.errors.push_back("plain: " + problem.text);
		}
		if (report.errors.empty()) {
			report.errors.emplace_back("every roll of the decoration broke it");
		}
	}
	const std::vector<std::byte> bytes = SerializeMap(map);
	report.fileSize = bytes.size();
	if (bytes.size() >= MaxMapBytes)
		report.errors.push_back(fmt::format("{} bytes is over the co-op limit", bytes.size()));
	SetLiveMap(nullptr, nullptr);
	if (!report.errors.empty())
		return report;

	const tl::expected<void, std::string> written = WriteMapBytes(outPath, bytes);
	if (!written.has_value()) {
		report.errors.push_back("can't write: " + written.error());
		return report;
	}

	// From here on the file alone, by the road the game takes.
	const tl::expected<std::vector<std::byte>, std::string> reread = ReadMapBytes(outPath);
	if (!reread.has_value()) {
		report.errors.push_back("can't read it back: " + reread.error());
		return report;
	}
	const tl::expected<KeepMap, std::string> loaded = ParseMap(reread->data(), reread->size());
	if (!loaded.has_value()) {
		report.errors.push_back("doesn't parse: " + loaded.error());
		return report;
	}
	if (!loaded->playable)
		report.errors.emplace_back("isn't flagged playable");
	SetCurrentSetup(SetupForCustomMap(outPath));
	ForgetMapCache();
	PrepareMapLevel();
	std::memset(dObject, 0, sizeof(dObject));
	if (!BuildMapLevel() || !IsLevelFromMap()) {
		report.errors.emplace_back("the game's loader refused the file");
		return report;
	}
	InitObjects();
	for (const MapProblem &problem : ValidateMap(*loaded)) {
		if (problem.blocking)
			report.errors.push_back("loaded: " + problem.text);
		else
			report.warnings.push_back(problem.text);
	}
	Analyse(*loaded, report);
	report.preview = Preview(*loaded, report);
	ForgetMapCache();
	return report;
}

std::string RouteSummary(const Report &report)
{
	if (report.routePoints == 0)
		return "No route points";
	std::vector<int> ways = report.waysRound;
	std::sort(ways.begin(), ways.end());
	std::string text = fmt::format("Shut at any of {} points along the route, {} leave no way on", report.routePoints, report.necks);
	int seriousWays = 0;
	for (const int way : ways) {
		if (way >= 10)
			seriousWays++;
	}
	if (seriousWays > 0)
		text += fmt::format("; {} have a real way round (+10 tiles or more, longest +{})", seriousWays, ways.back());
	return text;
}

std::string DoorSummary(const Report &report)
{
	int forced = 0;
	int detour = 0;
	int off = 0;
	int turnedByLock = 0;
	std::vector<int> detours;
	for (const DoorReport &door : report.doors) {
		if (door.blocked == Unreachable) {
			forced++;
		} else if (door.onRoute) {
			detour++;
			detours.push_back(door.blocked - report.route);
			if (door.blocked - report.route < LockRoutePenalty)
				turnedByLock++;
		} else {
			off++;
		}
	}
	std::string text = fmt::format("{} doors: {} can't be bypassed, {} on the short route with a way round", report.doors.size(), forced, detour);
	if (!detours.empty()) {
		std::sort(detours.begin(), detours.end());
		text += " (detour +";
		for (size_t i = 0; i < detours.size(); i++)
			text += (i == 0 ? "" : ", +") + std::to_string(detours[i]);
		text += " tiles)";
	}
	if (detour > 0)
		text += fmt::format(", of which a lock turns the party away from {}", turnedByLock);
	text += fmt::format(", {} off the short route", off);
	return text;
}

int RunLevels(const std::string &levelsDir, const std::string &outDir, const std::string &docPath)
{
	std::string doc;
	int failures = 0;
	for (int level = 1; level <= NumCampaignLevels; level++) {
		const std::string sourcePath = fmt::format("{}/level_{:02}.txt", levelsDir, level);
		const std::string outPath = fmt::format("{}/level_{:02}.dkmap", outDir, level);
		if (!std::ifstream(sourcePath)) {
			fmt::print("level {:2}: no source, stays random\n", level);
			continue;
		}
		Source source;
		bool parsed = false;
		const Report report = BuildLevel(sourcePath, outPath, source, parsed);
		const std::string_view expected = CampaignLevelRules(level).name;
		std::vector<std::string> errors = report.errors;
		if (parsed && source.map.name != expected)
			errors.push_back(fmt::format("named '{}', but the campaign calls it '{}'", source.map.name, expected));
		fmt::print("level {:2} {:<18} {:<10} route {:4} floor {:4} buildable {:4} {} bytes\n", level, source.map.name, parsed ? MapTilesetName(source.map.tileset) : "?", report.route, report.floorCells, report.buildableTiles, report.fileSize);
		fmt::print("         {}\n         {}\n", DoorSummary(report), RouteSummary(report));
		for (const std::string &warning : report.warnings)
			fmt::print("         warning: {}\n", warning);
		for (const std::string &error : errors)
			fmt::print("         ERROR: {}\n", error);
		if (!errors.empty()) {
			failures++;
			std::remove(outPath.c_str());
			continue;
		}
		doc += fmt::format("## Level {}: {}\n\n", level, source.map.name);
		doc += fmt::format("**{}.** Route {} tiles, {} floor cells, {} tiles to build on. {}. {}.{}\n\n", MapTilesetName(source.map.tileset), report.route, report.floorCells, report.buildableTiles, DoorSummary(report), RouteSummary(report), report.arches > 0 ? fmt::format(" {} arches.", report.arches) : "");
		doc += source.note + "\n\n```\n" + report.preview + "```\n\n";
	}
	if (!docPath.empty() && failures == 0) {
		// Everything up to the marker is written by hand.
		constexpr std::string_view Marker = "<!-- generated by dkmap_tool from here on -->\n";
		std::ifstream in(docPath, std::ios::binary);
		std::stringstream current;
		current << in.rdbuf();
		in.close();
		std::string head = current.str();
		const size_t at = head.find(Marker);
		head = at == std::string::npos ? std::string(Marker) : head.substr(0, at + Marker.size());
		std::ofstream out(docPath, std::ios::binary);
		out << head << "\n" << doc;
	}
	return failures == 0 ? 0 : 1;
}

/** What a shipped game does: find each level's map by asset name, then parse it. */
int CheckCampaign()
{
	int failures = 0;
	int found = 0;
	for (int level = 1; level <= NumCampaignLevels; level++) {
		const KeepSetup setup = SetupForCampaignLevel(level);
		if (setup.mapFile.empty()) {
			fmt::print("level {:2}: no map, stays random\n", level);
			continue;
		}
		found++;
		const tl::expected<std::vector<std::byte>, std::string> bytes = ReadMapBytes(setup.mapFile);
		if (!bytes.has_value()) {
			fmt::print("level {:2}: ERROR {}: {}\n", level, setup.mapFile, bytes.error());
			failures++;
			continue;
		}
		const tl::expected<KeepMap, std::string> map = ParseMap(bytes->data(), bytes->size());
		if (!map.has_value() || !map->playable || map->name != CampaignLevelRules(level).name) {
			fmt::print("level {:2}: ERROR {} is bad\n", level, setup.mapFile);
			failures++;
			continue;
		}
		fmt::print("level {:2}: {} -> {} ({} bytes)\n", level, setup.mapFile, map->name, bytes->size());
	}
	fmt::print("{} of {} levels have a map\n", found, NumCampaignLevels);
	return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
	std::string data;
	std::string assets;
	std::string levels;
	std::string out;
	std::string doc;
	bool checkCampaign = false;
	for (int i = 1; i < argc; i++) {
		const std::string arg = argv[i];
		const auto value = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
		if (arg == "--data")
			data = value();
		else if (arg == "--assets")
			assets = value();
		else if (arg == "--levels")
			levels = value();
		else if (arg == "--out")
			out = value();
		else if (arg == "--doc")
			doc = value();
		else if (arg == "--check-campaign")
			checkCampaign = true;
	}
	if (data.empty() || assets.empty() || (!checkCampaign && (levels.empty() || out.empty()))) {
		std::cerr << "usage: dkmap_tool --data <mpq folder> --assets <assets folder> (--levels <folder> --out <folder> [--doc <file>] | --check-campaign)\n";
		return 2;
	}
	const auto folder = [](std::string path) {
		if (!path.empty() && path.back() != '/' && path.back() != '\\')
			path += '\\';
		return path;
	};
	HeadlessMode = true;
	paths::SetBasePath(folder(data));
	paths::SetAssetsPath(folder(assets));
	// Nothing of the player's: the map code creates a maps folder under here.
	paths::SetPrefPath((std::filesystem::temp_directory_path() / "dkmap_tool").string() + "\\");
	paths::SetConfigPath(paths::PrefPath());

	LoadCoreArchives();
	LoadGameArchives();
	if (checkCampaign)
		return CheckCampaign();

	Players.resize(1);
	MyPlayer = &Players[0];
	LoadObjectData();
	const int result = RunLevels(levels, out, doc);
	return result;
}
