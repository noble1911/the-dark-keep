/**
 * @file keeper/keeper_devctl.cpp
 *
 * Dev/test only. See keeper_devctl.hpp.
 */
#include "keeper/keeper_devctl.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <vector>

#include <SDL.h>
#include <fmt/format.h>

#include "cursor.h"
#include "diablo.h"
#include "engine/dx.h"
#include "engine/surface.hpp"
#include "keeper/keeper.hpp"
#include "keeper/keeper_bots.hpp"
#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_devqa.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_heroes.hpp"
#include "keeper/keeper_input.hpp"
#include "keeper/keeper_profile.hpp"
#include "keeper/keeper_upgrades.hpp"
#include "levels/gendung.h"
#include "missiles.h"
#include "monster.h"
#include "objects.h"
#include "player.h"
#include "utils/display.h"
#include "utils/is_of.hpp"
#include "utils/surface_to_png.hpp"

namespace devilution::keeper {

void DevHallSelect(int whichTab, size_t index);

namespace {

bool enabled = false;
std::string commandPath;
std::string outputPath;
std::streamoff readOffset = 0;
std::string partialLine;
std::deque<std::string> pending;
uint32_t waitUntil = 0;
std::string waitPhase;
uint32_t waitPhaseUntil = 0;
uint32_t logEvery = 0;
uint32_t nextLog = 0;
SDL_Keymod modifiers = KMOD_NONE;
Point mouse { 0, 0 };

void Output(std::string_view line)
{
	std::ofstream out(outputPath, std::ios::app);
	out << line << '\n';
}

void PushMotion(int x, int y)
{
	mouse = { x, y };
	SDL_Event event {};
	event.type = SDL_MOUSEMOTION;
	event.motion.x = x;
	event.motion.y = y;
	SDL_PushEvent(&event);
}

void PushButton(bool down, bool right)
{
	SDL_Event event {};
	event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
	event.button.button = right ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
	event.button.state = down ? SDL_PRESSED : SDL_RELEASED;
	event.button.clicks = 1;
	event.button.x = mouse.x;
	event.button.y = mouse.y;
	SDL_PushEvent(&event);
}

void PushKey(SDL_Keycode key)
{
	SDL_Event event {};
	event.key.keysym.sym = key;
	event.key.keysym.scancode = SDL_GetScancodeFromKey(key);
	event.key.keysym.mod = modifiers;
	event.type = SDL_KEYDOWN;
	event.key.state = SDL_PRESSED;
	SDL_PushEvent(&event);
	event.type = SDL_KEYUP;
	event.key.state = SDL_RELEASED;
	SDL_PushEvent(&event);
}

void WriteShot(const std::string &path)
{
	SDL_RWops *file = SDL_RWFromFile(path.c_str(), "wb");
	if (file == nullptr) {
		Output(fmt::format("error cannot write {}", path));
		return;
	}
	// As true colour: the 8-bit surface's palette doesn't survive saving as PNG.
	SDL_Surface *rgb = SDL_ConvertSurfaceFormat(PalSurface, SDL_PIXELFORMAT_RGB24, 0);
	if (const auto result = WriteSurfaceToFilePng(Surface(rgb), file); !result.has_value())
		Output(fmt::format("error {}", result.error()));
	SDL_FreeSurface(rgb);
}

void WriteState()
{
	int keepers = 0;
	const bool inKeep = IsActive();
	if (inKeep) {
		for (size_t i = 0; i < ActiveMonsterCount; i++) {
			const Monster &monster = Monsters[ActiveMonsters[i]];
			if (monster.hitPoints > 0 && monster.mode != MonsterMode::Death && !IsHero(monster))
				keepers++;
		}
	}
	int lairs = 0;
	for (const Lair &lair : State.lairs)
		lairs += lair.active ? 1 : 0;
	int traps = 0;
	for (const Trap &trap : State.traps)
		traps += trap.active ? 1 : 0;
	const Monster *boss = inKeep ? FindBoss() : nullptr;
	const int heroes = inKeep ? LivingHeroCount() : 0;
	std::string line = fmt::format("state t={} inKeep={} phase={} phaseTicks={} wave={}/{} gold={} lost={} mana={} cd={} lairs={} traps={} pop={}/{}",
	    SDL_GetTicks(), inKeep, PhaseName(State.phase), State.phaseTicks, State.wave, NumWaves(), State.vaultGold, State.goldLost, State.mana[MyPlayerId] / ManaScale, State.spellCooldown[MyPlayerId], lairs, traps,
	    inKeep ? SupplyUsed() : 0, inKeep ? SupplyCap() : 0);
	line += fmt::format(" heroes={} loot={} monsters={} boss={},{} bossHp={}/{} bossDown={}",
	    heroes, inKeep ? GoldInFlight() : 0, keepers, boss != nullptr ? boss->position.tile.x : -1, boss != nullptr ? boss->position.tile.y : -1,
	    boss != nullptr ? boss->hitPoints >> 6 : 0, boss != nullptr ? boss->maxHitPoints >> 6 : 0, State.bossDown);
	line += fmt::format(" xp={} mapXp={} klevel={} renown={} cleared={} curs={},{} mouse={},{} screen={}x{} level={} entrance={},{} vault={},{}",
	    ProfileXp(), MapXpEarned(), KeeperLevel(), RenownAvailable(), LevelsCleared(), cursPosition.x, cursPosition.y, MousePosition.x, MousePosition.y, gnScreenWidth, gnScreenHeight, CurrentLevelNumber(),
	    State.entrance.x, State.entrance.y, State.vault.x, State.vault.y);
	Output(line);
}

/** Raid events, without having to poll: when heroes reach the vault, and how each raid ended. */
struct RaidWatch {
	bool inRaid = false;
	int wave = 0;
	uint32_t startTick = 0;
	bool reached = false;
	int keepAtStart = 0;
	int peakHeroes = 0;
	int peakPartyLife = 0;
	int lordAtStart = 0;
	int lordLow = 0;
	int goldAtStart = 0;
	int partyLife = 0;
	int heroesNow = 0;
	int keepNow = 0;
};
RaidWatch watch;
std::string autocast;
size_t autocastNext = 0;

void WatchRaid()
{
	const bool inKeep = IsActive();
	const bool raiding = inKeep && State.phase == Phase::Raid;
	if (!raiding && !watch.inRaid)
		return;
	if (raiding) {
		int party = 0;
		int heroes = 0;
		int keep = 0;
		int nearest = 9999;
		for (size_t i = 0; i < ActiveMonsterCount; i++) {
			const Monster &monster = Monsters[ActiveMonsters[i]];
			if (monster.hitPoints <= 0 || monster.mode == MonsterMode::Death)
				continue;
			if (IsHero(monster)) {
				heroes++;
				party += monster.hitPoints >> 6;
				nearest = std::min(nearest, monster.position.tile.WalkingDistance(State.vault));
			} else if (monster.type().type != MT_GOLEM) {
				keep++;
			}
		}
		for (size_t i = 0; i < MaxBots; i++) {
			const Player &bot = Players[FirstBotId + i];
			if (!IsLivingBot(bot))
				continue;
			heroes++;
			party += bot._pHitPoints >> 6;
			nearest = std::min(nearest, bot.position.tile.WalkingDistance(State.vault));
		}
		const Monster *boss = FindBoss();
		const int lord = boss != nullptr ? boss->hitPoints >> 6 : 0;
		if (!watch.inRaid) {
			watch = {};
			watch.inRaid = true;
			watch.wave = State.wave;
			watch.startTick = DevKeeperTicks();
			watch.keepAtStart = keep;
			watch.lordAtStart = lord;
			watch.lordLow = lord;
			watch.goldAtStart = State.vaultGold;
		}
		watch.peakHeroes = std::max(watch.peakHeroes, heroes);
		watch.peakPartyLife = std::max(watch.peakPartyLife, party);
		watch.lordLow = std::min(watch.lordLow, lord);
		watch.partyLife = party;
		watch.heroesNow = heroes;
		watch.keepNow = keep;
		if (!watch.reached && nearest <= 3) {
			watch.reached = true;
			Output(fmt::format("reach wave={} ticks={}", watch.wave, DevKeeperTicks() - watch.startTick));
		}
		if (!autocast.empty() && heroes > 0 && State.spellCooldown[MyPlayerId] == 0) {
			std::istringstream list(autocast);
			std::vector<size_t> spells;
			for (std::string item; std::getline(list, item, ',');)
				spells.push_back(static_cast<size_t>(std::atoi(item.c_str())));
			for (size_t tried = 0; tried < spells.size(); tried++) {
				const size_t spell = spells[(autocastNext + tried) % spells.size()];
				if (State.mana[MyPlayerId] < SpellManaCost(spell) * ManaScale)
					continue;
				autocastNext += tried + 1;
				for (size_t i = 0; i < MaxBots; i++) {
					if (IsLivingBot(Players[FirstBotId + i])) {
						RequestCastSpell(spell, Players[FirstBotId + i].position.tile);
						Output(fmt::format("autocast {} tick={}", spell, DevKeeperTicks() - watch.startTick));
						return;
					}
				}
				for (size_t i = 0; i < ActiveMonsterCount; i++) {
					const Monster &monster = Monsters[ActiveMonsters[i]];
					if (IsHero(monster) && monster.hitPoints > 0) {
						RequestCastSpell(spell, monster.position.tile);
						Output(fmt::format("autocast {} tick={}", spell, DevKeeperTicks() - watch.startTick));
						return;
					}
				}
				return;
			}
		}
		return;
	}
	watch.inRaid = false;
	Output(fmt::format("raidend wave={} result={} ticks={} peakHeroes={} peakPartyLife={} partyLifeLeft={} heroesLeft={} keepStart={} keepEnd={} lordStart={} lordLow={} goldStart={} goldEnd={} goldLost={}",
	    watch.wave, inKeep ? PhaseName(State.phase) : "left", DevKeeperTicks() - watch.startTick, watch.peakHeroes, watch.peakPartyLife, watch.partyLife, watch.heroesNow, watch.keepAtStart, watch.keepNow, watch.lordAtStart, watch.lordLow, watch.goldAtStart, State.vaultGold, State.goldLost));
}

void WriteMonsters()
{
	if (!IsActive())
		return;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const size_t id = ActiveMonsters[i];
		const Monster &monster = Monsters[id];
		const std::optional<size_t> lair = LairOfMonster(id);
		Output(fmt::format("mon {} {} hero={} lair={} at={},{} hp={}/{} mode={} home={},{}", id, monster.name(), IsHero(monster), lair ? static_cast<int>(*lair) : -1,
		    monster.position.tile.x, monster.position.tile.y, monster.hitPoints >> 6, monster.maxHitPoints >> 6, static_cast<int>(monster.mode),
		    lair ? State.lairs[*lair].position.x : -1, lair ? State.lairs[*lair].position.y : -1));
	}
	for (size_t i = 0; i < MaxBots; i++) {
		const Player &bot = Players[FirstBotId + i];
		if (IsLivingBot(bot))
			Output(fmt::format("bot {} {} at={},{} hp={}/{} mode={}", i, bot._pName, bot.position.tile.x, bot.position.tile.y, bot._pHitPoints >> 6, bot._pMaxHP >> 6, static_cast<int>(bot._pmode)));
	}
	for (const DoorLock &lock : State.locks) {
		if (lock.active)
			Output(fmt::format("lock at={},{} strength={}", lock.position.x, lock.position.y, lock.strength));
	}
	for (const Bait &bait : State.baits) {
		if (bait.active)
			Output(fmt::format("bait at={},{} uses={}", bait.position.x, bait.position.y, bait.uses));
	}
	Output(fmt::format("possessed={} upgrades={},{},{},{},{},{}", State.possessed[MyPlayerId], UpgradeTiers()[0], UpgradeTiers()[1], UpgradeTiers()[2], UpgradeTiers()[3], UpgradeTiers()[4], UpgradeTiers()[5]));
	for (size_t i = 0; i < State.traps.size(); i++) {
		if (State.traps[i].active)
			Output(fmt::format("trap {} kind={} at={},{} cooldown={}", i, State.traps[i].kind, State.traps[i].position.x, State.traps[i].position.y, State.traps[i].cooldown));
	}
}

/** The floor plan, for choosing where to build. # solid, . floor. */
void WriteMap()
{
	for (int i = 0; i < ActiveObjectCount; i++) {
		const Object &object = Objects[ActiveObjects[i]];
		if (object.isDoor())
			Output(fmt::format("door at={},{} closed={}", object.position.x, object.position.y, KeeperIsDoorClosed(object)));
	}
	for (int y = 0; y < MAXDUNY; y++) {
		std::string row;
		for (int x = 0; x < MAXDUNX; x++)
			row += TileHasAny({ x, y }, TileProperties::Solid) ? '#' : '.';
		Output(row);
	}
}

void WriteLairs()
{
	std::string selected;
	for (const size_t slot : SelectedLairs())
		selected += fmt::format("{} ", slot);
	Output(fmt::format("selected {}", selected));
	for (size_t slot = 0; slot < State.lairs.size(); slot++) {
		const Lair &lair = State.lairs[slot];
		if (!lair.active)
			continue;
		std::string route;
		for (size_t i = 0; i < lair.routeCount; i++)
			route += fmt::format("{},{} ", lair.route[i].x, lair.route[i].y);
		std::string members;
		for (size_t i = 0; i < lair.members.size(); i++) {
			if (lair.members[i] >= 0) {
				const Monster &monster = Monsters[lair.members[i]];
				members += fmt::format("{}@{},{}/leg{}/mode{} ", lair.members[i], monster.position.tile.x, monster.position.tile.y, lair.patrolLeg[i], static_cast<int>(monster.mode));
			}
		}
		Output(fmt::format("lair {} kind={} at={},{} rally={} {},{} route=[{}] members=[{}]", slot, lair.kind, lair.position.x, lair.position.y, lair.hasRally, lair.rally.x, lair.rally.y, route, members));
	}
}

/** Endurance: spends the keep's gold on the dearest kinds that fit, closest to the vault first. */
void AutoBuild()
{
	std::vector<Point> tiles;
	for (int x = 1; x < MAXDUNX - 1; x++) {
		for (int y = 1; y < MAXDUNY - 1; y++) {
			if (IsOpenFloor({ x, y }))
				tiles.push_back({ x, y });
		}
	}
	std::sort(tiles.begin(), tiles.end(), [](Point a, Point b) { return a.WalkingDistance(State.vault) < b.WalkingDistance(State.vault); });
	std::vector<size_t> kinds = MusterKinds();
	std::sort(kinds.begin(), kinds.end(), [](size_t a, size_t b) { return BuildKinds[a].cost > BuildKinds[b].cost; });
	int built = 0;
	int used = SupplyUsed();
	size_t from = 0;
	for (const size_t kind : kinds) {
		// Only the top few, so one level doesn't load the whole roster.
		if (kind == kinds[std::min<size_t>(6, kinds.size() - 1)])
			break;
		while (used + BuildKinds[kind].supply <= SupplyCap() && built < 24 && from < tiles.size()) {
			std::string_view reason;
			if (CanBuildAt(kind, tiles[from], &reason)) {
				RequestBuild(kind, tiles[from]);
				used += BuildKinds[kind].supply;
				built++;
				from += 4;
			}
			from++;
		}
	}
	for (size_t trap = 0; trap < NumTrapKinds; trap++) {
		if (IsTrapAvailable(trap)) {
			for (; from < tiles.size(); from++) {
				if (CanBuildAt(NumLairKinds + trap, tiles[from])) {
					RequestBuild(NumLairKinds + trap, tiles[from]);
					built++;
					break;
				}
			}
		}
	}
	Output(fmt::format("autobuild built={} pop={}/{} gold={}", built, used, SupplyCap(), State.vaultGold));
}

/** Endurance: spends all Renown, as a player would on the Keeper screen. */
void AutoProfile()
{
	int bought = 0;
	for (size_t kind = 0; kind < NumLairKinds; kind++)
		bought += UnlockKind(kind) ? 1 : 0;
	for (size_t round = 0; round < 2; round++) {
		for (size_t spell = 0; spell < NumSpellKinds; spell++) {
			const bool owned = OwnedSpellLevel(spell) > 0;
			if (RaiseSpell(spell)) {
				bought++;
				if (!owned)
					ToggleProfileSpell(spell);
			}
		}
		for (size_t trap = 0; trap < NumTrapKinds; trap++)
			bought += RaiseTrap(trap) ? 1 : 0;
	}
	Output(fmt::format("autoprofile bought={} klevel={} renown={} xp={}", bought, KeeperLevel(), RenownAvailable(), ProfileXp()));
}

int drawEvery = 1;
uint32_t drawCounter = 0;

struct Perf {
	uint32_t frames = 0;
	uint32_t start = 0;
	uint32_t worst = 0;
	uint32_t last = 0;
	uint32_t startTick = 0;
};
Perf perf;

void WritePerf()
{
	const uint32_t now = SDL_GetTicks();
	const uint32_t elapsed = now - perf.start;
	Output(fmt::format("perf t={} frames={} msPerFrame={:.2f} worstFrame={} keepTicks={} activeMonsters={} missiles={}", now, perf.frames, perf.frames > 0 ? static_cast<double>(elapsed) / perf.frames : 0.0, perf.worst, IsActive() ? DevKeeperTicks() : 0, IsActive() ? ActiveMonsterCount : 0, IsActive() ? Missiles.size() : 0));
	const uint32_t last = perf.last;
	perf = {};
	perf.start = now;
	perf.last = last;
}

void Execute(const std::string &line)
{
	std::istringstream in(line);
	std::string command;
	in >> command;
	if (command.empty() || command[0] == '#')
		return;
	int a = 0;
	int b = 0;
	int c = 0;
	std::string word;
	if (command == "move") {
		in >> a >> b;
		PushMotion(a, b);
	} else if (command == "click" || command == "down" || command == "up") {
		in >> a >> b >> word;
		// The game reads the tile under the cursor once a frame: the mouse gets there first, as a person's would.
		PushMotion(a, b);
		pending.push_front(fmt::format("press {} {}", command, word));
		waitUntil = SDL_GetTicks() + 80;
	} else if (command == "press") {
		std::string which;
		in >> which >> word;
		const bool right = word == "right";
		if (which != "up")
			PushButton(true, right);
		if (which != "down")
			PushButton(false, right);
	} else if (command == "key") {
		in >> word;
		const SDL_Keycode key = SDL_GetKeyFromName(word.c_str());
		if (key == SDLK_UNKNOWN)
			Output(fmt::format("error unknown key {}", word));
		else
			PushKey(key);
	} else if (command == "mod") {
		in >> word;
		modifiers = word == "shift" ? KMOD_LSHIFT : (word == "ctrl" ? KMOD_LCTRL : KMOD_NONE);
		SDL_SetModState(modifiers);
	} else if (command == "wheel") {
		in >> a;
		SDL_Event event {};
		event.type = SDL_MOUSEWHEEL;
		event.wheel.y = a;
		SDL_PushEvent(&event);
	} else if (command == "text") {
		in >> word;
		SDL_Event event {};
		event.type = SDL_TEXTINPUT;
		word.copy(event.text.text, sizeof(event.text.text) - 1);
		SDL_PushEvent(&event);
	} else if (command == "shot") {
		in >> word;
		WriteShot(word);
	} else if (command == "wait") {
		in >> a;
		waitUntil = SDL_GetTicks() + a;
	} else if (command == "waitphase") {
		in >> waitPhase >> a;
		waitPhaseUntil = SDL_GetTicks() + (a > 0 ? a : 60000);
	} else if (command == "state") {
		WriteState();
	} else if (command == "mons") {
		WriteMonsters();
	} else if (command == "map") {
		WriteMap();
	} else if (command == "log") {
		in >> a;
		logEvery = a;
		nextLog = 0;
	} else if (command == "speed") {
		in >> a;
		gnTickDelay = static_cast<uint16_t>(std::max(1000 / std::max(a, 1), 1));
	} else if (command == "cam") {
		in >> a >> b;
		DevCenterCamera({ a, b });
	} else if (command == "build") {
		in >> a >> b >> c;
		std::string_view reason;
		if (!CanBuildAt(static_cast<size_t>(a), { b, c }, &reason))
			Output(fmt::format("build {} {},{} refused: {}", a, b, c, reason));
		else
			RequestBuild(static_cast<size_t>(a), { b, c });
	} else if (command == "cast") {
		in >> a >> b >> c;
		RequestCastSpell(static_cast<size_t>(a), { b, c });
	} else if (command == "casthero") {
		// At the living hero nearest the entrance.
		in >> a;
		std::optional<Point> target;
		for (size_t i = 0; i < ActiveMonsterCount && !target; i++) {
			const Monster &monster = Monsters[ActiveMonsters[i]];
			if (IsHero(monster) && monster.hitPoints > 0)
				target = monster.position.tile;
		}
		for (size_t i = 0; i < MaxBots && !target; i++) {
			if (IsLivingBot(Players[FirstBotId + i]))
				target = Players[FirstBotId + i].position.tile;
		}
		if (target) {
			Output(fmt::format("cast {} at {},{}", a, target->x, target->y));
			RequestCastSpell(static_cast<size_t>(a), *target);
		} else {
			Output("cast: no hero");
		}
	} else if (command == "trapfire") {
		in >> a >> b >> c;
		DevFireTrap(static_cast<size_t>(a), { b, c });
	} else if (command == "autocast") {
		autocast.clear();
		in >> autocast;
	} else if (command == "sellall") {
		// The same request twice in one turn.
		in >> a;
		RequestSell({ StructureType::Lair, static_cast<size_t>(a) });
		RequestSell({ StructureType::Lair, static_cast<size_t>(a) });
	} else if (command == "bait") {
		in >> a >> b;
		RequestPlaceBait({ a, b });
	} else if (command == "lock") {
		in >> a >> b;
		RequestLockDoor({ a, b });
	} else if (command == "sell") {
		in >> a;
		RequestSell({ StructureType::Lair, static_cast<size_t>(a) });
	} else if (command == "raid") {
		RequestStartRaid();
	} else if (command == "gold") {
		in >> a;
		State.vaultGold = a;
	} else if (command == "rosterqa" || command == "spellqa" || command == "trapqa") {
		a = 0;
		b = -1;
		word = "-";
		in >> a >> b >> c >> word;
		DevQaStart(command, a, b, c, word, Output, WriteShot);
	} else if (command == "clicktile") {
		in >> a >> b >> word;
		const Point at = DevTileOnScreen({ a, b });
		PushMotion(at.x, at.y);
		pending.push_front(fmt::format("press click {}", word));
		waitUntil = SDL_GetTicks() + 80;
	} else if (command == "movetile") {
		in >> a >> b;
		const Point at = DevTileOnScreen({ a, b });
		PushMotion(at.x, at.y);
	} else if (command == "hallselect") {
		in >> a >> b;
		DevHallSelect(a, static_cast<size_t>(b));
	} else if (command == "lairs") {
		WriteLairs();
	} else if (command == "spelllevel") {
		in >> a >> b;
		State.spellLevels[static_cast<size_t>(a)] = static_cast<uint8_t>(b);
	} else if (command == "traplevel") {
		in >> a >> b;
		State.trapLevels[static_cast<size_t>(a)] = static_cast<uint8_t>(b);
	} else if (command == "wavenum") {
		in >> a;
		State.wave = static_cast<uint8_t>(a);
	} else if (command == "autobuild") {
		AutoBuild();
	} else if (command == "autoprofile") {
		AutoProfile();
	} else if (command == "perf") {
		WritePerf();
	} else if (command == "drawevery") {
		in >> drawEvery;
	} else if (command == "setphaseticks") {
		in >> a;
		State.phaseTicks = a;
	} else if (command == "fillmana") {
		State.mana[MyPlayerId] = MaxMana * ManaScale;
		State.spellCooldown[MyPlayerId] = 0;
	} else if (command == "echo") {
		std::string rest;
		std::getline(in, rest);
		Output(rest);
	} else if (command == "quit") {
		SDL_Event event {};
		event.type = SDL_QUIT;
		SDL_PushEvent(&event);
	} else {
		Output(fmt::format("error unknown command {}", line));
	}
}

void ReadNewCommands()
{
	std::error_code error;
	const auto size = std::filesystem::file_size(commandPath, error);
	if (error || static_cast<std::streamoff>(size) <= readOffset)
		return;
	std::ifstream in(commandPath, std::ios::binary);
	if (!in)
		return;
	in.seekg(readOffset);
	const std::string chunk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	readOffset += static_cast<std::streamoff>(chunk.size());
	partialLine += chunk;
	size_t start = 0;
	for (size_t end = partialLine.find('\n'); end != std::string::npos; end = partialLine.find('\n', start)) {
		std::string line = partialLine.substr(start, end - start);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		pending.push_back(std::move(line));
		start = end + 1;
	}
	partialLine.erase(0, start);
}

bool IsWaiting()
{
	const uint32_t now = SDL_GetTicks();
	if (now < waitUntil)
		return true;
	if (waitPhase.empty())
		return false;
	if ((IsActive() && PhaseName(State.phase) == waitPhase) || now >= waitPhaseUntil) {
		Output(fmt::format("waitphase {} {}", waitPhase, now >= waitPhaseUntil ? "timed out" : "reached"));
		waitPhase.clear();
		return false;
	}
	return true;
}

} // namespace

void DevctlInit(std::string path)
{
	enabled = true;
	commandPath = std::move(path);
	outputPath = commandPath + ".out";
}

bool DevctlSkipsDraw()
{
	return drawEvery > 1 && ++drawCounter % static_cast<uint32_t>(drawEvery) != 0;
}

void DevctlFrame()
{
	if (!enabled)
		return;
	{
		const uint32_t now = SDL_GetTicks();
		if (perf.last != 0)
			perf.worst = std::max(perf.worst, now - perf.last);
		perf.last = now;
		perf.frames++;
	}
	if (logEvery != 0 && SDL_GetTicks() >= nextLog) {
		nextLog = SDL_GetTicks() + logEvery;
		WriteState();
	}
	WatchRaid();
	ReadNewCommands();
	// One input a frame, so a screen sees each event as it would from a person.
	while (!pending.empty() && !DevQaRunning() && !IsWaiting()) {
		const std::string line = std::move(pending.front());
		pending.pop_front();
		Execute(line);
		std::istringstream first(line);
		std::string command;
		first >> command;
		if (IsAnyOf(command, "move", "click", "down", "up", "press", "key", "wheel"))
			break;
	}
}

} // namespace devilution::keeper
