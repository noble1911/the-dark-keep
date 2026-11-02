/**
 * @file keeper/keeper_campaign.cpp
 *
 * The campaign: level table, progress file, and telling joiners what is being played.
 */
#include "keeper/keeper_campaign.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <utility>

#ifdef USE_SDL3
#include <SDL3/SDL_error.h>
#else
#include <SDL.h>
#endif

#include <fmt/format.h>

#include "appfat.h"
#include "keeper/keeper.hpp"
#include "multi.h"
#include "storm/storm_net.hpp"
#include "utils/file_util.h"
#include "utils/log.hpp"
#include "utils/paths.h"

namespace devilution::keeper {

namespace {

// clang-format off
/**
 * The campaign, easiest first. Balance is tuned here. Level 1's seed is the keep's original layout.
 * Columns: name, raids, starting gold, hero strength %, first party, party growth (1/100 hero per raid), seed.
 */
constexpr std::array<LevelRules, NumCampaignLevels> CampaignLevels { {
	{ "Old Cellars",       5, 1200, 100, 2,  80, KeepLevelSeed },
	{ "Rat Warrens",       5, 1250, 105, 2,  80, 0x1F3A97C4 },
	{ "Sunken Chapel",     5, 1300, 110, 2,  85, 0xA4C21E0B },
	{ "Bone Pits",         6, 1350, 115, 2,  85, 0x57D0E36A },
	{ "Black Vaults",      6, 1400, 125, 2,  90, 0xC93B7721 },
	{ "Weeping Halls",     6, 1500, 135, 3,  80, 0x2E86F15D },
	{ "Ashen Crypts",      7, 1600, 145, 3,  80, 0x7BA4096E },
	{ "Iron Gaol",         7, 1700, 155, 3,  85, 0xE15C38D2 },
	{ "Maggot Deeps",      7, 1800, 165, 3,  90, 0x39F7A640 },
	{ "Hollow Court",      8, 1950, 180, 3,  90, 0x94D2C1B7 },
	{ "Blood Galleries",   8, 2100, 195, 3,  95, 0x0C6E5FA9 },
	{ "Shrouded Nave",     8, 2250, 210, 4,  85, 0xD7318B4C },
	{ "Gallows Reach",     8, 2400, 225, 4,  90, 0x63AF02E5 },
	{ "Cinder Keep",       9, 2600, 240, 4,  90, 0xB8E97D13 },
	{ "Howling Stair",     9, 2800, 260, 4,  95, 0x4A15C6F8 },
	{ "Tomb of Kings",     9, 3000, 280, 4, 100, 0xF20D93A6 },
	{ "Silent Abbey",      9, 3200, 300, 5,  90, 0x8C7B24D1 },
	{ "Chains of Night",  10, 3500, 325, 5,  90, 0x26E85B3F },
	{ "Burning Sepulcre", 10, 3800, 350, 5,  95, 0xAD4F1C72 },
	{ "Throne of Ash",    10, 4200, 400, 6, 100, 0x71C6E09B },
} };

/** Custom maps and the editor have no place in the curve: these are the keep's original rules. */
constexpr LevelRules CustomRules { "Custom Map", 7, 1200, 100, 2, 80, KeepLevelSeed };
// clang-format on

/** However a level's numbers are tuned, a raid never brings more heroes than this. */
constexpr int MaxPartySize = 14;

/** The map travels in the join packet, whose frame limit is 64 KiB including its headers. */
constexpr size_t MaxMapBytes = 60000;

constexpr std::string_view ProgressFileName = "campaign_progress.txt";
constexpr std::string_view ProgressHeader = "DARKKEEP-CAMPAIGN";
constexpr int ProgressVersion = 1;

KeepSetup currentSetup;

/** Rank from --keeper-level, so a test of a late level has the units it is meant to be played with. */
int devRank = 0;

bool joinedGame = false;
int hostRank = 1;

bool restartRequested = false;
Rematch pendingRematch;

bool progressLoaded = false;
/** A progress file from a newer build is left alone rather than overwritten. */
bool progressWritable = true;
int levelsCleared = 0;

std::string ProgressPath()
{
	return paths::PrefPath() + std::string(ProgressFileName);
}

bool ReadProgressFile(const std::string &path, int &cleared)
{
	FILE *file = OpenFile(path.c_str(), "rb");
	if (file == nullptr)
		return false;
	char header[32] {};
	int version = 0;
	int value = 0;
	const int fields = std::fscanf(file, "%31s %d %d", header, &version, &value);
	std::fclose(file);
	if (fields != 3 || ProgressHeader != header) {
		LogError("Ignoring unreadable campaign progress file {}", path);
		return false;
	}
	if (version > ProgressVersion) {
		LogError("Campaign progress file {} is from a newer version ({})", path, version);
		progressWritable = false;
		return false;
	}
	cleared = std::clamp(value, 0, NumCampaignLevels);
	return true;
}

void LoadProgress()
{
	if (progressLoaded)
		return;
	progressLoaded = true;
	levelsCleared = 0;
	const std::string path = ProgressPath();
	// Saving removes the old file just before moving the new one in; survive a crash between the two.
	if (!ReadProgressFile(path, levelsCleared) && progressWritable)
		ReadProgressFile(path + ".tmp", levelsCleared);
}

void SaveProgress()
{
	if (!progressWritable)
		return;
	const std::string path = ProgressPath();
	const std::string temp = path + ".tmp";
	FILE *file = OpenFile(temp.c_str(), "wb");
	if (file == nullptr) {
		LogError("Cannot write campaign progress file {}", temp);
		return;
	}
	const std::string text = fmt::format("{} {} {}\n", ProgressHeader, ProgressVersion, levelsCleared);
	const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
	const bool closed = std::fclose(file) == 0;
	if (!written || !closed) {
		LogError("Cannot write campaign progress file {}", temp);
		return;
	}
	// The engine's RenameFile will not replace an existing file on Windows.
	if (FileExists(path))
		RemoveFile(path.c_str());
	RenameFile(temp.c_str(), path.c_str());
}

/** Appends the file to `out`. @return false if it cannot be read or is larger than `maxBytes`. */
bool AppendFile(const std::string &path, std::vector<char> &out, size_t maxBytes)
{
	FILE *file = OpenFile(path.c_str(), "rb");
	if (file == nullptr)
		return false;
	const size_t start = out.size();
	out.resize(start + maxBytes + 1);
	const size_t got = std::fread(out.data() + start, 1, maxBytes + 1, file);
	std::fclose(file);
	out.resize(start + got);
	return got <= maxBytes;
}

uint32_t HashName(std::string_view text)
{
	uint32_t hash = 2166136261U; // FNV-1a
	for (const char c : text) {
		hash ^= static_cast<uint8_t>(c);
		hash *= 16777619U;
	}
	return hash;
}

/** Where a joiner keeps the host's map. A subfolder, so it stays out of the Custom Map list. */
std::string ReceivedMapPath()
{
	const std::string folder = MapsFolder() + "received" + DIRECTORY_SEPARATOR_STR;
	RecursivelyCreateDir(folder.c_str());
	return folder + "from_host.dkmap";
}

} // namespace

const KeepSetup &CurrentSetup()
{
	return currentSetup;
}

void SetCurrentSetup(const KeepSetup &setup)
{
	currentSetup = setup;
}

int CampaignRank()
{
	if (joinedGame)
		return hostRank;
	return std::max(SavedRank(), devRank);
}

const LevelRules &CampaignLevelRules(int level)
{
	return CampaignLevels[std::clamp(level, 1, NumCampaignLevels) - 1];
}

const LevelRules &CustomMapRules()
{
	return CustomRules;
}

const LevelRules &CurrentRules()
{
	return currentSetup.mode == GameMode::Campaign ? CampaignLevelRules(currentSetup.campaignLevel) : CustomRules;
}

int NumWaves()
{
	// --keeper-start-wave may ask for a raid past this level's last one; make it exist.
	return std::max(CurrentRules().raids, TestStartWave);
}

int StartingGold()
{
	return CurrentRules().startingGold;
}

int HeroStrengthPercent()
{
	return CurrentRules().heroStrengthPercent;
}

int CurrentLevelNumber()
{
	return currentSetup.mode == GameMode::Campaign ? std::clamp(currentSetup.campaignLevel, 1, NumCampaignLevels) : 1;
}

int ScaleByHeroStrength(int value)
{
	return value * HeroStrengthPercent() / 100;
}

int PartySizeForWave(int wave)
{
	const LevelRules &rules = CurrentRules();
	return std::clamp(rules.startingParty + rules.partyGrowthPercent * (std::max(wave, 1) - 1) / 100, 1, MaxPartySize);
}

KeepSetup SetupForCampaignLevel(int level)
{
	KeepSetup setup;
	setup.mode = GameMode::Campaign;
	setup.campaignLevel = std::clamp(level, 1, NumCampaignLevels);
	setup.seed = CampaignLevelRules(setup.campaignLevel).seed;
	const std::string map = fmt::format("{0}keeper{1}campaign{1}level_{2:02}.dkmap", paths::AssetsPath(), DIRECTORY_SEPARATOR_STR, setup.campaignLevel);
	if (FileExists(map))
		setup.mapFile = map;
	return setup;
}

KeepSetup SetupForCustomMap(std::string mapFile)
{
	KeepSetup setup;
	setup.mode = GameMode::CustomMap;
	setup.seed = HashName(mapFile);
	setup.mapFile = std::move(mapFile);
	return setup;
}

KeepSetup SetupForMapEditor()
{
	KeepSetup setup;
	setup.mode = GameMode::MapEditor;
	setup.seed = KeepLevelSeed;
	return setup;
}

void ApplyAutoStartSetup()
{
	if (TestCampaignLevel > 0)
		devRank = std::clamp(TestCampaignLevel, 1, NumCampaignLevels);
	SetCurrentSetup(SetupForCampaignLevel(TestCampaignLevel > 0 ? TestCampaignLevel : 1));
}

std::string MapsFolder()
{
	const std::string folder = paths::PrefPath() + "maps" + DIRECTORY_SEPARATOR_STR;
	RecursivelyCreateDir(folder.c_str());
	return folder;
}

std::vector<std::string> ListCustomMaps()
{
	constexpr std::string_view Extension = ".dkmap";
	std::vector<std::string> maps;
	for (std::string &file : ListFiles(MapsFolder().c_str())) {
		// The editor's autosave and test-run files start with an underscore.
		if (file.size() <= Extension.size() || file[0] == '_')
			continue;
		const std::string_view tail = std::string_view(file).substr(file.size() - Extension.size());
		const bool matches = std::equal(tail.begin(), tail.end(), Extension.begin(), [](char a, char b) {
			return std::tolower(static_cast<unsigned char>(a)) == b;
		});
		if (matches)
			maps.push_back(std::move(file));
	}
	std::sort(maps.begin(), maps.end());
	return maps;
}

int LevelsCleared()
{
	LoadProgress();
	return levelsCleared;
}

int SavedRank()
{
	return std::min(LevelsCleared() + 1, NumCampaignLevels);
}

void NoteVictory()
{
	if (joinedGame || currentSetup.mode != GameMode::Campaign)
		return;
	// The menus only offer levels up to the rank, so this is the only way forward. Being strict
	// also keeps --keeper-level from unlocking the whole campaign in a real profile.
	if (currentSetup.campaignLevel != LevelsCleared() + 1)
		return;
	levelsCleared = currentSetup.campaignLevel;
	SaveProgress();
}

void BeginSession()
{
	joinedGame = false;
	hostRank = 1;
	restartRequested = false;
}

bool IsJoinedGame()
{
	return joinedGame;
}

bool HostGame(GameData &gameData, const char *gameName, const char *password, int *playerId)
{
	gameData.nDifficulty = DIFF_NORMAL; // how hard a game is comes from the campaign
	gameData.keeperMode = static_cast<uint8_t>(currentSetup.mode);
	gameData.keeperLevel = static_cast<uint8_t>(currentSetup.campaignLevel);
	gameData.keeperRank = static_cast<uint8_t>(CampaignRank());
	gameData.keeperSeed = currentSetup.seed;

	GameData wire = gameData;
	wire.swapLE();
	std::vector<char> blob(sizeof(wire));
	std::memcpy(blob.data(), &wire, sizeof(wire));
	if (!currentSetup.mapFile.empty() && !AppendFile(currentSetup.mapFile, blob, MaxMapBytes)) {
		SDL_SetError("Cannot send this map: it is unreadable or larger than %d bytes.", static_cast<int>(MaxMapBytes));
		return false;
	}
	return SNetCreateGame(gameName, password, blob.data(), static_cast<int>(blob.size()), playerId);
}

void ApplyHostSetup(const GameData &gameData, const std::byte *mapBytes, size_t mapSize)
{
	joinedGame = true;
	hostRank = std::clamp<int>(gameData.keeperRank, 1, NumCampaignLevels);

	KeepSetup setup;
	setup.mode = gameData.keeperMode == static_cast<uint8_t>(GameMode::CustomMap) ? GameMode::CustomMap : GameMode::Campaign;
	setup.campaignLevel = std::clamp<int>(gameData.keeperLevel, 1, NumCampaignLevels);
	setup.seed = gameData.keeperSeed;
	if (mapSize > 0) {
		// The host's bytes, under a name of our own choosing: nothing from the network names a path.
		if (mapSize > MaxMapBytes)
			app_fatal("The host's map is too large.");
		setup.mapFile = ReceivedMapPath();
		FILE *file = OpenFile(setup.mapFile.c_str(), "wb");
		const bool written = file != nullptr && std::fwrite(mapBytes, 1, mapSize, file) == mapSize;
		if (file != nullptr)
			std::fclose(file);
		// Playing on without it would give this client a different keep from everyone else's.
		if (!written)
			app_fatal("Cannot save the host's map in the maps folder.");
	}
	currentSetup = setup;
}

std::string DescribeHostedGame(const GameData &gameData)
{
	if (gameData.keeperMode == static_cast<uint8_t>(GameMode::CustomMap))
		return fmt::format("Custom map, rank {}", gameData.keeperRank);
	return fmt::format("Level {}: {}, rank {}", gameData.keeperLevel, CampaignLevelRules(gameData.keeperLevel).name, gameData.keeperRank);
}

std::optional<KeepSetup> ContinueSetup(bool won)
{
	if (!won || currentSetup.mode != GameMode::Campaign)
		return currentSetup;
	if (currentSetup.campaignLevel >= NumCampaignLevels)
		return std::nullopt;
	return SetupForCampaignLevel(currentSetup.campaignLevel + 1);
}

void MarkRestartRequested()
{
	restartRequested = true;
}

bool TakeRestartRequest()
{
	if (!restartRequested)
		return false;
	restartRequested = false;
	if (gbIsMultiplayer) {
		pendingRematch.role = joinedGame ? RematchRole::Joiner : RematchRole::Host;
		pendingRematch.tickRate = sgGameInitInfo.nTickRate;
	}
	return true;
}

Rematch TakeRematch()
{
	const Rematch rematch = pendingRematch;
	pendingRematch = {};
	return rematch;
}

} // namespace devilution::keeper
