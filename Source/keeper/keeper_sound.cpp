/**
 * @file keeper/keeper_sound.cpp
 *
 * Monster growls and hero voices. See keeper_sound.hpp.
 */
#include "keeper/keeper_sound.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#ifdef KEEPER_DEVCTL
#include <fmt/format.h>
#endif

#include "effects.h"
#include "engine/assets.hpp"
#include "engine/sound.h"
#include "engine/sound_position.hpp"
#include "keeper/keeper.hpp"
#include "keeper/keeper_alerts.hpp"
#include "keeper/keeper_game.hpp"
#include "levels/gendung.h"
#include "monster.h"
#include "options.h"
#include "tables/playerdat.hpp"

namespace devilution::keeper {

namespace {

// Times are the client's own clock, in milliseconds: how often a sound plays is about what the player hears, not game speed.
/** A monster growls again no sooner than this. */
constexpr uint32_t MonsterCryGapMs = 16000;
/** A guard that has just seen a hero growls no sooner than this after any other growl... */
constexpr uint32_t AlertGapMs = 1200;
/** ...and a guard only waiting, with a hero near, no sooner than this. */
constexpr uint32_t IdleGapMs = 3500;
/** A waiting guard growls when a hero is this close (it takes up the fight at GuardRadius, 8 from its post)... */
constexpr int IdleHeroTiles = 12;
/** ...about once in this many looks (one look per IdleLookMs). */
constexpr uint32_t IdleOdds = 8;
constexpr uint32_t IdleLookMs = 250;
/** Monsters further than this from the camera keep quiet: the growl is for the fight on screen. */
constexpr int ListenTiles = 18;
/** ListenTiles, or a whole keep's worth for a raid script measuring the most it could ever play. */
int listenTiles = ListenTiles;
/** Idle growls sit a little behind the fighting: as if from this many tiles further away. */
constexpr int IdleQuieter = 6 * 64;

std::array<uint32_t, MaxMonsters> lastCry {};
std::array<bool, MaxMonsters> wasGuarding {};
uint32_t lastAnyCry = 0;
uint32_t nextIdleLook = 0;
bool started = false;

/** The cosmetic generator: never the game's synced one, which every client must draw from alike. */
uint32_t cosmeticSeed = 1;
uint32_t CosmeticRandom(uint32_t below)
{
	cosmeticSeed = cosmeticSeed * 1103515245U + 12345U;
	return below == 0 ? 0 : ((cosmeticSeed >> 16) & 0x7FFF) % below;
}

/** Counts for raid scripts (`sounds` in keeper_devctl.cpp): a handful of ints, kept in every build. */
struct Stats {
	int alerts = 0;
	int idles = 0;
	int voices = 0;
	int voicesUnvoiced = 0;
	/** Guards that took up a fight, and why some of them kept quiet. */
	int engaged = 0;
	int quietCooldown = 0;
	int quietFar = 0;
	int quietNoSound = 0;
	uint32_t firstCry = 0;
	uint32_t lastCry = 0;
	uint32_t minGap = UINT32_MAX;
	uint32_t minSameMonsterGap = UINT32_MAX;
};
Stats stats;

bool IsKeepMonster(const Monster &monster)
{
	return !monster.isInvalid && monster.hitPoints > 0 && monster.mode != MonsterMode::Death
	    && monster.type().type != MT_GOLEM && !IsHeroType(monster.type().type);
}

TSnd *PickSound(const Monster &monster, MonsterSound kind)
{
	const auto &variants = monster.type().sounds[static_cast<size_t>(kind)];
	const uint32_t first = CosmeticRandom(2);
	for (uint32_t i = 0; i < 2; i++) {
		TSnd *snd = variants[(first + i) % 2].get();
		if (snd != nullptr)
			return snd;
	}
	return nullptr;
}

/** @return true if the monster growled. */
bool Cry(const Monster &monster, bool alert, uint32_t now)
{
	const size_t id = monster.getId();
	const auto quiet = [alert](int &reason) {
		if (alert)
			reason++;
		return false;
	};
	if ((lastCry[id] != 0 && now - lastCry[id] < MonsterCryGapMs) || (lastAnyCry != 0 && now - lastAnyCry < (alert ? AlertGapMs : IdleGapMs)))
		return quiet(stats.quietCooldown);
	if (monster.position.tile.WalkingDistance(ViewPosition) > listenTiles)
		return quiet(stats.quietFar);
	TSnd *snd = PickSound(monster, MonsterSound::Attack);
	if (snd == nullptr || snd->isPlaying())
		return quiet(stats.quietNoSound);
	int volume = 0;
	int pan = 0;
	if (!CalculateSoundPosition(monster.position.tile, &volume, &pan))
		return false;
	if (!alert)
		volume = std::max(volume - IdleQuieter, ATTENUATION_MIN + 1);
	snd_play_snd(snd, volume, pan, *GetOptions().Audio.soundVolume);
	if (lastAnyCry != 0)
		stats.minGap = std::min(stats.minGap, now - lastAnyCry);
	if (lastCry[id] != 0)
		stats.minSameMonsterGap = std::min(stats.minSameMonsterGap, now - lastCry[id]);
	if (stats.firstCry == 0)
		stats.firstCry = now;
	stats.lastCry = now;
	(alert ? stats.alerts : stats.idles)++;
	// 0 means "never", so a clock that reads 0 still counts as a growl.
	lastCry[id] = std::max(now, 1U);
	lastAnyCry = lastCry[id];
	return true;
}

/** Engine class for a hero's class: the voices are the base game's. */
HeroClass VoiceClass(HeroClassKind cls)
{
	switch (cls) {
	case HeroClassKind::Rogue: return HeroClass::Rogue;
	case HeroClassKind::Sorcerer: return HeroClass::Sorcerer;
	case HeroClassKind::Monk: return HeroClass::Monk;
	default: return HeroClass::Warrior;
	}
}

/** A class's voice files are in the game data (the Monk's are Hellfire's): playing a missing one would stop the game. */
bool HasVoiceFiles(HeroClassKind cls)
{
	static std::array<int8_t, NumHeroClasses> known { -1, -1, -1, -1 };
	int8_t &have = known[static_cast<size_t>(cls)];
	if (have < 0) {
		constexpr std::array<const char *, NumHeroClasses> Probes { "sfx\\warrior\\warior29.wav", "sfx\\rogue\\rogue29.wav", "sfx\\sorceror\\mage29.wav", "sfx\\monk\\monk29.wav" };
		have = FindAsset(Probes[static_cast<size_t>(cls)]).ok() ? 1 : 0;
	}
	return have == 1;
}

/** The base game's lines that fit what was said, best first: a class lacking one tries the next. */
std::span<const HeroSpeech> VoicesFor(HeroLine line)
{
	static constexpr HeroSpeech Burning[] { HeroSpeech::ItsHotDownHere, HeroSpeech::Argh };
	static constexpr HeroSpeech WrongElement[] { HeroSpeech::ThatDidntDoAnything };
	static constexpr HeroSpeech FacingLord[] { HeroSpeech::TimeToDie, HeroSpeech::VengeanceIsMine, HeroSpeech::ImNotImpressed };
	static constexpr HeroSpeech Trap[] { HeroSpeech::WhatWasThat, HeroSpeech::IveGotABadFeelingAboutThis };
	static constexpr HeroSpeech FallingBack[] { HeroSpeech::HeavyBreathing, HeroSpeech::OofAh };
	static constexpr HeroSpeech Vault[] { HeroSpeech::JustWhatIWasLookingFor };
	switch (line) {
	case HeroLine::Burning: return Burning;
	case HeroLine::WrongElement: return WrongElement;
	case HeroLine::FacingLord: return FacingLord;
	case HeroLine::Trap: return Trap;
	case HeroLine::FallingBack: return FallingBack;
	case HeroLine::Vault: return Vault;
	// Nothing in the base game says "keep them off me", "stand behind me" or "I have you".
	default: return {};
	}
}

} // namespace

void SoundsOnLevelLoaded()
{
	lastCry.fill(0);
	wasGuarding.fill(false);
	lastAnyCry = 0;
	nextIdleLook = 0;
	started = false;
	cosmeticSeed = SDL_GetTicks() | 1;
}

void SoundsTick()
{
	if (!gbSndInited || !gbSoundOn || State.phase != Phase::Raid) {
		started = false;
		return;
	}
	const uint32_t now = SDL_GetTicks();
	const bool look = now >= nextIdleLook;
	if (look)
		nextIdleLook = now + IdleLookMs;
	std::vector<HeroMark> heroes;
	if (look)
		heroes = LivingHeroMarks();
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (!IsKeepMonster(monster))
			continue;
		const size_t id = monster.getId();
		const bool guarding = (monster.flags & MFLAG_NO_ENEMY) != 0;
		// A guard that has just picked out a hero: the base game's monsters snarl as they close in.
		if (started && wasGuarding[id] && !guarding) {
			stats.engaged++;
			Cry(monster, true, now);
		}
		wasGuarding[id] = guarding;
		if (!look || !guarding || CosmeticRandom(IdleOdds) != 0)
			continue;
		for (const HeroMark &hero : heroes) {
			if (hero.tile.WalkingDistance(monster.position.tile) <= IdleHeroTiles) {
				Cry(monster, false, now);
				break;
			}
		}
	}
	// The first tick only learns who is guarding: monsters that came into the raid fighting don't all snarl at once.
	started = true;
}

void PlayHeroVoice(HeroClassKind cls, Point tile, HeroLine line)
{
	if (!gbSndInited || !gbSoundOn || !HasVoiceFiles(cls))
		return;
	for (const HeroSpeech speech : VoicesFor(line)) {
		const SfxID sfx = GetHeroSound(VoiceClass(cls), speech);
		if (sfx == SfxID::None)
			continue;
		// Not randomised by category: that would draw from the synced generator.
		PlaySfxLoc(sfx, tile, false);
		stats.voices++;
		return;
	}
	stats.voicesUnvoiced++;
}

#ifdef KEEPER_DEVCTL
std::string DevSoundReport()
{
	const uint32_t span = stats.lastCry - stats.firstCry;
	const int cries = stats.alerts + stats.idles;
	return fmt::format("sounds soundOn={} musicOn={} music={} levelMusic={} leveltype={} alerts={} idles={} spanMs={} perMin={:.1f} minGapMs={} minSameMonsterGapMs={} engaged={} quietCooldown={} quietFar={} quietNoSound={} voices={} unvoiced={}",
	    gbSoundOn, gbMusicOn, static_cast<int>(sgnMusicTrack), static_cast<int>(GetLevelMusic(leveltype)), static_cast<int>(leveltype),
	    stats.alerts, stats.idles, span, span > 0 ? cries * 60000.0 / span : 0.0,
	    stats.minGap == UINT32_MAX ? -1 : static_cast<int64_t>(stats.minGap), stats.minSameMonsterGap == UINT32_MAX ? -1 : static_cast<int64_t>(stats.minSameMonsterGap),
	    stats.engaged, stats.quietCooldown, stats.quietFar, stats.quietNoSound, stats.voices, stats.voicesUnvoiced);
}

void DevSoundReset(int listen)
{
	stats = {};
	listenTiles = listen > 0 ? listen : ListenTiles;
}
#endif

} // namespace devilution::keeper
