/**
 * @file keeper/keeper_resume.hpp
 *
 * Save and resume between raids, in a single-player campaign game: one slot, kept up to date at every
 * respite and at the moment a raid begins, offered as Continue on the Campaign screen.
 *
 * It can't dodge a raid's result beyond what Retry already allows: losing or winning deletes the file,
 * and leaving during a raid takes back that raid's XP, just as retrying it would. Retry then writes it
 * again. A file that is damaged, from another version, or for a level that has changed is ignored.
 */
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine/point.hpp"
#include "keeper/keeper_upgrades.hpp"

namespace devilution::keeper {

struct ResumeData {
	struct SavedLair {
		int kind = 0;
		/** Which monster the kind was: indexes aren't stable from one version of the roster to the next. */
		int monsterType = 0;
		/** What it cost: if it can't come back it is refunded, as selling it in the respite would have been. */
		int cost = 0;
		Point position;
		bool hasRally = false;
		Point rally;
		std::vector<Point> route;
	};
	struct SavedPiece {
		/** Traps: the trap kind. */
		int kind = 0;
		Point position;
		/** Locks: blows left. Bait: heroes left to fool. */
		int amount = 0;
		int cost = 0;
	};

	int level = 1;
	uint32_t seed = 0;
	std::string mapFile;
	Point entrance;
	Point vault;
	int wave = 1;
	/** Phase::Setup or Phase::Breather. */
	int phase = 0;
	int phaseTicks = 0;
	int gold = 0;
	int goldLost = 0;
	int mana = 0;
	int mapXp = 0;
	std::array<uint8_t, NumUpgrades> upgrades {};
	std::vector<SavedLair> lairs;
	std::vector<SavedPiece> traps;
	std::vector<SavedPiece> locks;
	std::vector<SavedPiece> baits;
};

void WriteResumeFile(const ResumeData &data);
void DeleteResumeFile();
/** True if there is a file at all, valid or not (a finished one still waiting to be moved into place counts). */
bool HasResumeFile();
/**
 * The save, if it can be used: intact, this version, a level that is open, and that level's seed and map
 * as they are now. A damaged file is set aside as .bad and doesn't come up again.
 */
std::optional<ResumeData> ReadResumeFile();

/** Continue was chosen: the keep picks this up once it has loaded. */
void SetPendingResume(ResumeData data);
void ClearPendingResume();
std::optional<ResumeData> TakePendingResume();

} // namespace devilution::keeper
