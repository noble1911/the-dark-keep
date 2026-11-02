/**
 * @file keeper/keeper_upgrades.cpp
 *
 * The upgrade table, tiers and their effects. See keeper_upgrades.hpp.
 */
#include "keeper/keeper_upgrades.hpp"

#include <algorithm>

#include <fmt/format.h>

#include "keeper/keeper.hpp"
#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_pad.hpp"
#include "keeper/keeper_heroes.hpp"
#include "monster.h"
#include "plrmsg.h"

namespace devilution::keeper {

namespace {

// All the balance numbers for upgrades live in this table.
constexpr std::array<UpgradeInfo, NumUpgrades> Upgrades { {
	{ .cardName = "Mon Life", .title = "Monster health", .effectSign = "+", .effectUnit = "% life for your lair monsters", .icon = SpellID::Healing, .maxTier = 5, .baseCost = 150, .costStep = 100, .effectPerTier = 15 },
	{ .cardName = "Mon Dmg", .title = "Monster damage", .effectSign = "+", .effectUnit = "% damage for your lair monsters", .icon = SpellID::Rage, .maxTier = 5, .baseCost = 200, .costStep = 125, .effectPerTier = 15 },
	{ .cardName = "Mon Armor", .title = "Monster armor", .effectSign = "+", .effectUnit = " armor for your lair monsters", .icon = SpellID::ManaShield, .maxTier = 5, .baseCost = 100, .costStep = 75, .effectPerTier = 5 },
	{ .cardName = "Mana", .title = "Mana regeneration", .effectSign = "+", .effectUnit = "% mana regeneration", .icon = SpellID::Mana, .maxTier = 4, .baseCost = 120, .costStep = 80, .effectPerTier = 25 },
	{ .cardName = "Lord Life", .title = "Keep Lord health", .effectSign = "+", .effectUnit = "% life for the Keep Lord", .icon = SpellID::HealOther, .maxTier = 5, .baseCost = 150, .costStep = 100, .effectPerTier = 20 },
	{ .cardName = "Lord Dmg", .title = "Keep Lord damage", .effectSign = "+", .effectUnit = "% damage for the Keep Lord", .icon = SpellID::Berserk, .maxTier = 5, .baseCost = 150, .costStep = 100, .effectPerTier = 15 },
} };

/** The stats a monster had before any upgrade: every tier is worked out from these, so nothing compounds. */
struct BaseStats {
	int maxHitPoints = 0;
	uint8_t minDamage = 0;
	uint8_t maxDamage = 0;
	uint8_t minDamageSpecial = 0;
	uint8_t maxDamageSpecial = 0;
	uint8_t armorClass = 0;
};

std::array<uint8_t, NumUpgrades> tiers {};
/** By monster id; an id's entry is rewritten whenever a monster is created with that id. */
std::array<BaseStats, MaxMonsters> baseStats {};

/** What one tier times the current tier adds up to. */
int TotalEffect(UpgradeId id)
{
	return UpgradeTier(id) * GetUpgradeInfo(id).effectPerTier;
}

/** `value` raised by `percent`, to the nearest whole. Hit points are fixed point, which scales just the same. */
int AddPercent(int value, int percent)
{
	return static_cast<int>((static_cast<int64_t>(value) * (100 + percent) + 50) / 100);
}

uint8_t AddPercentToByte(uint8_t value, int percent)
{
	return static_cast<uint8_t>(std::min(AddPercent(value, percent), 255));
}

bool IsUpgradable(const Monster &monster)
{
	return !IsHero(monster) && monster.type().type != MT_GOLEM;
}

/** Sets a monster's stats from its base stats and the current tiers. Safe to repeat. */
void ApplyStats(Monster &monster)
{
	if (!IsUpgradable(monster))
		return;
	const BaseStats &base = baseStats[monster.getId()];
	const bool lord = IsBoss(monster);
	const int healthPercent = TotalEffect(lord ? UpgradeId::LordHealth : UpgradeId::MonsterHealth);
	const int damagePercent = TotalEffect(lord ? UpgradeId::LordDamage : UpgradeId::MonsterDamage);

	const int newMax = AddPercent(base.maxHitPoints, healthPercent);
	// A wounded monster gains the new life without being healed; dead ones stay dead.
	if (monster.hitPoints > 0)
		monster.hitPoints = std::max(std::min(monster.hitPoints + newMax - monster.maxHitPoints, newMax), 1);
	monster.maxHitPoints = newMax;

	monster.minDamage = AddPercentToByte(base.minDamage, damagePercent);
	monster.maxDamage = AddPercentToByte(base.maxDamage, damagePercent);
	monster.minDamageSpecial = AddPercentToByte(base.minDamageSpecial, damagePercent);
	monster.maxDamageSpecial = AddPercentToByte(base.maxDamageSpecial, damagePercent);
	if (!lord)
		monster.armorClass = static_cast<uint8_t>(std::min(base.armorClass + TotalEffect(UpgradeId::MonsterArmor), 255));
}

void ApplyStatsToLivingMonsters()
{
	if (!IsActive())
		return; // no keep loaded: monsters pick the tiers up as they are created
	for (size_t i = 0; i < ActiveMonsterCount; i++)
		ApplyStats(Monsters[ActiveMonsters[i]]);
}

/** @return true if the tier went up */
bool RaiseTier(UpgradeId id, int tier)
{
	const int clamped = std::min(tier, GetUpgradeInfo(id).maxTier);
	uint8_t &current = tiers[static_cast<size_t>(id)];
	if (clamped <= current)
		return false;
	current = static_cast<uint8_t>(clamped);
	return true;
}

} // namespace

const UpgradeInfo &GetUpgradeInfo(UpgradeId id)
{
	return Upgrades[static_cast<size_t>(id)];
}

int UpgradeTier(UpgradeId id)
{
	return tiers[static_cast<size_t>(id)];
}

const std::array<uint8_t, NumUpgrades> &UpgradeTiers()
{
	return tiers;
}

int UpgradeNextCost(UpgradeId id)
{
	const UpgradeInfo &info = GetUpgradeInfo(id);
	const int tier = UpgradeTier(id);
	// Tiers add percentages, so they are worth as much as the army they lift, and that grows with the level.
	return tier < info.maxTier ? ScaleByHeroStrength(info.baseCost + tier * info.costStep) : 0;
}

std::string UpgradeDescription(UpgradeId id)
{
	const UpgradeInfo &info = GetUpgradeInfo(id);
	const std::string effect = fmt::format("{}: {}{}{} per tier. Tier {} of {}.", info.title, info.effectSign, info.effectPerTier, info.effectUnit, UpgradeTier(id), info.maxTier);
	if (UpgradeNextCost(id) == 0)
		return effect;
	return fmt::format("{} {} to buy, between raids. Lasts this game.", effect, InputName(PadAction::Confirm));
}

bool CanBuyUpgrade(UpgradeId id, std::string_view *reason)
{
	auto fail = [reason](std::string_view why) {
		if (reason != nullptr)
			*reason = why;
		return false;
	};
	const int cost = UpgradeNextCost(id);
	if (cost == 0)
		return fail("Already at the top tier");
	if (State.phase != Phase::Setup && State.phase != Phase::Breather)
		return fail("Upgrades can only be bought between raids");
	if (State.vaultGold < cost)
		return fail("Not enough gold");
	return true;
}

void ResetUpgrades()
{
	tiers.fill(0);
	// A joining client has already created the keep's monsters by now, possibly under a previous game's tiers.
	ApplyStatsToLivingMonsters();
}

void ApplyUpgradeTier(UpgradeId id, int tier)
{
	if (!RaiseTier(id, tier))
		return;
	ApplyStatsToLivingMonsters();
	const UpgradeInfo &info = GetUpgradeInfo(id);
	EventPlrMsg(fmt::format("{} raised to tier {}.", info.title, UpgradeTier(id)), UiFlags::ColorGold);
}

void SetUpgradeTierQuietly(UpgradeId id, int tier)
{
	if (RaiseTier(id, tier))
		ApplyStatsToLivingMonsters();
}

void SyncUpgradeTiers(const std::array<uint8_t, NumUpgrades> &newTiers)
{
	bool changed = false;
	for (size_t i = 0; i < NumUpgrades; i++)
		changed = RaiseTier(static_cast<UpgradeId>(i), newTiers[i]) || changed;
	if (changed)
		ApplyStatsToLivingMonsters();
}

void OnKeepMonsterInit(Monster &monster)
{
	if (!IsUpgradable(monster))
		return;
	baseStats[monster.getId()] = {
		.maxHitPoints = monster.maxHitPoints,
		.minDamage = monster.minDamage,
		.maxDamage = monster.maxDamage,
		.minDamageSpecial = monster.minDamageSpecial,
		.maxDamageSpecial = monster.maxDamageSpecial,
		.armorClass = monster.armorClass,
	};
	ApplyStats(monster);
}

int UpgradedManaRegen(int baseRegen)
{
	return AddPercent(baseRegen, TotalEffect(UpgradeId::ManaRegen));
}

} // namespace devilution::keeper
