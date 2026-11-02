/**
 * @file keeper/keeper_devctl.cpp
 *
 * Dev/test only. See keeper_devctl.hpp.
 */
#include "keeper/keeper_devctl.hpp"

#include <algorithm>
#include <array>
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
#include "items.h"
#include "keeper/keeper.hpp"
#include "keeper/keeper_bots.hpp"
#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_heroes.hpp"
#include "keeper/keeper_profile.hpp"
#include "keeper/keeper_upgrades.hpp"
#include "levels/gendung.h"
#include "monster.h"
#include "objects.h"
#include "player.h"
#include "utils/display.h"
#include "utils/is_of.hpp"
#include "utils/surface_to_png.hpp"

namespace devilution::keeper {

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
	/** Times a trap has gone off, seen as its cooldown starting. */
	int trapFires = 0;
	std::array<int, MaxTraps> trapCooldown {};
};
RaidWatch watch;
std::string autocast;
size_t autocastNext = 0;
/** Ticks between autocasts, so a test can spend mana as a person might. 0: whenever the spell is ready. */
uint32_t autocastEvery = 0;
uint32_t autocastLast = 0;
/** Ticks between lines about every hero (`herolog`), for scripts/dev/hero_raids.py. 0: off. */
uint32_t heroLogEvery = 0;
uint32_t heroLogLast = 0;

/** Every hero's place and state: enough to spot a stuck, circling or burning one from the log. */
void WriteHeroLog()
{
	const uint32_t tick = DevKeeperTicks() - watch.startTick;
	for (size_t i = 0; i < MaxBots; i++) {
		const Player &bot = Players[FirstBotId + i];
		const std::optional<BotSummary> summary = GetBotSummary(bot);
		if (!summary)
			continue;
		Output(fmt::format("h t={} who=b{} cls={} at={},{} hp={}/{} mode={} carried={} fleeing={} hazard={} act={} ac={} block={} dmg={}-{} weapon={}", tick, i, HeroClassName(summary->cls), bot.position.tile.x, bot.position.tile.y,
		    bot._pHitPoints >> 6, bot._pMaxHP >> 6, static_cast<int>(bot._pmode), summary->carried, summary->fleeing, IsHazardous(bot.position.tile), static_cast<int>(bot.destAction), bot.GetArmor(), bot.GetBlockChance(), bot._pIMinDam, bot._pIMaxDam,
		    bot.InvBody[INVLOC_HAND_LEFT].isEmpty() ? "" : bot.InvBody[INVLOC_HAND_LEFT]._iName));
	}
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		const std::optional<HeroSummary> summary = GetHeroSummary(monster);
		if (!summary || monster.isInvalid || monster.mode == MonsterMode::Death)
			continue;
		Output(fmt::format("h t={} who=m{} cls={} at={},{} hp={}/{} mode={} carried={} fleeing={} hazard={} act={}", tick, monster.getId(), HeroClassName(summary->cls), monster.position.tile.x, monster.position.tile.y,
		    monster.hitPoints >> 6, monster.maxHitPoints >> 6, static_cast<int>(monster.mode), summary->carried, summary->fleeing, IsHazardous(monster.position.tile), (monster.flags & MFLAG_NO_ENEMY) == 0 ? 1 : 0));
	}
	if (const Monster *boss = FindBoss(); boss != nullptr)
		Output(fmt::format("h t={} who=lord cls=Lord at={},{} hp={}/{} mode={}", tick, boss->position.tile.x, boss->position.tile.y, boss->hitPoints >> 6, boss->maxHitPoints >> 6, static_cast<int>(boss->mode)));
}

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
		for (size_t i = 0; i < MaxTraps; i++) {
			if (State.traps[i].active && State.traps[i].cooldown > watch.trapCooldown[i])
				watch.trapFires++;
			watch.trapCooldown[i] = State.traps[i].active ? State.traps[i].cooldown : 0;
		}
		if (heroLogEvery != 0 && DevKeeperTicks() - heroLogLast >= heroLogEvery) {
			heroLogLast = DevKeeperTicks();
			WriteHeroLog();
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
		if (!autocast.empty() && heroes > 0 && State.spellCooldown[MyPlayerId] == 0 && (autocastEvery == 0 || autocastLast == 0 || DevKeeperTicks() - autocastLast >= autocastEvery)) {
			std::istringstream list(autocast);
			std::vector<size_t> spells;
			for (std::string item; std::getline(list, item, ',');)
				spells.push_back(static_cast<size_t>(std::atoi(item.c_str())));
			for (size_t tried = 0; tried < spells.size(); tried++) {
				const size_t spell = spells[(autocastNext + tried) % spells.size()];
				if (State.mana[MyPlayerId] < SpellManaCostScaled(spell))
					continue;
				autocastNext += tried + 1;
				autocastLast = DevKeeperTicks();
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
	Output(fmt::format("raidend wave={} result={} ticks={} peakHeroes={} peakPartyLife={} partyLifeLeft={} heroesLeft={} keepStart={} keepEnd={} lordStart={} lordLow={} goldStart={} goldEnd={} goldLost={} trapFires={} trapsLeft={}",
	    watch.wave, inKeep ? PhaseName(State.phase) : "left", DevKeeperTicks() - watch.startTick, watch.peakHeroes, watch.peakPartyLife, watch.partyLife, watch.heroesNow, watch.keepAtStart, watch.keepNow, watch.lordAtStart, watch.lordLow, watch.goldAtStart, State.vaultGold, State.goldLost, watch.trapFires, std::count_if(State.traps.begin(), State.traps.end(), [](const Trap &trap) { return trap.active; })));
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
		SDL_RWops *file = SDL_RWFromFile(word.c_str(), "wb");
		if (file == nullptr)
			Output(fmt::format("error cannot write {}", word));
		else {
			// As true colour: the 8-bit surface's palette doesn't survive saving as PNG.
			SDL_Surface *rgb = SDL_ConvertSurfaceFormat(PalSurface, SDL_PIXELFORMAT_RGB24, 0);
			if (const auto result = WriteSurfaceToFilePng(Surface(rgb), file); !result.has_value())
				Output(fmt::format("error {}", result.error()));
			SDL_FreeSurface(rgb);
		}
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
		autocastEvery = 0;
		autocastLast = 0;
		in >> autocast >> autocastEvery;
	} else if (command == "herolog") {
		in >> a;
		heroLogEvery = static_cast<uint32_t>(a);
	} else if (command == "upgrade") {
		in >> a;
		RequestBuyUpgrade(static_cast<UpgradeId>(a));
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

void DevctlNoteMessage(std::string_view text)
{
	if (enabled)
		Output(fmt::format("msg t={} {}", IsActive() ? DevKeeperTicks() - watch.startTick : 0, text));
}

void DevctlFrame()
{
	if (!enabled)
		return;
	if (logEvery != 0 && SDL_GetTicks() >= nextLog) {
		nextLog = SDL_GetTicks() + logEvery;
		WriteState();
	}
	WatchRaid();
	ReadNewCommands();
	// One input a frame, so a screen sees each event as it would from a person.
	while (!pending.empty() && !IsWaiting()) {
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
