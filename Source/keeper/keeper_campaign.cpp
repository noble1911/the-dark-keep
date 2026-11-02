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
#include "config.h"
#include "keeper_build_id.h"
#include "keeper/keeper.hpp"
#include "keeper/keeper_map.hpp"
#include "keeper/keeper_profile.hpp"
#include "keeper/keeper_upgrades.hpp"
#include "multi.h"
#include "storm/storm_net.hpp"
#include "utils/endian_read.hpp"
#include "utils/endian_write.hpp"
#include "utils/file_util.h"
#include "utils/log.hpp"
#include "utils/paths.h"

namespace devilution::keeper {

namespace {

// clang-format off
/**
 * The campaign, easiest first. Balance is tuned here (docs/BALANCE.md). Level 1's seed is the keep's original layout.
 * Columns: name, raids, starting gold, hero strength %, first party, party growth (1/100 hero per raid), seed.
 */
constexpr std::array<LevelRules, NumCampaignLevels> CampaignLevels { {
	{ "Old Cellars",       5, 1500, 100, 2,  80, KeepLevelSeed },
	{ "Rat Warrens",       5, 1500, 105, 2,  80, 0x1F3A97C4 },
	{ "Sunken Chapel",     5, 1500, 110, 2,  85, 0xA4C21E0B },
	{ "Bone Pits",         6, 1500, 115, 2,  85, 0x57D0E36A },
	{ "Black Vaults",      6, 1500, 120, 2,  90, 0xC93B7721 },
	{ "Weeping Halls",     6, 1500, 125, 3,  80, 0x2E86F15D },
	{ "Ashen Crypts",      7, 1600, 130, 3,  80, 0x7BA4096E },
	{ "Iron Gaol",         7, 1700, 135, 3,  85, 0xE15C38D2 },
	{ "Maggot Deeps",      7, 1800, 140, 3,  90, 0x39F7A640 },
	{ "Hollow Court",      8, 1950, 140, 3,  90, 0x94D2C1B7 },
	{ "Blood Galleries",   8, 2100, 140, 3,  95, 0x0C6E5FA9 },
	{ "Shrouded Nave",     8, 2250, 145, 4,  85, 0xD7318B4C },
	{ "Gallows Reach",     8, 2400, 150, 4,  90, 0x63AF02E5 },
	{ "Cinder Keep",       9, 2600, 155, 4,  90, 0xB8E97D13 },
	{ "Howling Stair",     9, 2800, 160, 4,  95, 0x4A15C6F8 },
	{ "Tomb of Kings",     9, 3000, 165, 4, 100, 0xF20D93A6 },
	{ "Silent Abbey",      9, 3200, 170, 5,  90, 0x8C7B24D1 },
	{ "Chains of Night",  10, 3500, 180, 5,  90, 0x26E85B3F },
	{ "Burning Sepulcre", 10, 3800, 190, 5,  95, 0xAD4F1C72 },
	{ "Throne of Ash",    10, 4200, 215, 6, 100, 0x71C6E09B },
} };

/** Custom maps and the editor have no place in the curve: these are the keep's original rules. */
constexpr LevelRules CustomRules { "Custom Map", 7, 1500, 100, 2, 80, KeepLevelSeed };
// clang-format on

/**
 * How much of a level's strength above 100% reaches armour, in percent. Monster missiles and traps hit on
 * 90 - armour, so at the full strength every ranged monster and trap was down to its 5% floor by level 15.
 */
constexpr int HeroArmorStrengthPercent = 50;

/** However a level's numbers are tuned, a raid never brings more heroes than this. */
constexpr int MaxPartySize = 14;

/** Column order: name, hero fighting strength %, XP %, blurb. Prices, tribute and bounties don't change, so Hard is simply harder. */
struct DifficultyRules {
	std::string_view name;
	int strengthPercent;
	int xpPercent;
	std::string_view blurb;
};
constexpr std::array<DifficultyRules, NumDifficulties> DifficultyTable { {
	{ "Easy", 80, 75, "Heroes hit less hard and fall sooner. 75% XP." },
	{ "Normal", 100, 100, "The keep as it was meant to be held." },
	{ "Hard", 125, 125, "Heroes hit harder and last longer. 125% XP." },
} };

/**
 * Co-op monsters have twice single-player life (keeper_roster.cpp) and each master brings spells, so parties grow
 * with the masters: `strengthPercent` on their life and damage, `partyPercent` on their number. Starting points
 * from the square law in docs/BALANCE.md (party power goes with strength^2 * size^2); never played.
 */
struct MasterScale {
	int strengthPercent;
	int partyPercent;
};
constexpr MasterScale SoloScale { 100, 100 };
/** By number of masters in a network game, from 1: alone in one, a master still gets the doubled monsters. */
constexpr std::array<MasterScale, 4> NetworkScale { {
	{ 140, 100 },
	{ 125, 125 },
	{ 130, 140 },
	{ 135, 150 },
} };

const MasterScale &CurrentMasterScale()
{
	return gbIsMultiplayer ? NetworkScale[std::clamp(RaidMasters(), 1, static_cast<int>(NetworkScale.size())) - 1] : SoloScale;
}

constexpr std::string_view ProgressFileName = "campaign_progress.txt";
constexpr std::string_view ProgressHeader = "DARKKEEP-CAMPAIGN";
constexpr int ProgressVersion = 1;

KeepSetup currentSetup;
Difficulty gameDifficulty = Difficulty::Normal;
int raidMasters = 1;

/** From --keeper-level, so a test of a late level has the units it is meant to be played with. */
int devRank = 0;

bool joinedGame = false;
std::optional<KeepSetup> postMapSetup;

bool restartRequested = false;
Rematch pendingRematch;

bool progressLoaded = false;
/** A progress file from a newer build is left alone rather than overwritten. */
bool progressWritable = true;
int levelsCleared = 0;
/** Levels past the next open one that this master has won in someone else's game (bit 0 = level 1). */
uint32_t levelsWonAhead = 0;

std::string ProgressPath()
{
	return paths::PrefPath() + std::string(ProgressFileName);
}

bool ReadProgressFile(const std::string &path, int &cleared, uint32_t &wonAhead)
{
	FILE *file = OpenFile(path.c_str(), "rb");
	if (file == nullptr)
		return false;
	char header[32] {};
	int version = 0;
	int value = 0;
	unsigned ahead = 0; // not in files from before co-op credit, and older builds ignore it
	const int fields = std::fscanf(file, "%31s %d %d %u", header, &version, &value, &ahead);
	std::fclose(file);
	if (fields < 3 || ProgressHeader != header) {
		LogError("Ignoring unreadable campaign progress file {}", path);
		return false;
	}
	if (version > ProgressVersion) {
		LogError("Campaign progress file {} is from a newer version ({})", path, version);
		progressWritable = false;
		return false;
	}
	cleared = std::clamp(value, 0, NumCampaignLevels);
	wonAhead = fields == 4 ? ahead : 0;
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
	if (!ReadProgressFile(path, levelsCleared, levelsWonAhead) && progressWritable)
		ReadProgressFile(path + ".tmp", levelsCleared, levelsWonAhead);
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
	const std::string text = fmt::format("{} {} {} {}\n", ProgressHeader, ProgressVersion, levelsCleared, levelsWonAhead);
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

/** Appends the map to `out`. @return false if it cannot be read or is larger than `maxBytes`. */
bool AppendMapFile(const std::string &path, std::vector<char> &out, size_t maxBytes)
{
	const tl::expected<std::vector<std::byte>, std::string> bytes = ReadMapBytes(path);
	if (!bytes.has_value() || bytes->size() > maxBytes || !CheckMapBytes(bytes->data(), bytes->size()).has_value())
		return false; // a joiner would refuse it, and be thrown out of the game
	const size_t start = out.size();
	out.resize(start + bytes->size());
	if (!bytes->empty())
		std::memcpy(out.data() + start, bytes->data(), bytes->size());
	return true;
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

int OverrideRank()
{
	return std::clamp(TestRank > 0 ? TestRank : devRank, 0, NumCampaignLevels);
}

void SetPostMapSetup(const KeepSetup &setup)
{
	postMapSetup = setup;
}

std::optional<KeepSetup> TakePostMapSetup()
{
	std::optional<KeepSetup> setup = postMapSetup;
	postMapSetup.reset();
	return setup;
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

std::string_view DifficultyName(Difficulty difficulty)
{
	return DifficultyTable[static_cast<size_t>(difficulty)].name;
}

std::string_view DifficultyBlurb(Difficulty difficulty)
{
	return DifficultyTable[static_cast<size_t>(difficulty)].blurb;
}

Difficulty GameDifficulty()
{
	return gameDifficulty;
}

void SetGameDifficulty(Difficulty difficulty)
{
	gameDifficulty = static_cast<size_t>(difficulty) < NumDifficulties ? difficulty : Difficulty::Normal;
}

void LatchGameDifficulty()
{
	if (!joinedGame)
		SetGameDifficulty(TestDifficulty >= 0 ? static_cast<Difficulty>(TestDifficulty) : ProfileDifficulty());
}

int RaidMasters()
{
	return raidMasters;
}

void SetRaidMasters(int masters)
{
	raidMasters = std::max(masters, 1);
}

int HeroCombatPercent()
{
	return HeroStrengthPercent() * DifficultyTable[static_cast<size_t>(gameDifficulty)].strengthPercent / 100 * CurrentMasterScale().strengthPercent / 100;
}

int ScaleHeroCombat(int value)
{
	return value * HeroCombatPercent() / 100;
}

int ScaleHeroArmor(int value)
{
	return value * (100 + (HeroCombatPercent() - 100) * HeroArmorStrengthPercent / 100) / 100;
}

int DifficultyXpPercent()
{
	return DifficultyTable[static_cast<size_t>(gameDifficulty)].xpPercent;
}

int PartyXpPercent()
{
	return 10000 / CurrentMasterScale().partyPercent;
}

int PartySizeForWave(int wave)
{
	const LevelRules &rules = CurrentRules();
	const int size = rules.startingParty + rules.partyGrowthPercent * (std::max(wave, 1) - 1) / 100;
	return std::clamp((size * CurrentMasterScale().partyPercent + 50) / 100, 1, MaxPartySize);
}

KeepSetup SetupForCampaignLevel(int level)
{
	KeepSetup setup;
	setup.mode = GameMode::Campaign;
	setup.campaignLevel = std::clamp(level, 1, NumCampaignLevels);
	setup.seed = CampaignLevelRules(setup.campaignLevel).seed;
	// An asset name rather than a path: it's a loose file or inside devilutionx.mpq, depending on the
	// build. Without the map's tileset in the game data (shareware) the level stays a random Cathedral.
	std::string map = CampaignMapAsset(setup.campaignLevel);
	if (CanPlayMap(map))
		setup.mapFile = std::move(map);
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
	if (currentSetup.mode == GameMode::MapEditor) { // --keeper-editor
		currentSetup = SetupForMapEditor();
		return;
	}
	if (!TestCustomMap.empty()) {
		SetCurrentSetup(SetupForCustomMap(TestCustomMap));
		return;
	}
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

std::vector<std::string> ListMapFiles(bool includeScratch)
{
	constexpr std::string_view Extension = ".dkmap";
	std::vector<std::string> maps;
	for (std::string &file : ListFiles(MapsFolder().c_str())) {
		// The editor's autosave and test-run files start with an underscore.
		if (file.size() <= Extension.size() || (!includeScratch && file[0] == '_'))
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

int NextOpenLevel()
{
	return std::min(LevelsCleared() + 1, NumCampaignLevels);
}

bool IsLevelCredited(int level)
{
	LoadProgress();
	return level <= levelsCleared || (levelsWonAhead & (1U << (level - 1))) != 0;
}

bool IsTestRun()
{
	return OverrideRank() != 0 || TestStartWave > 0 || TestLordHitPoints > 0;
}

void NoteVictory()
{
	const bool campaign = currentSetup.mode == GameMode::Campaign;
	const int level = currentSetup.campaignLevel;
	ProfileNoteVictory(campaign ? level : 1, campaign && !IsLevelCredited(level));
	if (!campaign || IsTestRun() || IsLevelCredited(level))
		return;
	// A master unlocks levels in order, whoever's game they win them in. A level won further on, in a
	// host's game, is remembered and counts as soon as the levels before it have been cleared.
	levelsWonAhead |= 1U << (level - 1);
	while (levelsCleared < NumCampaignLevels && (levelsWonAhead & (1U << levelsCleared)) != 0) {
		levelsWonAhead &= ~(1U << levelsCleared);
		levelsCleared++;
	}
	SaveProgress();
}

void BeginSession()
{
	joinedGame = false;
	ForgetHostUnlocks();
	restartRequested = false;
}

bool IsJoinedGame()
{
	return joinedGame;
}

uint32_t BuildStamp()
{
	// The AI is simulated on every client, so different code drifts apart. The commit says so, and the same
	// commit is the same code on any platform or build type: Windows and Switch builds play together.
	uint32_t hash = 2166136261U;
	const auto add = [&](uint32_t value) {
		for (int shift = 0; shift < 32; shift += 8)
			hash = (hash ^ ((value >> shift) & 0xFF)) * 16777619U;
	};
	for (const char c : std::string_view(KEEPER_BUILD_ID))
		add(static_cast<unsigned char>(c));
	add(PROJECT_VERSION_MAJOR);
	add(PROJECT_VERSION_MINOR);
	add(PROJECT_VERSION_PATCH);
	add(KeeperProtocolVersion);
	add(NumLairKinds);
	add(NumSpellKinds);
	add(NumTrapKinds);
	add(NumUpgrades);
	return hash + static_cast<uint32_t>(TestBuildStamp);
}

bool IsSameBuild(const GameData &gameData)
{
	return LoadLE32(gameData.reserved) == BuildStamp();
}

bool HostGame(GameData &gameData, const char *gameName, const char *password, int *playerId)
{
	gameData.nDifficulty = DIFF_NORMAL; // how hard a game is comes from the campaign
	LatchGameDifficulty();
	gameData.keeperReserved = static_cast<uint8_t>(GameDifficulty());
	gameData.keeperMode = static_cast<uint8_t>(currentSetup.mode);
	gameData.keeperLevel = static_cast<uint8_t>(currentSetup.campaignLevel);
	WriteHostUnlocks(gameData);
	gameData.keeperSeed = currentSetup.seed;
	WriteLE32(gameData.reserved, BuildStamp());

	GameData wire = gameData;
	wire.swapLE();
	std::vector<char> blob(sizeof(wire));
	std::memcpy(blob.data(), &wire, sizeof(wire));
	if (!currentSetup.mapFile.empty() && !AppendMapFile(currentSetup.mapFile, blob, MaxMapBytes)) {
		SDL_SetError("Cannot send this map: it is unreadable, unfinished or larger than %d bytes.", static_cast<int>(MaxMapBytes));
		return false;
	}
	return SNetCreateGame(gameName, password, blob.data(), static_cast<int>(blob.size()), playerId);
}

void ApplyHostSetup(const GameData &gameData, const std::byte *mapBytes, size_t mapSize)
{
	joinedGame = true;
	ReadHostUnlocks(gameData);
	SetGameDifficulty(static_cast<Difficulty>(gameData.keeperReserved));

	KeepSetup setup;
	setup.mode = gameData.keeperMode == static_cast<uint8_t>(GameMode::CustomMap) ? GameMode::CustomMap : GameMode::Campaign;
	setup.campaignLevel = std::clamp<int>(gameData.keeperLevel, 1, NumCampaignLevels);
	setup.seed = gameData.keeperSeed;
	if (mapSize > 0) {
		// The host's bytes, under a name of our own choosing: nothing from the network names a path.
		if (mapSize > MaxMapBytes)
			app_fatal("The host's map is too large.");
		// It's data from another player, and carrying on without it would be a different keep from theirs.
		if (const tl::expected<void, std::string> usable = CheckMapBytes(mapBytes, mapSize); !usable.has_value())
			app_fatal(fmt::format("The host's map can't be used: {}.", usable.error()));
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
	const std::string_view difficulty = gameData.keeperReserved < NumDifficulties && gameData.keeperReserved != static_cast<uint8_t>(Difficulty::Normal)
	    ? DifficultyName(static_cast<Difficulty>(gameData.keeperReserved))
	    : "";
	const std::string extra = difficulty.empty() ? std::string() : fmt::format(", {}", difficulty);
	if (gameData.keeperMode == static_cast<uint8_t>(GameMode::CustomMap))
		return fmt::format("Custom map, keeper level {}{}", gameData.keeperRank, extra);
	return fmt::format("Level {}: {}, keeper level {}{}", gameData.keeperLevel, CampaignLevelRules(gameData.keeperLevel).name, gameData.keeperRank, extra);
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
