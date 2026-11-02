/**
 * @file keeper/keeper_devqa.hpp
 *
 * Dev/test only (KEEPER_DEVCTL): automated sweeps that a running game does on itself, driven from
 * keeper_devctl.cpp. Each sweep writes one `key=value` line per thing it checked.
 *
 * Each takes [first] [last] [window] [shotDir]; first and last are clamped to the list, last -1 means the end.
 *   rosterqa   every lair kind: placed, loaded, idles, fights, is hurt, dies, corpse. window: ticks to wait for a first hit
 *   spellqa    every spell at every level, then at the edges of the map
 *   trapqa     every trap at every level, on a hero and on a bot, with keep monsters by it. window > 0: all levels on a bot only
 */
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace devilution::keeper {

void DevQaStart(std::string_view sweep, int first, int last, int window, std::string shotDir, std::function<void(std::string_view)> output, std::function<void(const std::string &)> shot);
/** True while a sweep runs: other commands wait for it. */
bool DevQaRunning();
/** Every keep tick. */
void DevQaTick();
/** Every frame: keep ticks stop when the keep ends, and a sweep left running would hold up every command. */
void DevQaCheckStillInKeep();

} // namespace devilution::keeper
