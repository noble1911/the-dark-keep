/**
 * @file keeper/keeper_upgrades.hpp
 *
 * Upgrades the masters buy with vault gold between raids. They apply by category (lair monsters,
 * traps, spells, the Keep Lord) and last for the current game only.
 *
 * The host decides every purchase and deducts the gold; every client holds the same tiers and
 * derives the same stats from them. Tiers only ever rise within a game, so hearing about one twice
 * (the purchase message and the state snapshot) is harmless.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "tables/spelldat.h"

namespace devilution {

struct Monster;

namespace keeper {

/** Sent over the network and used as a card index: add new upgrades at the end. */
enum class UpgradeId : uint8_t {
	MonsterHealth,
	MonsterDamage,
	MonsterArmor,
	ManaRegen,
	LordHealth,
	LordDamage,
};
constexpr size_t NumUpgrades = 6;

struct UpgradeInfo {
	/** Short enough for a card. */
	std::string_view cardName;
	std::string_view title;
	/** How the effect reads on the card, after the amount: "+15" + "% life for ...". */
	std::string_view effectSign;
	std::string_view effectUnit;
	SpellID icon;
	int maxTier;
	/** Cost of the first tier; each later tier costs costStep more. */
	int baseCost;
	int costStep;
	/** Effect of one tier, in the unit the upgrade works in (see effectUnit). */
	int effectPerTier;
};

const UpgradeInfo &GetUpgradeInfo(UpgradeId id);

int UpgradeTier(UpgradeId id);
/** All tiers, as carried by the state snapshot. */
const std::array<uint8_t, NumUpgrades> &UpgradeTiers();
/** Gold for the next tier (0 = already at the top tier). */
int UpgradeNextCost(UpgradeId id);
/** The card's description: what the upgrade does and how far along it is. */
std::string UpgradeDescription(UpgradeId id);
/** Run locally before requesting; the host re-checks. */
bool CanBuyUpgrade(UpgradeId id, std::string_view *reason = nullptr);

/** Asks the host to buy the next tier (defined in keeper_game.cpp, with the other requests). */
void RequestBuyUpgrade(UpgradeId id);

/** Back to no upgrades (called from ResetGame). */
void ResetUpgrades();
/** Raises a tier, updates the living monsters and tells the player (a purchase). */
void ApplyUpgradeTier(UpgradeId id, int tier);
/** The same with no announcement: for catching a joiner up, when nobody just bought anything. */
void SetUpgradeTierQuietly(UpgradeId id, int tier);
/** Catches up with the host's snapshot quietly (a client that joined late or missed a purchase). */
void SyncUpgradeTiers(const std::array<uint8_t, NumUpgrades> &tiers);

/** Remembers a freshly created monster's own stats, then upgrades it (from OnMonsterInit). */
void OnKeepMonsterInit(Monster &monster);

int UpgradedManaRegen(int baseRegen);

} // namespace keeper
} // namespace devilution
