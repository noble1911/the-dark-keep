/**
 * @file keeper/keeper_botgear.hpp
 *
 * What the bot heroes wear: better gear as the campaign and the raids go on. Nothing here draws random
 * numbers. The tier comes from the campaign level and the raid, and items are made at their base stats,
 * so every client that spawns a bot gives it the same gear.
 */
#pragma once

#include <string_view>

#include "keeper/keeper_heroes.hpp"

namespace devilution {

struct Player;

namespace keeper {

/** 0 (starting gear) up to the best tier, from the current level and raid. */
int BotGearTier();
/** Dresses a bot for the current tier. Call before its stats are worked out. */
void EquipBotGear(Player &bot, HeroClassKind cls);
/** Extra spell levels a Sorcerer's gear is worth at the current tier. */
int BotGearSpellLevels();
/** The armour a bot of this class wears at the current tier (empty: starting gear). */
std::string_view BotGearArmorName(HeroClassKind cls);

} // namespace keeper
} // namespace devilution
