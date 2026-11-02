/**
 * @file keeper/keeper_devctl.cpp
 *
 * Dev/test only. See keeper_devctl.hpp.
 */
#include "keeper/keeper_devctl.hpp"

#ifndef USE_SDL3

#include <algorithm>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>

#include <SDL.h>
#include <fmt/format.h>

#include "cursor.h"
#include "diablo.h"
#include "engine/dx.h"
#include "engine/surface.hpp"
#include "keeper/keeper.hpp"
#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_heroes.hpp"
#include "keeper/keeper_profile.hpp"
#include "levels/gendung.h"
#include "monster.h"
#include "player.h"
#include "utils/display.h"
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
	int heroes = 0;
	int keepers = 0;
	const bool inKeep = IsActive();
	if (inKeep) {
		for (size_t i = 0; i < ActiveMonsterCount; i++) {
			const Monster &monster = Monsters[ActiveMonsters[i]];
			if (monster.hitPoints <= 0 || monster.mode == MonsterMode::Death)
				continue;
			if (IsHero(monster))
				heroes++;
			else
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
	for (size_t i = 0; i < State.traps.size(); i++) {
		if (State.traps[i].active)
			Output(fmt::format("trap {} kind={} at={},{} cooldown={}", i, State.traps[i].kind, State.traps[i].position.x, State.traps[i].position.y, State.traps[i].cooldown));
	}
}

/** The floor plan, for choosing where to build. # solid, . floor. */
void WriteMap()
{
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
		const bool right = word == "right";
		PushMotion(a, b);
		if (command != "up")
			PushButton(true, right);
		if (command != "down")
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

void DevctlFrame()
{
	if (!enabled)
		return;
	if (logEvery != 0 && SDL_GetTicks() >= nextLog) {
		nextLog = SDL_GetTicks() + logEvery;
		WriteState();
	}
	ReadNewCommands();
	// One input a frame, so a screen sees each event as it would from a person.
	while (!pending.empty() && !IsWaiting()) {
		const std::string line = std::move(pending.front());
		pending.pop_front();
		Execute(line);
		const bool isInput = line.rfind("move", 0) == 0 || line.rfind("click", 0) == 0 || line.rfind("down", 0) == 0 || line.rfind("up", 0) == 0 || line.rfind("key", 0) == 0 || line.rfind("wheel", 0) == 0;
		if (isInput)
			break;
	}
}

} // namespace devilution::keeper

#else

namespace devilution::keeper {

void DevctlInit(std::string /*path*/)
{
}

void DevctlFrame()
{
}

} // namespace devilution::keeper

#endif
