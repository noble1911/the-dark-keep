/**
 * @file keeper/keeper_sound.hpp
 *
 * The keep's atmosphere sounds: the master's monsters growl when heroes come near, and heroes speak
 * their class voice lines along with what they say in the event log.
 *
 * Purely cosmetic and local to each client: it only reads the keep's state, never takes a number from
 * the game's synced random generator, and changes nothing in the simulation. Everything goes through
 * the engine's positional sound (heard from the camera) and the player's sound volume.
 *
 * Music needs nothing here: the keep loads through LoadGameLevel after PrepareMapLevel has set leveltype
 * from the map's tileset, so the base game's GetLevelMusic picks the Cathedral, Catacombs, Caves or Hell
 * track, in the editor and its test run (the edited map's tileset) and on every co-op client (the map the
 * host sent) alike. The track plays on through preparation and raids: changing or silencing it at the
 * raid would restart it every few minutes, and the raid already announces itself with a banner and sound.
 */
#pragma once

#include <cstdint>
#include <string>

#include "engine/point.hpp"
#include "keeper/keeper_heroes.hpp"

namespace devilution::keeper {

/** What a hero said in the event log, for choosing a voice line that fits. */
enum class HeroLine : uint8_t {
	None,
	Burning,
	Kiting,
	WrongElement,
	FacingLord,
	Protecting,
	Trap,
	FallingBack,
	Healing,
	Vault,
};

void SoundsOnLevelLoaded();
/** Every keep tick on every client. */
void SoundsTick();
/** A hero's line went into the event log: its voice, if its class has one that fits. */
void PlayHeroVoice(HeroClassKind cls, Point tile, HeroLine line);

#ifdef KEEPER_DEVCTL
/** What sounds this module has played, and the level's music, for raid scripts. */
std::string DevSoundReport();
/** Starts the counts again; `listen` > 0 hears monsters that far from the camera instead of the usual range. */
void DevSoundReset(int listen);
#endif

} // namespace devilution::keeper
