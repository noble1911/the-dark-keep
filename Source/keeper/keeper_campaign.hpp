/**
 * @file keeper/keeper_campaign.hpp
 *
 * What game is being played: campaign level or custom map, and the master's saved progress.
 * This is the contract the roster, upgrades, map and menu code share (see docs/NEXT-PHASE.md).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace devilution {

struct GameData;

namespace keeper {

constexpr int NumCampaignLevels = 20;

enum class GameMode : uint8_t {
	Campaign,
	CustomMap,
	MapEditor,
};

/** Chosen in the menus before the keep level loads. In co-op the host's setup is used by everyone. */
struct KeepSetup {
	GameMode mode = GameMode::Campaign;
	/** 1-based campaign level (ignored for custom maps). */
	int campaignLevel = 1;
	/** Seed for the random keep layout, when no map file is used. */
	uint32_t seed = 0;
	/** Map to load instead of a random layout, as a full path on this machine. Empty: use `seed`. */
	std::string mapFile;
};

const KeepSetup &CurrentSetup();
void SetCurrentSetup(const KeepSetup &setup);

/** Test flags: everything up to this keeper level counts as unlocked (--keeper-rank, --keeper-level). 0 = play normally. */
int OverrideRank();

// ---- level rules -------------------------------------------------------------------------

/** How hard a level is. Every level's numbers live in one table in keeper_campaign.cpp. */
struct LevelRules {
	std::string_view name;
	/** Raids to survive; the last one is the champions' raid. */
	int raids;
	int startingGold;
	/** Hero hit points and damage, in percent of the normal strength for the raid. */
	int heroStrengthPercent;
	/** Heroes in the first raid. */
	int startingParty;
	/** Heroes added per raid, in hundredths (150 = three more every two raids). */
	int partyGrowthPercent;
	uint32_t seed;
};

/** Rules of campaign level `level` (1-based, clamped). */
const LevelRules &CampaignLevelRules(int level);
/** The fixed rules custom maps and the editor share. */
const LevelRules &CustomMapRules();
/** Rules of the game being played. */
const LevelRules &CurrentRules();

/** Number of raids to survive this game; the last one is the champions' raid. */
int NumWaves();
int StartingGold();
int HeroStrengthPercent();
/** 1-based number of the level being played; custom maps and the editor count as level 1. */
int CurrentLevelNumber();
/** Scales a hero stat by HeroStrengthPercent(). */
int ScaleByHeroStrength(int value);
/** How many heroes raid `wave` brings. */
int PartySizeForWave(int wave);

// ---- choosing a game ---------------------------------------------------------------------

/** Uses assets/keeper/campaign/level_NN.dkmap as the map file when that file exists. */
KeepSetup SetupForCampaignLevel(int level);
KeepSetup SetupForCustomMap(std::string mapFile);
KeepSetup SetupForMapEditor();
/** --keeper-autostart / --keeper-host: the level from --keeper-level, or level 1. */
void ApplyAutoStartSetup();

/** The maps folder in the save directory (with trailing separator); created if missing. */
std::string MapsFolder();
/** File names of the *.dkmap files in MapsFolder(), sorted. `includeScratch`: also the editor's autosave and test files. */
std::vector<std::string> ListMapFiles(bool includeScratch);

// ---- progress ----------------------------------------------------------------------------

/** Campaign levels this machine's master has cleared, from the progress file. */
int LevelsCleared();
/** The furthest campaign level open to this machine's master: LevelsCleared() + 1, capped at 20. */
int NextOpenLevel();
/** The keep stands. Saves progress when this client's own campaign game cleared its next level. */
void NoteVictory();

// ---- co-op -------------------------------------------------------------------------------

/** A new network session starts: forget anything received from a previous host. */
void BeginSession();
/** True if this client joined someone else's game, so the setup and rank are the host's. */
bool IsJoinedGame();
/**
 * Creates the network game with the current setup. The setup, rank and map file travel in the
 * game's init blob: a GameData followed by the map's bytes. @return false with an SDL error set.
 */
bool HostGame(GameData &gameData, const char *gameName, const char *password, int *playerId);
/** A joiner received the host's init blob: adopts the setup and rank, and stores the map. */
void ApplyHostSetup(const GameData &gameData, const std::byte *mapBytes, size_t mapSize);
/** One line about a hosted game, for the public game list. */
std::string DescribeHostedGame(const GameData &gameData);

// ---- after the siege ---------------------------------------------------------------------

/**
 * What the host's "continue" choice on the game-over screen starts: the next level, the same
 * level or map again after a defeat, or nothing after the campaign's last level.
 */
std::optional<KeepSetup> ContinueSetup(bool won);

/** Single player chose what comes next: it leaves through the character screen and starts `setup` after it. */
void SetPostMapSetup(const KeepSetup &setup);
/** Consumed by the main menu loop. */
std::optional<KeepSetup> TakePostMapSetup();

/** Every client got the host's restart command: leave this game at the next tick. */
void MarkRestartRequested();
/** Consumed by keeper::Tick. Remembers how to get back into the next game. */
bool TakeRestartRequest();

enum class RematchRole : uint8_t {
	None,
	Host,
	Joiner,
};
struct Rematch {
	RematchRole role = RematchRole::None;
	/** The host's game speed, kept for the next game. */
	uint8_t tickRate = 20;
};
/** Consumed by InitMulti: reconnect without the menus because a next level or retry was chosen. */
Rematch TakeRematch();

} // namespace keeper
} // namespace devilution
