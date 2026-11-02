/**
 * @file keeper/keeper_profile.hpp
 *
 * The master's permanent progress: XP, keeper level, Renown, stats, and the monsters, spells and trap
 * types bought with Renown. Gold is for the current map; everything here lasts.
 *
 * Renown isn't stored: it is what the keeper level has earned minus the price of everything owned, so
 * a respec is simply clearing what is owned. In co-op a game uses the host's unlocks and levels, which
 * travel in GameData; each player still earns and keeps their own XP.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "keeper/keeper_game.hpp"

namespace devilution {

struct GameData;

namespace keeper {

struct ProfileStats {
	int mapsCleared = 0;
	int mapsLost = 0;
	int raidsSurvived = 0;
	int heroesSlain = 0;
	int monstersLost = 0;
};

// ---- the player's own profile (for the character screen) ------------------------------------

int ProfileXp();
int KeeperLevel();
/** Total XP at which `level` is reached. */
int XpForKeeperLevel(int level);
/** Renown left to spend. */
int RenownAvailable();
const ProfileStats &GetProfileStats();

bool OwnsKind(size_t kind);
/** 0 = not owned. */
int OwnedSpellLevel(size_t spell);
int OwnedTrapLevel(size_t trap);

/** Renown to unlock the item, or to raise it to its next level. */
int KindUnlockCost(size_t kind);
int SpellNextCost(size_t spell);
int TrapNextCost(size_t trap);

bool CanUnlockKind(size_t kind, std::string_view *reason = nullptr);
bool CanRaiseSpell(size_t spell, std::string_view *reason = nullptr);
bool CanRaiseTrap(size_t trap, std::string_view *reason = nullptr);
/** Each of these saves the profile if the purchase went through. */
bool UnlockKind(size_t kind);
bool RaiseSpell(size_t spell);
bool RaiseTrap(size_t trap);
/** Gives back every point of Renown; starting items stay. Free. */
void RespecProfile();

/** The spell bar, by slot: a spell, or -1. */
const std::array<int8_t, NumSpellSlots> &ProfileSpellBar();
/** Puts the spell on the bar or takes it off. @return false if the bar is full. */
bool ToggleProfileSpell(size_t spell);

/** Which first-map tips this master has seen, one bit each. */
unsigned ProfileTipsSeen();
void ProfileSetTipsSeen(unsigned seen);

// ---- what this game may use -----------------------------------------------------------------

/** The host's unlocks for a joiner, a test flag's rank, or this player's own. */
bool IsKindAvailable(size_t kind);
bool IsSpellAvailable(size_t spell);
bool IsTrapAvailable(size_t trap);
int GameSpellLevel(size_t spell);
int GameTrapLevel(size_t trap);

/** Host: writes its keeper level, unlocks and levels into the join data. */
void WriteHostUnlocks(GameData &gameData);
/** Joiner: uses the host's. */
void ReadHostUnlocks(const GameData &gameData);
/** A new network session starts: back to this player's own. */
void ForgetHostUnlocks();

// ---- earning XP ------------------------------------------------------------------------------

void ProfileBeginMap();
void ProfileNoteHeroSlain(int wave);
void ProfileNoteMonsterLost();
void ProfileNoteRaidSurvived(int wave);
/** Call before the level counts as cleared: the first clear pays best. */
void ProfileNoteVictory(int campaignLevel, bool firstClear);
void ProfileNoteDefeat();
/** XP earned on the current map. */
int MapXpEarned();
/** Writes the file, unless a test flag or an editor test run is in play. */
void SaveProfile();

} // namespace keeper
} // namespace devilution
