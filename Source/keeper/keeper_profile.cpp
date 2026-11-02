/**
 * @file keeper/keeper_profile.cpp
 *
 * Permanent progress. See keeper_profile.hpp.
 */
#include "keeper/keeper_profile.hpp"

#include <algorithm>
#include <bitset>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

#include <fmt/format.h>

#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_editor.hpp"
#include "keeper/keeper_roster.hpp"
#include "multi.h"
#include "utils/file_util.h"
#include "utils/log.hpp"
#include "utils/paths.h"

namespace devilution::keeper {

namespace {

// ---- balance -------------------------------------------------------------------------------

/** Reaching keeper level L takes XpPerLevelStep * L * (L - 1) / 2 XP in all. */
constexpr int XpPerLevelStep = 120;
constexpr int RenownPerLevel = 4;

constexpr int HeroSlainXpBase = 10;
constexpr int HeroSlainXpPerWave = 2;
constexpr int RaidXpBase = 25;
constexpr int RaidXpPerWave = 5;
constexpr int ClearXpBase = 150;
constexpr int ClearXpPerLevel = 25;
/** Replays still pay, but less, in percent. First clears of a campaign level pay in full. */
constexpr int ReplayFightPercent = 50;
constexpr int ReplayClearPercent = 25;
/** Each medal, the first time it's won on a level. Difficulty's XP share applies. */
constexpr int MedalXp = 40;
/** A profile made from older progress: how many times a level's clear bonus each cleared level is worth. */
constexpr int MigratedClearMultiple = 3;

/** Renown prices, from what an item's required keeper level is. */
constexpr int KindCostBase = 1;
constexpr int SpellCostBase = 2;
constexpr int LevelsPerExtraRenown = 4;
constexpr int RaiseCostBase = 1;
constexpr int LevelsPerExtraRaiseCost = 3;

// ---- the file ------------------------------------------------------------------------------

constexpr std::string_view ProfileFileName = "keeper_profile.txt";
constexpr std::string_view ProfileHeader = "DARKKEEP-PROFILE";
constexpr int ProfileVersion = 1;

struct Profile {
	int xp = 0;
	ProfileStats stats;
	std::bitset<NumLairKinds> kinds;
	std::array<uint8_t, NumSpellKinds> spells {};
	std::array<uint8_t, NumTrapKinds> traps {};
	std::array<int8_t, NumSpellSlots> spellBar {};
	unsigned tipsSeen = 0;
	Difficulty difficulty = Difficulty::Normal;
	std::array<uint8_t, NumCampaignLevels> medals {};
};

Profile profile;
bool loaded = false;
/** A profile from a newer build is never overwritten. */
bool writable = true;

/** The host's unlocks, while in someone else's game. */
bool usingHost = false;
std::bitset<NumLairKinds> hostKinds;
std::array<uint8_t, NumSpellKinds> hostSpells {};
std::array<uint8_t, NumTrapKinds> hostTraps {};

int mapXp = 0;
/** Where the master stood when the raid in play began, so that retrying it can give back what it paid. */
struct RaidStartMark {
	int xp;
	int mapXp;
	ProfileStats stats;
};
std::optional<RaidStartMark> raidStart;
bool mapIsReplay = false;

std::string ProfilePath()
{
	return paths::PrefPath() + std::string(ProfileFileName);
}

/** Whatever the required keeper level, level 1 items are what every keeper starts with. */
void GrantStarters(Profile &p)
{
	for (size_t kind = 0; kind < NumLairKinds; kind++) {
		if (IsKindOnRoster(kind) && UnlockRankOf(kind) <= 1)
			p.kinds.set(kind);
	}
	for (size_t spell = 0; spell < NumSpellKinds; spell++) {
		if (SpellKinds[spell].unlockRank <= 1)
			p.spells[spell] = std::max<uint8_t>(p.spells[spell], 1);
	}
	for (size_t trap = 0; trap < NumTrapKinds; trap++) {
		if (TrapKinds[trap].unlockRank <= 1)
			p.traps[trap] = std::max<uint8_t>(p.traps[trap], 1);
	}
}

void DefaultSpellBar(Profile &p)
{
	p.spellBar.fill(-1);
	size_t slot = 0;
	for (size_t spell = 0; spell < NumSpellKinds && slot < NumSpellSlots; spell++) {
		if (p.spells[spell] > 0)
			p.spellBar[slot++] = static_cast<int8_t>(spell);
	}
}

enum class ReadResult : uint8_t {
	Ok,
	Missing,
	Damaged,
	Newer,
};

ReadResult ReadProfile(const std::string &path, Profile &p)
{
	FILE *file = OpenFile(path.c_str(), "rb");
	if (file == nullptr)
		return ReadResult::Missing;
	char header[32] {};
	int version = 0;
	if (std::fscanf(file, "%31s %d", header, &version) != 2 || ProfileHeader != header) {
		std::fclose(file);
		LogError("Keeper profile {} is damaged", path);
		return ReadResult::Damaged;
	}
	if (version > ProfileVersion) {
		std::fclose(file);
		LogError("Keeper profile {} is from a newer version ({})", path, version);
		return ReadResult::Newer;
	}
	p.spellBar.fill(-1);
	size_t barSlot = 0;
	bool hasTips = false;
	char key[16] {};
	int a = 0;
	int b = 0;
	while (std::fscanf(file, "%15s %d %d", key, &a, &b) == 3) {
		const std::string_view name = key;
		if (name == "xp")
			p.xp = std::max(a, 0);
		else if (name == "stat") {
			int *const fields[] = { &p.stats.mapsCleared, &p.stats.mapsLost, &p.stats.raidsSurvived, &p.stats.heroesSlain, &p.stats.monstersLost };
			if (a >= 0 && a < static_cast<int>(std::size(fields)))
				*fields[a] = std::max(b, 0);
		} else if (name == "kind") {
			if (a >= 0 && static_cast<size_t>(a) < NumLairKinds)
				p.kinds.set(static_cast<size_t>(a));
		} else if (name == "spell") {
			if (a >= 0 && static_cast<size_t>(a) < NumSpellKinds)
				p.spells[static_cast<size_t>(a)] = static_cast<uint8_t>(std::clamp(b, 0, MaxKeepLevel));
		} else if (name == "trap") {
			if (a >= 0 && static_cast<size_t>(a) < NumTrapKinds)
				p.traps[static_cast<size_t>(a)] = static_cast<uint8_t>(std::clamp(b, 0, MaxKeepLevel));
		} else if (name == "bar") {
			if (a >= 0 && static_cast<size_t>(a) < NumSpellKinds && barSlot < NumSpellSlots)
				p.spellBar[barSlot++] = static_cast<int8_t>(a);
		} else if (name == "tips") {
			// Two halves: fscanf's %d stops short of a full 32 bits.
			p.tipsSeen = static_cast<unsigned>(a & 0xFFFF) | (static_cast<unsigned>(b & 0xFFFF) << 16);
			hasTips = true;
		} else if (name == "medal") {
			if (a >= 1 && a <= NumCampaignLevels)
				p.medals[static_cast<size_t>(a) - 1] = static_cast<uint8_t>(b);
		} else if (name == "difficulty") {
			if (a >= 0 && static_cast<size_t>(a) < NumDifficulties)
				p.difficulty = static_cast<Difficulty>(a);
		}
	}
	std::fclose(file);
	// A profile from before the tips, that has played: no need to teach this master the basics.
	if (!hasTips && (p.xp > 0 || p.stats.heroesSlain > 0))
		p.tipsSeen = ~0U;
	return ReadResult::Ok;
}

void Load()
{
	if (loaded)
		return;
	loaded = true;
	profile = {};
	const std::string path = ProfilePath();
	ReadResult result = ReadProfile(path, profile);
	// Saving removes the old file just before moving the new one in; survive a crash between the two.
	if (result == ReadResult::Missing)
		result = ReadProfile(path + ".tmp", profile);
	if (result == ReadResult::Ok) {
		GrantStarters(profile);
		return;
	}
	if (result == ReadResult::Newer)
		writable = false; // plays with a fresh profile, and leaves that one alone
	if (result == ReadResult::Damaged)
		CopyFileOverwrite(path.c_str(), (path + ".bad").c_str()); // kept for repair: the next save replaces it
	// A first run, or progress from before XP: every level already cleared counts.
	profile = {};
	GrantStarters(profile);
	DefaultSpellBar(profile);
	const int cleared = LevelsCleared();
	for (int level = 1; level <= cleared; level++)
		profile.xp += MigratedClearMultiple * (ClearXpBase + ClearXpPerLevel * level);
	profile.stats.mapsCleared = cleared;
}

int KindRenown(size_t kind)
{
	return KindCostBase + UnlockRankOf(kind) / LevelsPerExtraRenown;
}

int ItemRenown(int unlockRank)
{
	return SpellCostBase + unlockRank / LevelsPerExtraRenown;
}

/** Renown to go from `level` to `level + 1`. */
int RaiseRenown(int level)
{
	return RaiseCostBase + level / LevelsPerExtraRaiseCost;
}

/** Renown spent on an item at `level`. Items owned from the start cost nothing to own. */
int ItemSpent(int level, int unlockRank)
{
	if (level <= 0)
		return 0;
	int spent = unlockRank > 1 ? ItemRenown(unlockRank) : 0;
	for (int i = 1; i < level; i++)
		spent += RaiseRenown(i);
	return spent;
}

int RenownSpent()
{
	int spent = 0;
	for (size_t kind = 0; kind < NumLairKinds; kind++) {
		if (profile.kinds.test(kind) && UnlockRankOf(kind) > 1)
			spent += KindRenown(kind);
	}
	for (size_t spell = 0; spell < NumSpellKinds; spell++)
		spent += ItemSpent(profile.spells[spell], SpellKinds[spell].unlockRank);
	for (size_t trap = 0; trap < NumTrapKinds; trap++)
		spent += ItemSpent(profile.traps[trap], TrapKinds[trap].unlockRank);
	return spent;
}

bool Fail(std::string_view *reason, std::string_view why)
{
	if (reason != nullptr)
		*reason = why;
	return false;
}

/** The common rules for spells and traps. */
bool CanRaise(int level, int unlockRank, int cost, std::string_view *reason)
{
	Load();
	if (level >= MaxKeepLevel)
		return Fail(reason, "Already at the top level");
	if (level == 0 && KeeperLevel() < unlockRank)
		return Fail(reason, "Your keeper level is too low");
	if (RenownAvailable() < cost)
		return Fail(reason, "Not enough Renown");
	return true;
}

void AddXp(int amount)
{
	mapXp += amount;
	profile.xp += amount;
}

/** Test flags and editor test runs earn nothing. */
bool EarnsXp()
{
	return !IsTestRun() && !IsEditorTestRun();
}

} // namespace

int ProfileXp()
{
	Load();
	return profile.xp;
}

int XpForKeeperLevel(int level)
{
	return XpPerLevelStep * level * (level - 1) / 2;
}

int KeeperLevel()
{
	Load();
	int level = 1;
	while (profile.xp >= XpForKeeperLevel(level + 1))
		level++;
	return level;
}

int RenownAvailable()
{
	Load();
	return std::max(RenownPerLevel * (KeeperLevel() - 1) - RenownSpent(), 0);
}

const ProfileStats &GetProfileStats()
{
	Load();
	return profile.stats;
}

bool OwnsKind(size_t kind)
{
	Load();
	return kind < NumLairKinds && profile.kinds.test(kind);
}

int OwnedSpellLevel(size_t spell)
{
	Load();
	return spell < NumSpellKinds ? profile.spells[spell] : 0;
}

int OwnedTrapLevel(size_t trap)
{
	Load();
	return trap < NumTrapKinds ? profile.traps[trap] : 0;
}

int KindUnlockCost(size_t kind)
{
	return KindRenown(kind);
}

int SpellNextCost(size_t spell)
{
	const int level = OwnedSpellLevel(spell);
	return level == 0 ? ItemRenown(SpellKinds[spell].unlockRank) : RaiseRenown(level);
}

int TrapNextCost(size_t trap)
{
	const int level = OwnedTrapLevel(trap);
	return level == 0 ? ItemRenown(TrapKinds[trap].unlockRank) : RaiseRenown(level);
}

bool CanUnlockKind(size_t kind, std::string_view *reason)
{
	Load();
	if (kind >= NumLairKinds || !IsKindOnRoster(kind))
		return Fail(reason, "Not available");
	if (profile.kinds.test(kind))
		return Fail(reason, "Already unlocked");
	if (KeeperLevel() < UnlockRankOf(kind))
		return Fail(reason, "Your keeper level is too low");
	if (const std::optional<size_t> previous = PreviousTier(kind); previous && !profile.kinds.test(*previous))
		return Fail(reason, "Unlock the tier below it first");
	if (RenownAvailable() < KindUnlockCost(kind))
		return Fail(reason, "Not enough Renown");
	return true;
}

bool CanRaiseSpell(size_t spell, std::string_view *reason)
{
	return CanRaise(OwnedSpellLevel(spell), SpellKinds[spell].unlockRank, SpellNextCost(spell), reason);
}

bool CanRaiseTrap(size_t trap, std::string_view *reason)
{
	return CanRaise(OwnedTrapLevel(trap), TrapKinds[trap].unlockRank, TrapNextCost(trap), reason);
}

bool UnlockKind(size_t kind)
{
	if (!CanUnlockKind(kind))
		return false;
	profile.kinds.set(kind);
	SaveProfile();
	return true;
}

bool RaiseSpell(size_t spell)
{
	if (!CanRaiseSpell(spell))
		return false;
	profile.spells[spell]++;
	SaveProfile();
	return true;
}

bool RaiseTrap(size_t trap)
{
	if (!CanRaiseTrap(trap))
		return false;
	profile.traps[trap]++;
	SaveProfile();
	return true;
}

void RespecProfile()
{
	Load();
	profile.kinds.reset();
	profile.spells.fill(0);
	profile.traps.fill(0);
	GrantStarters(profile);
	for (int8_t &slot : profile.spellBar) {
		if (slot >= 0 && profile.spells[static_cast<size_t>(slot)] == 0)
			slot = -1;
	}
	SaveProfile();
}

Difficulty ProfileDifficulty()
{
	Load();
	return profile.difficulty;
}

void CycleProfileDifficulty()
{
	Load();
	profile.difficulty = static_cast<Difficulty>((static_cast<size_t>(profile.difficulty) + 1) % NumDifficulties);
	SaveProfile();
}

const std::array<int8_t, NumSpellSlots> &ProfileSpellBar()
{
	Load();
	return profile.spellBar;
}

bool ToggleProfileSpell(size_t spell)
{
	Load();
	for (int8_t &slot : profile.spellBar) {
		if (slot == static_cast<int8_t>(spell)) {
			slot = -1;
			SaveProfile();
			return true;
		}
	}
	for (int8_t &slot : profile.spellBar) {
		if (slot < 0) {
			slot = static_cast<int8_t>(spell);
			SaveProfile();
			return true;
		}
	}
	return false;
}

unsigned ProfileTipsSeen()
{
	Load();
	return profile.tipsSeen;
}

void ProfileSetTipsSeen(unsigned seen)
{
	Load();
	if (profile.tipsSeen == seen)
		return;
	profile.tipsSeen = seen;
	SaveProfile();
}

bool IsKindAvailable(size_t kind)
{
	if (kind >= NumLairKinds)
		return false;
	if (const int rank = OverrideRank(); rank > 0)
		return UnlockRankOf(kind) <= rank;
	if (usingHost)
		return hostKinds.test(kind);
	return OwnsKind(kind);
}

bool IsSpellAvailable(size_t spell)
{
	return GameSpellLevel(spell) > 0;
}

bool IsTrapAvailable(size_t trap)
{
	return GameTrapLevel(trap) > 0;
}

int GameSpellLevel(size_t spell)
{
	if (spell >= NumSpellKinds)
		return 0;
	if (const int rank = OverrideRank(); rank > 0)
		return SpellKinds[spell].unlockRank <= rank ? std::max(OwnedSpellLevel(spell), 1) : 0;
	return usingHost ? hostSpells[spell] : OwnedSpellLevel(spell);
}

int GameTrapLevel(size_t trap)
{
	if (trap >= NumTrapKinds)
		return 0;
	if (const int rank = OverrideRank(); rank > 0)
		return TrapKinds[trap].unlockRank <= rank ? std::max(OwnedTrapLevel(trap), 1) : 0;
	return usingHost ? hostTraps[trap] : OwnedTrapLevel(trap);
}

void WriteHostUnlocks(GameData &gameData)
{
	static_assert(sizeof(gameData.keeperKinds) * 8 >= NumLairKinds);
	static_assert(sizeof(gameData.keeperSpells) + 1 == NumSpellKinds);
	static_assert(sizeof(gameData.keeperTraps) == NumTrapKinds);
	gameData.keeperRank = static_cast<uint8_t>(std::min(KeeperLevel(), 255));
	std::memset(gameData.keeperKinds, 0, sizeof(gameData.keeperKinds));
	for (size_t kind = 0; kind < NumLairKinds; kind++) {
		if (IsKindAvailable(kind))
			gameData.keeperKinds[kind / 8] |= static_cast<uint8_t>(1U << (kind % 8));
	}
	for (size_t spell = 0; spell < sizeof(gameData.keeperSpells); spell++)
		gameData.keeperSpells[spell] = static_cast<uint8_t>(GameSpellLevel(spell));
	gameData.keeperLastSpell = static_cast<uint8_t>(GameSpellLevel(GolemSpellKind));
	for (size_t trap = 0; trap < NumTrapKinds; trap++)
		gameData.keeperTraps[trap] = static_cast<uint8_t>(GameTrapLevel(trap));
}

void ReadHostUnlocks(const GameData &gameData)
{
	usingHost = true;
	hostKinds.reset();
	for (size_t kind = 0; kind < NumLairKinds; kind++) {
		if ((gameData.keeperKinds[kind / 8] & (1U << (kind % 8))) != 0)
			hostKinds.set(kind);
	}
	for (size_t spell = 0; spell < sizeof(gameData.keeperSpells); spell++)
		hostSpells[spell] = std::min<uint8_t>(gameData.keeperSpells[spell], MaxKeepLevel);
	hostSpells[GolemSpellKind] = std::min<uint8_t>(gameData.keeperLastSpell, MaxKeepLevel);
	for (size_t trap = 0; trap < NumTrapKinds; trap++)
		hostTraps[trap] = std::min<uint8_t>(gameData.keeperTraps[trap], MaxKeepLevel);
}

void ForgetHostUnlocks()
{
	usingHost = false;
}

void ProfileBeginMap()
{
	Load();
	mapXp = 0;
	raidStart.reset();
	const KeepSetup &setup = CurrentSetup();
	mapIsReplay = setup.mode != GameMode::Campaign || IsLevelCredited(setup.campaignLevel);
}

void ProfileMarkRaidStart()
{
	Load();
	raidStart = RaidStartMark { profile.xp, mapXp, profile.stats };
}

void ProfileTakeBackRaid()
{
	if (!raidStart || !EarnsXp())
		return;
	Load();
	profile.xp = raidStart->xp;
	mapXp = raidStart->mapXp;
	profile.stats = raidStart->stats;
	raidStart.reset();
	SaveProfile();
}

void ProfileNoteHeroSlain(int wave)
{
	if (!EarnsXp())
		return;
	Load();
	profile.stats.heroesSlain++;
	const int xp = (HeroSlainXpBase + HeroSlainXpPerWave * wave) * DifficultyXpPercent() / 100 * PartyXpPercent() / 100;
	AddXp(mapIsReplay ? xp * ReplayFightPercent / 100 : xp);
}

void ProfileNoteMonsterLost()
{
	if (!EarnsXp())
		return;
	Load();
	profile.stats.monstersLost++;
}

void ProfileNoteRaidSurvived(int wave)
{
	if (!EarnsXp())
		return;
	Load();
	profile.stats.raidsSurvived++;
	const int xp = (RaidXpBase + RaidXpPerWave * wave) * DifficultyXpPercent() / 100;
	AddXp(mapIsReplay ? xp * ReplayFightPercent / 100 : xp);
	SaveProfile();
}

void ProfileNoteVictory(int campaignLevel, bool firstClear)
{
	if (!EarnsXp())
		return;
	Load();
	profile.stats.mapsCleared++;
	const int xp = (ClearXpBase + ClearXpPerLevel * campaignLevel) * DifficultyXpPercent() / 100;
	AddXp(firstClear ? xp : xp * ReplayClearPercent / 100);
	SaveProfile();
}

void ProfileNoteDefeat()
{
	if (!EarnsXp())
		return;
	Load();
	profile.stats.mapsLost++;
	SaveProfile();
}

void ProfileSetMapXp(int xp)
{
	mapXp = xp;
}

void ProfileAwardMedals(int campaignLevel, uint8_t medals)
{
	if (!EarnsXp() || campaignLevel < 1 || campaignLevel > NumCampaignLevels)
		return;
	Load();
	uint8_t &held = profile.medals[static_cast<size_t>(campaignLevel) - 1];
	const auto fresh = static_cast<uint8_t>(medals & ~held);
	if (fresh == 0)
		return;
	held |= fresh;
	int count = 0;
	for (uint8_t bits = fresh; bits != 0; bits >>= 1)
		count += bits & 1;
	AddXp(count * MedalXp * DifficultyXpPercent() / 100);
	SaveProfile();
}

uint8_t ProfileMedals(int campaignLevel)
{
	Load();
	return campaignLevel >= 1 && campaignLevel <= NumCampaignLevels ? profile.medals[static_cast<size_t>(campaignLevel) - 1] : 0;
}

int ProfileMedalCount()
{
	Load();
	int count = 0;
	for (uint8_t bits : profile.medals) {
		for (; bits != 0; bits >>= 1)
			count += bits & 1;
	}
	return count;
}

int MapXpEarned()
{
	return mapXp;
}

void SaveProfile()
{
	if (!EarnsXp() || !loaded || !writable)
		return;
	const std::string path = ProfilePath();
	const std::string temp = path + ".tmp";
	FILE *file = OpenFile(temp.c_str(), "wb");
	if (file == nullptr) {
		LogError("Cannot write keeper profile {}", temp);
		return;
	}
	std::string text = fmt::format("{} {}\nxp {} 0\n", ProfileHeader, ProfileVersion, profile.xp);
	const int stats[] = { profile.stats.mapsCleared, profile.stats.mapsLost, profile.stats.raidsSurvived, profile.stats.heroesSlain, profile.stats.monstersLost };
	for (size_t i = 0; i < std::size(stats); i++)
		text += fmt::format("stat {} {}\n", i, stats[i]);
	for (size_t kind = 0; kind < NumLairKinds; kind++) {
		if (profile.kinds.test(kind))
			text += fmt::format("kind {} 1\n", kind);
	}
	for (size_t spell = 0; spell < NumSpellKinds; spell++) {
		if (profile.spells[spell] > 0)
			text += fmt::format("spell {} {}\n", spell, profile.spells[spell]);
	}
	for (size_t trap = 0; trap < NumTrapKinds; trap++) {
		if (profile.traps[trap] > 0)
			text += fmt::format("trap {} {}\n", trap, profile.traps[trap]);
	}
	for (const int8_t slot : profile.spellBar) {
		if (slot >= 0)
			text += fmt::format("bar {} 1\n", slot);
	}
	text += fmt::format("tips {} {}\n", profile.tipsSeen & 0xFFFF, profile.tipsSeen >> 16);
	text += fmt::format("difficulty {} 0\n", static_cast<int>(profile.difficulty));
	for (size_t level = 0; level < profile.medals.size(); level++) {
		if (profile.medals[level] != 0)
			text += fmt::format("medal {} {}\n", level + 1, profile.medals[level]);
	}
	const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
	const bool closed = std::fclose(file) == 0;
	if (!written || !closed) {
		LogError("Cannot write keeper profile {}", temp);
		return;
	}
	// The engine's RenameFile will not replace an existing file on Windows.
	if (FileExists(path))
		RemoveFile(path.c_str());
	RenameFile(temp.c_str(), path.c_str());
}

} // namespace devilution::keeper
