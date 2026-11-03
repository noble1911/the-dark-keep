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
#include <cstdint>
#include <string>
#include <string_view>

namespace devilution::keeper {

#ifdef KEEPER_DEVCTL
/** Starts reading commands from `path`; output goes to `path` + ".out". */
void DevctlInit(std::string path);
/** True in a game driven by --keeper-devctl: nothing may open a window, not even to report a fatal error. */
bool DevctlEnabled();

/** Runs due commands. Called once per presented frame, whatever screen is up. */
void DevctlFrame();

// Measuring how far a joiner's copies stray from the host's. `botsync 0` turns correction off, to compare.
bool DevctlBotSyncOn();
void DevctlNoteBotDrift(int tiles);
void DevctlNoteBotLifeDrift(int hitPoints);
void DevctlNoteBotFix(bool snap);
void DevctlNoteMonsterDrift(int tiles, int hitPoints);
/** A joiner's copy of a monster, already placed by the snapshot, is far from the host's: says which, for tracing it. */
void DevctlNoteMonsterJump(int monsterId, int hostX, int hostY);
void DevctlNoteSent(size_t bytes);
/** The event log, for raid scripts: EventPlrMsg copies each line here. */
void DevctlNoteMessage(std::string_view text);
/** Tells the script what turn a speed change landed on. */
void DevNoteSpeedSwitch(size_t level, uint32_t turn);

/** What the "perf" command times. */
enum class DevPerfCounter : uint8_t {
	Logic,
	Draw,
	Route,
	Field,
};
constexpr size_t NumDevPerfCounters = 4;
/** Adds its lifetime to a counter. */
class DevPerfScope {
public:
	explicit DevPerfScope(DevPerfCounter counter);
	~DevPerfScope();
	DevPerfScope(const DevPerfScope &) = delete;
	DevPerfScope &operator=(const DevPerfScope &) = delete;

private:
	DevPerfCounter counter_;
	uint64_t start_;
};
/** `drawevery N`: draw one frame in N, so a long sweep or soak runs faster. */
bool DevctlSkipsDraw();
#else
// Only built with -DKEEPER_DEVCTL=ON.
inline void DevctlFrame() { }
inline bool DevctlBotSyncOn() { return true; }
inline void DevctlNoteBotDrift(int /*tiles*/) { }
inline void DevctlNoteBotLifeDrift(int /*hitPoints*/) { }
inline void DevctlNoteBotFix(bool /*snap*/) { }
inline void DevctlNoteMonsterDrift(int /*tiles*/, int /*hitPoints*/) { }
inline void DevctlNoteMonsterJump(int /*monsterId*/, int /*hostX*/, int /*hostY*/) { }
inline void DevctlNoteSent(size_t /*bytes*/) { }
inline void DevNoteSpeedSwitch(size_t /*level*/, uint32_t /*turn*/) { }
#endif

} // namespace devilution::keeper
