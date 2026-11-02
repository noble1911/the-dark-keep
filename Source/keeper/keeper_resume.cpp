/**
 * @file keeper/keeper_resume.cpp
 *
 * See keeper_resume.hpp. The file is text, line by line, and ends with a checksum of everything before it.
 */
#include "keeper/keeper_resume.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <sstream>
#include <string_view>

#include <fmt/format.h>

#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_game.hpp"
#include "levels/gendung_defs.hpp"
#include "utils/file_util.h"
#include "utils/log.hpp"
#include "utils/paths.h"

namespace devilution::keeper {

namespace {

constexpr std::string_view FileName = "keeper_resume.txt";
constexpr std::string_view Header = "dkresume";
constexpr int Version = 1;
/** Sanity limits for numbers read back: no game gets near them. */
constexpr int MaxGold = 100000000;
constexpr int MaxTicks = 20 * 60 * 60;

std::optional<ResumeData> pending;

std::string ResumePath()
{
	return paths::PrefPath() + std::string(FileName);
}

uint32_t Checksum(std::string_view text)
{
	uint32_t hash = 2166136261U;
	for (const char c : text) {
		hash ^= static_cast<unsigned char>(c);
		hash *= 16777619U;
	}
	return hash;
}

/** Through OpenFile: the path is UTF-8, and a user's folder needn't be ASCII. */
bool ReadWholeFile(const std::string &path, std::string &text)
{
	FILE *file = OpenFile(path.c_str(), "rb");
	if (file == nullptr)
		return false;
	std::array<char, 4096> chunk;
	size_t got = 0;
	while ((got = std::fread(chunk.data(), 1, chunk.size(), file)) > 0)
		text.append(chunk.data(), got);
	const bool ok = std::ferror(file) == 0;
	std::fclose(file);
	return ok;
}

bool IsTile(int x, int y)
{
	return x >= 0 && y >= 0 && x < MAXDUNX && y < MAXDUNY;
}

bool ReadTile(std::istream &in, Point &tile)
{
	int x = -1;
	int y = -1;
	if (!(in >> x >> y) || !IsTile(x, y))
		return false;
	tile = { x, y };
	return true;
}

bool ReadInt(std::istream &in, int &value, int low, int high)
{
	return (in >> value) && value >= low && value <= high;
}

bool ParsePiece(std::istream &in, ResumeData::SavedPiece &piece)
{
	return ReadInt(in, piece.kind, 0, 255) && ReadTile(in, piece.position) && ReadInt(in, piece.amount, 0, 255) && ReadInt(in, piece.cost, 0, MaxGold);
}

/** @return false on anything unexpected: there is no such thing as half a keep. */
bool Parse(std::string_view text, ResumeData &data)
{
	std::istringstream lines { std::string(text) };
	std::string line;
	if (!std::getline(lines, line) || line != fmt::format("{} {}", Header, Version))
		return false;
	while (std::getline(lines, line)) {
		std::istringstream in(line);
		std::string key;
		in >> key;
		bool ok = true;
		if (key == "level") {
			ok = ReadInt(in, data.level, 1, NumCampaignLevels);
		} else if (key == "seed") {
			ok = static_cast<bool>(in >> data.seed);
		} else if (key == "map") {
			std::getline(in >> std::ws, data.mapFile);
		} else if (key == "places") {
			ok = ReadTile(in, data.entrance) && ReadTile(in, data.vault);
		} else if (key == "phase") {
			ok = ReadInt(in, data.wave, 1, 255) && ReadInt(in, data.phase, 0, 2) && data.phase != static_cast<int>(Phase::Raid) && ReadInt(in, data.phaseTicks, 0, MaxTicks);
		} else if (key == "vault") {
			ok = ReadInt(in, data.gold, 0, MaxGold) && ReadInt(in, data.goldLost, 0, MaxGold) && ReadInt(in, data.mana, 0, MaxMana * ManaScale) && ReadInt(in, data.mapXp, 0, MaxGold);
		} else if (key == "upgrades") {
			for (uint8_t &tier : data.upgrades) {
				int value = 0;
				ok = ok && ReadInt(in, value, 0, 255);
				tier = static_cast<uint8_t>(value);
			}
		} else if (key == "lair") {
			ResumeData::SavedLair lair;
			int hasRally = 0;
			int routeCount = 0;
			ok = data.lairs.size() < MaxLairs && ReadInt(in, lair.kind, 0, 0xFFFF) && ReadInt(in, lair.monsterType, 0, 0xFFFF) && ReadInt(in, lair.cost, 0, MaxGold) && ReadTile(in, lair.position)
			    && ReadInt(in, hasRally, 0, 1) && ReadTile(in, lair.rally) && ReadInt(in, routeCount, 0, MaxPatrolPoints);
			for (int i = 0; ok && i < routeCount; i++) {
				Point tile;
				ok = ReadTile(in, tile);
				lair.route.push_back(tile);
			}
			lair.hasRally = hasRally != 0;
			data.lairs.push_back(std::move(lair));
		} else if (key == "trap" || key == "lock" || key == "bait") {
			std::vector<ResumeData::SavedPiece> &list = key == "trap" ? data.traps : (key == "lock" ? data.locks : data.baits);
			const size_t room = key == "trap" ? MaxTraps : (key == "lock" ? MaxLocks : MaxBaits);
			ResumeData::SavedPiece piece;
			ok = list.size() < room && ParsePiece(in, piece);
			list.push_back(piece);
		} else if (!key.empty()) {
			return false;
		}
		if (!ok)
			return false;
	}
	return true;
}

/** The level as it is now has to be the one that was saved. Not damage: a new version may just have changed it. */
bool IsCurrent(const ResumeData &data)
{
	if (data.level > NextOpenLevel() || data.wave > CampaignLevelRules(data.level).raids)
		return false;
	const KeepSetup setup = SetupForCampaignLevel(data.level);
	return setup.seed == data.seed && setup.mapFile == data.mapFile;
}

} // namespace

void WriteResumeFile(const ResumeData &data)
{
	std::string text = fmt::format("{} {}\n", Header, Version);
	text += fmt::format("level {}\nseed {}\n", data.level, data.seed);
	if (!data.mapFile.empty())
		text += fmt::format("map {}\n", data.mapFile);
	text += fmt::format("places {} {} {} {}\n", data.entrance.x, data.entrance.y, data.vault.x, data.vault.y);
	text += fmt::format("phase {} {} {}\n", data.wave, data.phase, data.phaseTicks);
	text += fmt::format("vault {} {} {} {}\n", data.gold, data.goldLost, data.mana, data.mapXp);
	text += "upgrades";
	for (const uint8_t tier : data.upgrades)
		text += fmt::format(" {}", tier);
	text += "\n";
	for (const ResumeData::SavedLair &lair : data.lairs) {
		text += fmt::format("lair {} {} {} {} {} {} {} {} {}", lair.kind, lair.monsterType, lair.cost, lair.position.x, lair.position.y, lair.hasRally ? 1 : 0, lair.rally.x, lair.rally.y, lair.route.size());
		for (const Point tile : lair.route)
			text += fmt::format(" {} {}", tile.x, tile.y);
		text += "\n";
	}
	const auto writePieces = [&text](std::string_view key, const std::vector<ResumeData::SavedPiece> &list) {
		for (const ResumeData::SavedPiece &piece : list)
			text += fmt::format("{} {} {} {} {} {}\n", key, piece.kind, piece.position.x, piece.position.y, piece.amount, piece.cost);
	};
	writePieces("trap", data.traps);
	writePieces("lock", data.locks);
	writePieces("bait", data.baits);
	text += fmt::format("sum {}\n", Checksum(text));

	const std::string path = ResumePath();
	const std::string temp = path + ".tmp";
	FILE *file = OpenFile(temp.c_str(), "wb");
	if (file == nullptr) {
		LogError("Cannot write keeper save {}", temp);
		return;
	}
	const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
	const bool closed = std::fclose(file) == 0;
	if (!written || !closed) {
		LogError("Cannot write keeper save {}", temp);
		RemoveFile(temp.c_str());
		return;
	}
	// The engine's RenameFile will not replace an existing file on Windows. The new file is complete
	// from here on: if the rename fails, or the game dies before it, ReadResumeFile takes it from .tmp.
	if (FileExists(path))
		RemoveFile(path.c_str());
	RenameFile(temp.c_str(), path.c_str());
	if (!FileExists(path))
		LogError("Cannot move keeper save into place: {}", path);
}

void DeleteResumeFile()
{
	const std::string path = ResumePath();
	if (FileExists(path))
		RemoveFile(path.c_str());
	if (FileExists(path + ".tmp"))
		RemoveFile((path + ".tmp").c_str());
}

bool HasResumeFile()
{
	const std::string path = ResumePath();
	return FileExists(path) || FileExists(path + ".tmp");
}

std::optional<ResumeData> ReadResumeFile()
{
	const std::string path = ResumePath();
	const std::string temp = path + ".tmp";
	const bool fromTemp = !FileExists(path);
	if (fromTemp && !FileExists(temp))
		return std::nullopt;
	std::string text;
	// A file that can't be read isn't known to be damaged: it stays, for next time.
	if (!ReadWholeFile(fromTemp ? temp : path, text)) {
		LogError("Cannot read keeper save {}", fromTemp ? temp : path);
		return std::nullopt;
	}
	const size_t sumAt = text.rfind("sum ");
	ResumeData data;
	bool intact = false;
	if (sumAt != std::string::npos) {
		uint32_t stored = 0;
		std::istringstream sum(text.substr(sumAt + 4));
		intact = (sum >> stored) && stored == Checksum(std::string_view(text).substr(0, sumAt)) && Parse(std::string_view(text).substr(0, sumAt), data);
	}
	if (!intact) {
		if (fromTemp) {
			// The game died while writing it: the save it was replacing went only after it was complete, so this was never the save.
			RemoveFile(temp.c_str());
			return std::nullopt;
		}
		Log("Keeper save {} is damaged or from another version: setting it aside", path);
		CopyFileOverwrite(path.c_str(), (path + ".bad").c_str());
		RemoveFile(path.c_str());
		return std::nullopt;
	}
	if (fromTemp)
		RenameFile(temp.c_str(), path.c_str());
	if (!IsCurrent(data))
		return std::nullopt;
	return data;
}

void SetPendingResume(ResumeData data)
{
	pending = std::move(data);
}

void ClearPendingResume()
{
	pending.reset();
}

std::optional<ResumeData> TakePendingResume()
{
	std::optional<ResumeData> taken = std::move(pending);
	pending.reset();
	return taken;
}

} // namespace devilution::keeper
