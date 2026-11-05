/**
 * @file keeper/keeper_devctl.cpp
 *
 * Dev/test only. See keeper_devctl.hpp.
 */
#include "gmenu.h"
#include "keeper/keeper_devctl.hpp"

#include <algorithm>
#include <array>
#ifdef _WIN32
#include <stdlib.h>
#endif
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

#include "controls/control_mode.hpp"
#include "cursor.h"
#include "dead.h"
#include "diablo.h"
#include "engine/dx.h"
#include "engine/surface.hpp"
#include "items.h"
#include "keeper/keeper.hpp"
#include "keeper/keeper_barricade.hpp"
#include "keeper/keeper_bots.hpp"
#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_devqa.hpp"
#include "keeper/keeper_flow.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_heroes.hpp"
#include "keeper/keeper_input.hpp"
#include "keeper/keeper_pad.hpp"
#include "keeper/keeper_lobby.hpp"
#include "keeper/keeper_profile.hpp"
#include "keeper/keeper_raidtypes.hpp"
#include "keeper/keeper_resume.hpp"
#include "keeper/keeper_tutorial.hpp"
#include "keeper/keeper_upgrades.hpp"
#include "levels/gendung.h"
#include "missiles.h"
#include "monster.h"
#include "multi.h"
#include "objects.h"
#include "player.h"
#include "utils/display.h"
#include "utils/is_of.hpp"
#include "utils/surface_to_png.hpp"

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned int __stdcall SetErrorMode(unsigned int mode);
#endif

namespace devilution::keeper {

void DevHallSelect(int whichTab, size_t index);
void DevEditor(const std::string &what, const std::string &arg);

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

// ---- a controller: a real one as far as the game is concerned, SDL's virtual joystick ------------

SDL_Joystick *pad = nullptr;
std::optional<GamepadLayout> padLayout;

bool EnsurePad()
{
	if (pad != nullptr)
		return true;
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	const int device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
	if (device < 0) {
		Output(fmt::format("error no virtual pad: {}", SDL_GetError()));
		return false;
	}
	pad = SDL_JoystickOpen(device);
	if (pad == nullptr) {
		Output(fmt::format("error no virtual pad: {}", SDL_GetError()));
		return false;
	}
	return true;
}

/** The button named as on the SDL side: "a" is the bottom one, whichever layout is being tested. */
std::optional<SDL_GameControllerButton> PadButtonNamed(const std::string &name)
{
	if (name == "a") return SDL_CONTROLLER_BUTTON_A;
	if (name == "b") return SDL_CONTROLLER_BUTTON_B;
	if (name == "x") return SDL_CONTROLLER_BUTTON_X;
	if (name == "y") return SDL_CONTROLLER_BUTTON_Y;
	if (name == "back") return SDL_CONTROLLER_BUTTON_BACK;
	if (name == "start") return SDL_CONTROLLER_BUTTON_START;
	if (name == "l3") return SDL_CONTROLLER_BUTTON_LEFTSTICK;
	if (name == "r3") return SDL_CONTROLLER_BUTTON_RIGHTSTICK;
	if (name == "lb") return SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
	if (name == "rb") return SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
	if (name == "up") return SDL_CONTROLLER_BUTTON_DPAD_UP;
	if (name == "down") return SDL_CONTROLLER_BUTTON_DPAD_DOWN;
	if (name == "left") return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
	if (name == "right") return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
	return std::nullopt;
}

Sint16 StickValue(float value)
{
	return static_cast<Sint16>(std::clamp(value, -1.F, 1.F) * 32767.F);
}

std::string ControlTypeName(ControlTypes type)
{
	switch (type) {
	case ControlTypes::None:
		return "none";
	case ControlTypes::KeyboardAndMouse:
		return "kbm";
	case ControlTypes::Gamepad:
		return "pad";
	case ControlTypes::VirtualGamepad:
		return "touch";
	}
	return "?";
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
	line += fmt::format(" retryGold={} speed={} held={} turn={} ticks={}", State.retryGold, GameSpeedMultiplier(), IsPrepTimerHeld(), MyPlayer != nullptr ? multi_last_turn_from(LobbyHostSlot() != NoHostSlot ? LobbyHostSlot() : 0) : 0, DevKeeperTicks());
	line += fmt::format(" xp={} mapXp={} klevel={} renown={} cleared={} curs={},{} mouse={},{} screen={}x{} level={} entrance={},{} vault={},{}",
	    ProfileXp(), MapXpEarned(), KeeperLevel(), RenownAvailable(), LevelsCleared(), cursPosition.x, cursPosition.y, MousePosition.x, MousePosition.y, gnScreenWidth, gnScreenHeight, CurrentLevelNumber(),
	    State.entrance.x, State.entrance.y, State.vault.x, State.vault.y);
	line += fmt::format(" menu={}", gmenu_is_active() ? 1 : 0);
	line += fmt::format(" ready={} readyTicks={} voters={} snap={}", State.readyMask, State.readyTicks, MastersInKeepCount(), IsAwaitingKeepSnapshot() ? 1 : 0);
	line += fmt::format(" device={} mode={} myTile={},{}", ControlTypeName(ControlDevice), ControlTypeName(ControlMode), MyPlayer != nullptr ? MyPlayer->position.tile.x : -1, MyPlayer != nullptr ? MyPlayer->position.tile.y : -1);
	Output(line);
}

struct PerfTotals {
	uint64_t count = 0;
	uint64_t total = 0;
	uint64_t worst = 0;
};
std::array<PerfTotals, NumDevPerfCounters> perf;
uint32_t perfSince = 0;
uint32_t perfTicksAtStart = 0;

void WritePerf()
{
	const double frequency = static_cast<double>(SDL_GetPerformanceFrequency()) / 1000.0;
	const uint32_t now = SDL_GetTicks();
	std::string line = fmt::format("perf ms={} ticks={} speedDelay={} activeMonsters={} heroes={}", now - perfSince, DevKeeperTicks() - perfTicksAtStart, gnTickDelay, ActiveMonsterCount, IsActive() ? LivingHeroCount() : 0);
	constexpr std::array<const char *, NumDevPerfCounters> Names { "logic", "draw", "route", "field" };
	for (size_t i = 0; i < perf.size(); i++) {
		const PerfTotals &totals = perf[i];
		line += fmt::format(" {}[n={} avg={:.3f} max={:.3f} total={:.1f}]", Names[i], totals.count, totals.count > 0 ? static_cast<double>(totals.total) / frequency / static_cast<double>(totals.count) : 0.0, static_cast<double>(totals.worst) / frequency, static_cast<double>(totals.total) / frequency);
	}
	Output(line);
	perf = {};
	perfSince = now;
	perfTicksAtStart = DevKeeperTicks();
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
	int closest = 9999;
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
bool keepUnhurt = false;

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
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (monster.type().type == MT_GOLEM && monster.position.tile != GolemHoldingCell && !monster.isInvalid)
			Output(fmt::format("h t={} who=golem{} cls=Golem at={},{} hp={}/{} mode={} dmg={}-{} tohit={} enemy={} targetsMonster={}", tick, monster.getId(), monster.position.tile.x, monster.position.tile.y, monster.hitPoints >> 6, monster.maxHitPoints >> 6, static_cast<int>(monster.mode), monster.minDamage, monster.maxDamage, monster.golemToHit, monster.enemy, (monster.flags & MFLAG_TARGETS_MONSTER) != 0));
	}
	if (const Monster *boss = FindBoss(); boss != nullptr)
		Output(fmt::format("h t={} who=lord cls=Lord at={},{} hp={}/{} mode={}", tick, boss->position.tile.x, boss->position.tile.y, boss->hitPoints >> 6, boss->maxHitPoints >> 6, static_cast<int>(boss->mode)));
}
uint32_t lastAutocastTick = 0;

void WatchRaid()
{
	const bool inKeep = IsActive();
	const bool raiding = inKeep && State.phase == Phase::Raid;
	if (!raiding && !watch.inRaid)
		return;
	if (raiding) {
		if (keepUnhurt) {
			for (size_t i = 0; i < ActiveMonsterCount; i++) {
				Monster &monster = Monsters[ActiveMonsters[i]];
				if (!IsHero(monster) && !IsBoss(monster) && monster.hitPoints > 0 && monster.mode != MonsterMode::Death)
					monster.hitPoints = monster.maxHitPoints;
			}
		}
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
		watch.closest = std::min(watch.closest, nearest);
		if (!watch.reached && nearest <= 3) {
			watch.reached = true;
			Output(fmt::format("reach wave={} ticks={}", watch.wave, DevKeeperTicks() - watch.startTick));
		}
		if (!autocast.empty() && heroes > 0 && State.spellCooldown[MyPlayerId] == 0 && (autocastEvery == 0 || autocastLast == 0 || DevKeeperTicks() - autocastLast >= autocastEvery) && DevKeeperTicks() != lastAutocastTick) {
			lastAutocastTick = DevKeeperTicks(); // a cast takes effect a tick later: don't ask again each frame
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
	Output(fmt::format("raidend wave={} closest={} result={} ticks={} peakHeroes={} peakPartyLife={} partyLifeLeft={} heroesLeft={} keepStart={} keepEnd={} lordStart={} lordLow={} goldStart={} goldEnd={} goldLost={} trapFires={} trapsLeft={}",
	    watch.wave, watch.closest, inKeep ? PhaseName(State.phase) : "left", DevKeeperTicks() - watch.startTick, watch.peakHeroes, watch.peakPartyLife, watch.partyLife, watch.heroesNow, watch.keepAtStart, watch.keepNow, watch.lordAtStart, watch.lordLow, watch.goldAtStart, State.vaultGold, State.goldLost, watch.trapFires, std::count_if(State.traps.begin(), State.traps.end(), [](const Trap &trap) { return trap.active; })));
}

struct DriftStats {
	int samples = 0;
	int64_t total = 0;
	int worst = 0;
	void add(int value)
	{
		samples++;
		total += value;
		worst = std::max(worst, value);
	}
	std::string describe() const
	{
		return fmt::format("n={} mean={:.2f} max={}", samples, samples > 0 ? static_cast<double>(total) / samples : 0.0, worst);
	}
};
bool botSyncOn = true;
DriftStats botTiles;
DriftStats botLife;
std::array<int, 6> botTileBuckets {};
DriftStats monsterTiles;
DriftStats monsterLife;
int botSnaps = 0;
int botWalks = 0;
size_t bytesSent = 0;
uint32_t driftStartTicks = 0;
uint32_t driftStartTime = 0;

void WriteDrift()
{
	const uint32_t seconds = std::max<uint32_t>((SDL_GetTicks() - driftStartTime) / 1000, 1);
	const uint32_t gameSeconds = std::max<uint32_t>((DevKeeperTicks() - driftStartTicks) / 20, 1);
	Output(fmt::format("drift gameSeconds={} botTiles {} hist0/1/2/3-4/5-9/10+={},{},{},{},{},{} botLife {} snaps={} walks={} monsterTiles {} monsterLife {} stateBytes={} perGameSecond={} realSeconds={}",
	    gameSeconds, botTiles.describe(), botTileBuckets[0], botTileBuckets[1], botTileBuckets[2], botTileBuckets[3], botTileBuckets[4], botTileBuckets[5], botLife.describe(), botSnaps, botWalks, monsterTiles.describe(), monsterLife.describe(), bytesSent, bytesSent / gameSeconds, seconds));
}

void ResetDrift()
{
	botTiles = {};
	botLife = {};
	botTileBuckets = {};
	monsterTiles = {};
	monsterLife = {};
	botSnaps = 0;
	botWalks = 0;
	bytesSent = 0;
	driftStartTicks = DevKeeperTicks();
	driftStartTime = SDL_GetTicks();
}

void WriteMonsters()
{
	if (!IsActive())
		return;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const size_t id = ActiveMonsters[i];
		const Monster &monster = Monsters[id];
		const std::optional<size_t> lair = LairOfMonster(id);
		const std::optional<HeroSyncState> mind = GetHeroSyncState(id);
		Output(fmt::format("mon {} {} hero={} lair={} at={},{} hp={}/{} mode={} home={},{} carried={} fleeing={} traps={} baits={} regroup={} fallback={}", id, monster.name(), IsHero(monster), lair ? static_cast<int>(*lair) : -1,
		    monster.position.tile.x, monster.position.tile.y, monster.hitPoints >> 6, monster.maxHitPoints >> 6, static_cast<int>(monster.mode),
		    lair ? State.lairs[*lair].position.x : -1, lair ? State.lairs[*lair].position.y : -1, mind ? mind->carried : 0, mind ? mind->fleeing : false, mind ? mind->trapsKnown : 0, mind ? mind->baitsVisited : 0, mind ? mind->regroupBudget : 0, mind ? mind->fallBackSteps : 0));
	}
	for (size_t i = 0; i < MaxBots; i++) {
		const Player &bot = Players[FirstBotId + i];
		if (IsLivingBot(bot)) {
			const std::optional<BotSummary> summary = GetBotSummary(bot);
			Output(fmt::format("bot {} {} at={},{} hp={}/{} mode={} stone={} flee={} carried={} potions={} dest={} step={}", i, bot._pName, bot.position.tile.x, bot.position.tile.y, bot._pHitPoints >> 6, bot._pMaxHP >> 6, static_cast<int>(bot._pmode),
			    IsPetrifiedBot(bot), summary->fleeing, summary->carried, summary->potions, static_cast<int>(bot.destAction), static_cast<int>(bot.walkpath[0])));
		}
		if (const std::optional<BotSyncState> mind = GetBotSyncState(i); mind)
			Output(fmt::format("botmind {} carried={} fleeing={} potions={} traps={} regroup={} fallback={}", i, mind->carried, mind->fleeing, mind->potions, mind->trapsKnown, mind->regroupBudget, mind->fallBackThinks));
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
/** Bodies on the floor: by corpse id, by type, and bot heroes' (drawn only). Then the tile with most bodies near it. */
void WriteBodies()
{
	int byId = 0;
	int byType = 0;
	int drawnOnly = 0;
	Point busiest {};
	int most = 0;
	for (int x = 0; x < MAXDUNX; x++) {
		for (int y = 0; y < MAXDUNY; y++) {
			const std::optional<size_t> type = CorpseTypeAt({ x, y });
			if (type && (dCorpseType[x][y] & BodyWithoutMarker) != 0)
				drawnOnly++;
			else if (type)
				byType++;
			else if (dCorpse[x][y] != 0)
				byId++;
			else
				continue;
			int near = 0;
			for (int dx = -4; dx <= 4; dx++) {
				for (int dy = -4; dy <= 4; dy++) {
					const Point tile { x + dx, y + dy };
					if (InDungeonBounds(tile) && (dCorpse[tile.x][tile.y] != 0 || CorpseTypeAt(tile)))
						near++;
				}
			}
			if (near > most) {
				most = near;
				busiest = { x, y };
			}
		}
	}
	Output(fmt::format("bodies byId={} byType={} heroBodies={} busiest={},{} near={}", byId, byType, drawnOnly, busiest.x, busiest.y, most));
	for (int x = 0; x < MAXDUNX; x++) {
		for (int y = 0; y < MAXDUNY; y++) {
			if ((dCorpseType[x][y] & BodyWithoutMarker) != 0 && CorpseTypeAt({ x, y }))
				Output(fmt::format("herobody {},{} type={}", x, y, *CorpseTypeAt({ x, y })));
		}
	}
}

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

struct FramePerf {
	uint32_t frames = 0;
	uint32_t start = 0;
	uint32_t worst = 0;
	uint32_t last = 0;
	uint32_t startTick = 0;
};
FramePerf framePerf;

void WriteFramePerf()
{
	const uint32_t now = SDL_GetTicks();
	const uint32_t elapsed = now - framePerf.start;
	Output(fmt::format("frameperf t={} frames={} msPerFrame={:.2f} worstFrame={} keepTicks={} activeMonsters={} missiles={}", now, framePerf.frames, framePerf.frames > 0 ? static_cast<double>(elapsed) / framePerf.frames : 0.0, framePerf.worst, IsActive() ? DevKeeperTicks() : 0, IsActive() ? ActiveMonsterCount : 0, IsActive() ? Missiles.size() : 0));
	const uint32_t last = framePerf.last;
	framePerf = {};
	framePerf.start = now;
	framePerf.last = last;
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
	} else if (command == "padattach") {
		// Hot-plugged, so it arrives through the same added event as a real one. Give it a moment.
		if (EnsurePad())
			waitUntil = SDL_GetTicks() + 300;
	} else if (command == "pad" || command == "paddown" || command == "padup") {
		// pad a: press and release. paddown lt, padup lt: hold; lt and rt are the triggers.
		in >> word;
		if (!EnsurePad())
			return;
		const bool isTrigger = word == "lt" || word == "rt";
		const std::optional<SDL_GameControllerButton> button = PadButtonNamed(word);
		if (!isTrigger && !button) {
			Output(fmt::format("error unknown pad button {}", word));
		} else {
			const auto set = [&](bool down) {
				if (isTrigger)
					SDL_JoystickSetVirtualAxis(pad, word == "lt" ? SDL_CONTROLLER_AXIS_TRIGGERLEFT : SDL_CONTROLLER_AXIS_TRIGGERRIGHT, down ? 32767 : -32768);
				else
					SDL_JoystickSetVirtualButton(pad, *button, down ? 1 : 0);
			};
			if (command == "pad") {
				set(true);
				pending.push_front(fmt::format("padup {}", word));
				waitUntil = SDL_GetTicks() + 60;
			} else {
				set(command == "paddown");
			}
		}
	} else if (command == "padchord") {
		// padchord lt x: the left trigger and a button, both down in the same frame, for one press.
		std::string other;
		in >> word >> other;
		const std::optional<SDL_GameControllerButton> button = PadButtonNamed(other);
		if (EnsurePad() && button) {
			SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
			SDL_JoystickSetVirtualButton(pad, *button, 1);
			pending.push_front(fmt::format("padup {}", other));
			pending.insert(pending.begin() + 1, "padup lt");
			waitUntil = SDL_GetTicks() + 60;
		}
	} else if (command == "stick") {
		// stick l 0.5 -1: x right, y up, held until set back to 0 0.
		float x = 0.F;
		float y = 0.F;
		in >> word >> x >> y;
		if (EnsurePad()) {
			const bool left = word == "l";
			SDL_JoystickSetVirtualAxis(pad, left ? SDL_CONTROLLER_AXIS_LEFTX : SDL_CONTROLLER_AXIS_RIGHTX, StickValue(x));
			SDL_JoystickSetVirtualAxis(pad, left ? SDL_CONTROLLER_AXIS_LEFTY : SDL_CONTROLLER_AXIS_RIGHTY, StickValue(-y));
		}
	} else if (command == "padlayout") {
		// generic, xbox, ps or nintendo: what a real controller of that kind would report.
		in >> word;
		padLayout = word == "nintendo" ? GamepadLayout::Nintendo : word == "xbox" ? GamepadLayout::Xbox : word == "ps" ? GamepadLayout::PlayStation : GamepadLayout::Generic;
	} else if (command == "padengine") {
		// padengine 1: what the engine's own controller code does in the keep, without keeper_pad.
		in >> a;
		DevPadEngineOnly = a != 0;
	} else if (command == "tvui") {
		in >> a;
		DevTvUiScale = a != 0;
	} else if (command == "padname") {
		// What the on-screen prompts would call these.
		Output(fmt::format("padname confirm={} cancel={} waypoint={} sell={} spell1={} raid={} zoom={}", InputName(PadAction::Confirm), InputName(PadAction::Cancel), InputName(PadAction::Waypoint), InputName(PadAction::Sell), InputName(KeeperAction::Spell, 0), InputName(KeeperAction::StartRaid), InputName(PadAction::Zoom)));
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
	} else if (command == "bodies") {
		WriteBodies();
	} else if (command == "log") {
		in >> a;
		logEvery = a;
		nextLog = 0;
	} else if (command == "resumefile") {
		Output(fmt::format("resumefile exists={}", HasResumeFile()));
	} else if (command == "ping") {
		in >> a >> b;
		Output(fmt::format("ping {},{} sent={}", a, b, RequestPing({ a, b })));
	} else if (command == "pings") {
		const uint32_t now = SDL_GetTicks();
		for (const Ping &ping : Pings()) {
			if (ping.until > now)
				Output(fmt::format("live ping {},{} from {} left={}", ping.tile.x, ping.tile.y, ping.player, ping.until - now));
		}
	} else if (command == "gamespeed") {
		CycleGameSpeed();
	} else if (command == "speed") {
		in >> a;
		gnTickDelay = static_cast<uint16_t>(std::max(1000 / std::max(a, 1), 1));
	} else if (command == "unhurt") {
		// Keep monsters shrug off every blow: targets a hero can see and get nowhere with.
		in >> a;
		keepUnhurt = a != 0;
	} else if (command == "lordat") {
		// Moves the Keep Lord and his post, to test heroes who have a long way to go to him.
		in >> a >> b;
		if (const Monster *boss = FindBoss(); boss != nullptr) {
			Monster &lord = const_cast<Monster &>(*boss);
			const Point at { a, b };
			M_ClearSquares(lord);
			lord.position.tile = at;
			lord.position.old = at;
			lord.position.future = at;
			lord.occupyTile(at, false);
			SetMonsterHome(lord.getId(), at);
			M_StartStand(lord, lord.direction);
		}
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
	} else if (command == "botsync") {
		in >> a;
		botSyncOn = a != 0;
	} else if (command == "drift") {
		WriteDrift();
	} else if (command == "driftreset") {
		ResetDrift();
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
		std::string_view reason;
		if (!CanLockDoorAt({ a, b }, &reason))
			Output(fmt::format("lock {},{} refused: {}", a, b, reason));
		else
			RequestLockDoor({ a, b });
	} else if (command == "sell") {
		in >> a;
		RequestSell({ StructureType::Lair, static_cast<size_t>(a) });
	} else if (command == "herostate") {
		// id carried fleeing trapsKnown: for testing what a late joiner hears.
		int fleeing = 0;
		int known = 0;
		in >> a >> b >> fleeing >> known;
		if (std::optional<HeroSyncState> hero = GetHeroSyncState(static_cast<size_t>(a)); hero) {
			hero->carried = b;
			hero->fleeing = fleeing != 0;
			hero->trapsKnown = static_cast<uint16_t>(known);
			ApplyHeroSyncState(static_cast<size_t>(a), *hero);
		}
	} else if (command == "botstate") {
		// slot carried fleeing potions.
		int fleeing = 0;
		in >> a >> b >> fleeing >> c;
		if (std::optional<BotSyncState> bot = GetBotSyncState(static_cast<size_t>(a)); bot) {
			bot->carried = b;
			bot->fleeing = fleeing != 0;
			bot->potions = c;
			ApplyBotSyncState(static_cast<size_t>(a), *bot);
		}
	} else if (command == "pace") {
		// hero|bot id regroup fallback: for testing what a late joiner hears.
		in >> word >> a >> b >> c;
		if (word == "hero") {
			if (std::optional<HeroSyncState> hero = GetHeroSyncState(static_cast<size_t>(a)); hero) {
				hero->regroupBudget = b;
				hero->fallBackSteps = c;
				ApplyHeroSyncState(static_cast<size_t>(a), *hero);
			}
		} else if (std::optional<BotSyncState> bot = GetBotSyncState(static_cast<size_t>(a)); bot) {
			bot->regroupBudget = b;
			bot->fallBackThinks = c;
			ApplyBotSyncState(static_cast<size_t>(a), *bot);
		}
	} else if (command == "win") {
		DevForceVictory();
	} else if (command == "portal") {
		const PortalSyncState portal = GetPortalSyncState();
		Output(fmt::format("portal open={} at={},{} channel={} bot={} caster={} bossDown={}", portal.open, portal.position.x, portal.position.y, portal.channel, portal.botSlot, portal.casterMonster, State.bossDown));
	} else if (command == "raid") {
		RequestStartRaid();
	} else if (command == "perf") {
		WritePerf();
	} else if (command == "retry") {
		RequestRetryRaid();
	} else if (command == "gold") {
		in >> a;
		State.vaultGold = a;
	} else if (command == "raidtypes") {
		std::string types;
		for (int raid = 1; raid <= NumWaves(); raid++)
			types += fmt::format("{}={} ", raid, RaidTypeInfoOf(RaidTypeForWave(static_cast<uint8_t>(raid))).name);
		Output(types);
	} else if (command == "routecheck") {
		// What locking each door would do to the heroes' route.
		const size_t base = EstimateHeroRoute({}).size();
		Output(fmt::format("route {} tiles", base));
		for (int i = 0; i < ActiveObjectCount; i++) {
			const Object &object = Objects[ActiveObjects[i]];
			if (object.isDoor())
				Output(fmt::format("door {},{} locked: route {} tiles", object.position.x, object.position.y, EstimateHeroRoute({ .extraLock = object.position }).size()));
		}
	} else if (command == "hurtheroes") {
		// Every hero down to one hit point, for testing how raids end.
		for (size_t i = 0; i < ActiveMonsterCount; i++) {
			Monster &monster = Monsters[ActiveMonsters[i]];
			if (IsHero(monster) && monster.hitPoints > 0)
				monster.hitPoints = 1 << 6;
		}
		for (size_t i = 0; i < MaxBots; i++) {
			if (IsLivingBot(Players[FirstBotId + i]))
				SetPlayerHitPoints(Players[FirstBotId + i], 1 << 6);
		}
	} else if (command == "textcheck") {
		for (const std::string &problem : CheckTextFits())
			Output(fmt::format("textcheck {}", problem));
		Output("textcheck done");
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
	} else if (command == "editor") {
		std::string arg;
		in >> word >> arg;
		DevEditor(word, arg);
	} else if (command == "lockroute") {
		// The first door or neck on the heroes' way that can be shut, a good way in.
		const std::vector<Point> route = EstimateHeroRoute({});
		Output(fmt::format("lockroute route={} tiles barricades={}", route.size(), UsesBarricades()));
		for (size_t i = route.size() / 3; i < route.size(); i++) {
			for (const Direction dir : { Direction::NoDirection, Direction::North, Direction::East, Direction::South, Direction::West }) {
				const Point tile = dir == Direction::NoDirection ? route[i] : route[i] + dir;
				std::string_view reason;
				if (CanLockDoorAt(tile, &reason)) {
					Output(fmt::format("lockroute locking {},{}", tile.x, tile.y));
					RequestLockDoor(tile);
					return;
				}
			}
		}
		Output("lockroute nothing to lock");
	} else if (command == "hallselect") {
		in >> a >> b;
		DevHallSelect(a, static_cast<size_t>(b));
	} else if (command == "lairs") {
		WriteLairs();
	} else if (command == "spelllevel") {
		in >> a >> b;
		if (a >= 0 && static_cast<size_t>(a) < State.spellLevels.size())
			State.spellLevels[static_cast<size_t>(a)] = static_cast<uint8_t>(std::clamp(b, 0, MaxKeepLevel));
	} else if (command == "traplevel") {
		in >> a >> b;
		if (a >= 0 && static_cast<size_t>(a) < State.trapLevels.size())
			State.trapLevels[static_cast<size_t>(a)] = static_cast<uint8_t>(std::clamp(b, 0, MaxKeepLevel));
	} else if (command == "wavenum") {
		in >> a;
		State.wave = static_cast<uint8_t>(a);
	} else if (command == "autobuild") {
		AutoBuild();
	} else if (command == "autoprofile") {
		AutoProfile();
	} else if (command == "frameperf") {
		WriteFramePerf();
	} else if (command == "drawevery") {
		in >> drawEvery;
	} else if (command == "setphaseticks") {
		in >> a;
		State.phaseTicks = a;
	} else if (command == "fillmana") {
		// Every master's: the host's snapshots would put a joiner's straight back.
		State.mana.fill(MaxMana * ManaScale);
		State.spellCooldown.fill(0);
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

bool DevctlBotSyncOn()
{
	return botSyncOn;
}

void DevctlNoteBotDrift(int tiles)
{
	botTiles.add(tiles);
	botTileBuckets[tiles <= 2 ? tiles : (tiles <= 4 ? 3 : (tiles <= 9 ? 4 : 5))]++;
}

void DevctlNoteBotLifeDrift(int hitPoints)
{
	botLife.add(hitPoints);
}

void DevctlNoteBotFix(bool snap)
{
	(snap ? botSnaps : botWalks)++;
}

void DevctlNoteMonsterDrift(int tiles, int hitPoints)
{
	monsterTiles.add(tiles);
	monsterLife.add(hitPoints);
}

void DevctlNoteSent(size_t bytes)
{
	bytesSent += bytes;
}

void DevctlInit(std::string path)
{
	enabled = true;
	commandPath = std::move(path);
	outputPath = commandPath + ".out";
#ifdef _WIN32
	// A failed assert must end the run and say why in a file: the usual message box would land on the user's desktop.
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	SetErrorMode(0x0001 | 0x0002); // SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX: no Windows crash dialog either
#endif
	(void)std::freopen((commandPath + ".err").c_str(), "w", stderr);
}

bool DevctlEnabled()
{
	return enabled;
}

bool DevctlSkipsDraw()
{
	return drawEvery > 1 && ++drawCounter % static_cast<uint32_t>(drawEvery) != 0;
}

void DevctlNoteMonsterJump(int monsterId, int hostX, int hostY)
{
	if (!enabled)
		return;
	const Monster &monster = Monsters[monsterId];
	Output(fmt::format("bigdrift t={} mon={} {} here={},{} future={},{} old={},{} host={},{} mode={} hp={} hero={} invalid={} phase={}", DevKeeperTicks(), monsterId, monster.name(), monster.position.tile.x, monster.position.tile.y, monster.position.future.x, monster.position.future.y, monster.position.old.x, monster.position.old.y, hostX, hostY, static_cast<int>(monster.mode), monster.hitPoints >> 6, IsHero(monster), monster.isInvalid, PhaseName(State.phase)));
}

void DevctlNoteMessage(std::string_view text)
{
	if (enabled)
		Output(fmt::format("msg t={} {}", IsActive() ? DevKeeperTicks() - watch.startTick : 0, text));
}

void DevNoteSpeedSwitch(size_t level, uint32_t turn)
{
	if (enabled)
		Output(fmt::format("speedswitch level={} turn={} tick={}", level, turn, DevKeeperTicks()));
}

DevPerfScope::DevPerfScope(DevPerfCounter counter)
    : counter_(counter)
    , start_(SDL_GetPerformanceCounter())
{
}

DevPerfScope::~DevPerfScope()
{
	const uint64_t spent = SDL_GetPerformanceCounter() - start_;
	PerfTotals &totals = perf[static_cast<size_t>(counter_)];
	totals.count++;
	totals.total += spent;
	totals.worst = std::max(totals.worst, spent);
}

void DevctlFrame()
{
	if (!enabled)
		return;
	{
		const uint32_t now = SDL_GetTicks();
		if (framePerf.last != 0)
			framePerf.worst = std::max(framePerf.worst, now - framePerf.last);
		framePerf.last = now;
		framePerf.frames++;
	}
	if (logEvery != 0 && SDL_GetTicks() >= nextLog) {
		nextLog = SDL_GetTicks() + logEvery;
		WriteState();
	}
	WatchRaid();
	DevQaCheckStillInKeep();
	ReadNewCommands();
	if (padLayout)
		GamepadType = *padLayout;
	// One input a frame, so a screen sees each event as it would from a person.
	while (!pending.empty() && !DevQaRunning() && !IsWaiting()) {
		const std::string line = std::move(pending.front());
		pending.pop_front();
		Execute(line);
		std::istringstream first(line);
		std::string command;
		first >> command;
		if (IsAnyOf(command, "move", "click", "down", "up", "press", "key", "wheel", "pad", "paddown", "padup", "stick"))
			break;
	}
}

} // namespace devilution::keeper
