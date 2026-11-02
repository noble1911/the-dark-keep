/**
 * @file keeper/keeper_botgear.cpp
 *
 * Bot gear that scales with the raid. See keeper_botgear.hpp.
 */
#include "keeper/keeper_botgear.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>

#include "items.h"
#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_game.hpp"
#include "player.h"
#include "tables/itemdat.h"

namespace devilution::keeper {

namespace {

/** One tier per this many campaign levels, and one more per this many raids within a level. */
constexpr int BotGearLevelsPerTier = 4;
constexpr int BotGearRaidsPerTier = 4;
constexpr int BotGearMaxTier = 5;
/** A Sorcerer's robes and staff are worth this many levels of its spells, per tier. */
constexpr int BotGearSpellLevelsPerTier = 1;
/**
 * Share of an item's listed armour a bot gets, in percent. Monsters hit bots on toHit + 30 - armour, floor 15%,
 * so the full listed value puts a late Warrior at that floor. The look and the name stay.
 */
constexpr int BotGearArmorPercent = 60;

struct GearSet {
	std::string_view weapon;
	std::string_view shield;
	std::string_view armor;
	std::string_view helm;
};

/** Item names from itemdat.tsv. Tier 0 is the class's starting gear; an empty name keeps what's there. */
constexpr std::array<std::array<GearSet, BotGearMaxTier>, NumHeroClasses> GearTable { {
    // Warrior
    { {
        { "Falchion", "Small Shield", "Leather Armor", "Cap" },
        { "Broad Sword", "Large Shield", "Ring Mail", "Helm" },
        { "Bastard Sword", "Kite Shield", "Scale Mail", "Helm" },
        { "Bastard Sword", "Kite Shield", "Splint Mail", "Full Helm" },
        { "Bastard Sword", "Tower Shield", "Field Plate", "Full Helm" },
    } },
    // Rogue
    { {
        { "Hunter's Bow", "", "Cloak", "" },
        { "Composite Bow", "", "Quilted Armor", "" },
        { "Short War Bow", "", "Leather Armor", "Cap" },
        { "Long Battle Bow", "", "Hard Leather Armor", "Cap" },
        { "Long War Bow", "", "Studded Leather Armor", "Helm" },
    } },
    // Sorcerer
    { {
        { "Long Staff", "", "Robe", "" },
        { "Composite Staff", "", "Quilted Armor", "" },
        { "Quarter Staff", "", "Leather Armor", "Cap" },
        { "War Staff", "", "Hard Leather Armor", "Cap" },
        { "War Staff", "", "Studded Leather Armor", "Helm" },
    } },
    // Monk. The engine gives a Monk in light armour twice its level in armour, and bots run to level 50: they wear mail.
    { {
        { "Long Staff", "", "Ring Mail", "" },
        { "Composite Staff", "", "Ring Mail", "" },
        { "Quarter Staff", "", "Ring Mail", "" },
        { "War Staff", "", "Chain Mail", "" },
        { "War Staff", "", "Chain Mail", "" },
    } },
} };

std::optional<_item_indexes> FindItem(std::string_view name)
{
	for (size_t i = 0; i < AllItemsList.size(); i++) {
		if (AllItemsList[i].iName == name)
			return static_cast<_item_indexes>(i);
	}
	return std::nullopt;
}

void Equip(Player &bot, inv_body_loc slot, std::string_view name)
{
	if (name.empty())
		return;
	const std::optional<_item_indexes> index = FindItem(name);
	if (!index)
		return;
	Item &item = bot.InvBody[slot];
	InitializeItem(item, *index); // base stats, no random rolls
	item._iAC = item._iAC * BotGearArmorPercent / 100;
	// A hero's gear suits its station whatever its strength: the requirements would only leave it bare-handed.
	item._iMinStr = 0;
	item._iMinMag = 0;
	item._iMinDex = 0;
	item._iIdentified = true;
	item._iStatFlag = true;
}

} // namespace

int BotGearTier()
{
	return std::clamp((CurrentLevelNumber() - 1) / BotGearLevelsPerTier + (std::max<int>(State.wave, 1) - 1) / BotGearRaidsPerTier, 0, BotGearMaxTier);
}

void EquipBotGear(Player &bot, HeroClassKind cls)
{
	const int tier = BotGearTier();
	if (tier == 0)
		return;
	const GearSet &gear = GearTable[static_cast<size_t>(cls)][tier - 1];
	Equip(bot, INVLOC_HAND_LEFT, gear.weapon);
	Equip(bot, INVLOC_HAND_RIGHT, gear.shield);
	Equip(bot, INVLOC_CHEST, gear.armor);
	Equip(bot, INVLOC_HEAD, gear.helm);
}

int BotGearSpellLevels()
{
	return BotGearTier() * BotGearSpellLevelsPerTier;
}

std::string_view BotGearArmorName(HeroClassKind cls)
{
	const int tier = BotGearTier();
	return tier == 0 ? std::string_view {} : GearTable[static_cast<size_t>(cls)][tier - 1].armor;
}

} // namespace devilution::keeper
