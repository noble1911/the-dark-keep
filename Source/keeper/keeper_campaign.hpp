/**
 * @file keeper/keeper_campaign.hpp
 *
 * What game is being played: campaign level or custom map, and the master's saved progress.
 * This is the contract the roster, upgrades, map and menu code share (see docs/NEXT-PHASE.md).
 */
#pragma once

#include <cstdint>
#include <string>

namespace devilution::keeper {

constexpr int NumCampaignLevels = 20;

enum class GameMode : uint8_t {
	Campaign,
	CustomMap,
};

/** Chosen in the menus before the keep level loads. In co-op the host's setup is used by everyone. */
struct KeepSetup {
	GameMode mode = GameMode::Campaign;
	/** 1-based campaign level (ignored for custom maps). */
	int campaignLevel = 1;
	/** Seed for the random keep layout, when no map file is used. */
	uint32_t seed = 0;
	/** Map to load instead of a random layout. Empty: use `seed`. */
	std::string mapFile;
};

const KeepSetup &CurrentSetup();
void SetCurrentSetup(const KeepSetup &setup);

/**
 * The master's rank for this game, 1-based: rises as campaign levels are cleared and unlocks monsters.
 * In co-op this is the host's rank, so every client agrees on what can be built.
 */
int CampaignRank();

} // namespace devilution::keeper
