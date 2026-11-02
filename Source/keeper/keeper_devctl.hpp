/**
 * @file keeper/keeper_devctl.hpp
 *
 * Dev/test only (--keeper-devctl <file>): lets a script drive a running game without touching the
 * desktop. Commands are read from the file, input arrives as pushed SDL events (no real cursor or
 * focus), and screenshots come straight from the game's own surface. Run with SDL_VIDEODRIVER=dummy
 * and no window opens at all. scripts/dev/testrun.ps1 and ctl.ps1 use it.
 */
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace devilution::keeper {

#ifdef KEEPER_DEVCTL
/** Starts reading commands from `path`; output goes to `path` + ".out". */
void DevctlInit(std::string path);

/** Runs due commands. Called once per presented frame, whatever screen is up. */
void DevctlFrame();

// Measuring how far a joiner's copies stray from the host's. `botsync 0` turns correction off, to compare.
bool DevctlBotSyncOn();
void DevctlNoteBotDrift(int tiles);
void DevctlNoteBotLifeDrift(int hitPoints);
void DevctlNoteBotFix(bool snap);
void DevctlNoteMonsterDrift(int tiles, int hitPoints);
void DevctlNoteSent(size_t bytes);
/** The event log, for raid scripts: EventPlrMsg copies each line here. */
void DevctlNoteMessage(std::string_view text);
#else
// Only built with -DKEEPER_DEVCTL=ON.
inline void DevctlFrame() { }
inline bool DevctlBotSyncOn() { return true; }
inline void DevctlNoteBotDrift(int /*tiles*/) { }
inline void DevctlNoteBotLifeDrift(int /*hitPoints*/) { }
inline void DevctlNoteBotFix(bool /*snap*/) { }
inline void DevctlNoteMonsterDrift(int /*tiles*/, int /*hitPoints*/) { }
inline void DevctlNoteSent(size_t /*bytes*/) { }
#endif

} // namespace devilution::keeper
