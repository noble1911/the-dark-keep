/**
 * @file keeper/keeper_devctl.hpp
 *
 * Dev/test only (--keeper-devctl <file>): lets a script drive a running game without touching the
 * desktop. Commands are read from the file, input arrives as pushed SDL events (no real cursor or
 * focus), and screenshots come straight from the game's own surface. Run with SDL_VIDEODRIVER=dummy
 * and no window opens at all. scripts/dev/testrun.ps1 and ctl.ps1 use it.
 */
#pragma once

#include <string>

namespace devilution::keeper {

#ifdef KEEPER_DEVCTL
/** Starts reading commands from `path`; output goes to `path` + ".out". */
void DevctlInit(std::string path);

/** Runs due commands. Called once per presented frame, whatever screen is up. */
void DevctlFrame();
/** `drawevery N`: draw one frame in N, so a long sweep or soak runs faster. */
bool DevctlSkipsDraw();
#else
// Only built with -DKEEPER_DEVCTL=ON.
inline void DevctlFrame() { }
#endif

} // namespace devilution::keeper
