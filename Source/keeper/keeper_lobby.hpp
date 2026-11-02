/**
 * @file keeper/keeper_lobby.hpp
 *
 * Co-op in the keep: a joiner's panel for picking spells from what the host's game has, and the list
 * of masters that shows everyone who is who.
 *
 * A joiner's picks are local: casts are checked by ApplyCast on every client, so nothing here touches
 * the network, and the profile's own spell bar is never written. Picks last through the host's
 * Next/Retry games and are forgotten when the joiner leaves.
 */
#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "engine/point.hpp"
#include "engine/rectangle.hpp"
#include "engine/surface.hpp"
#include "keeper/keeper_game.hpp"

namespace devilution::keeper {

/** The spell bar for this game, by slot. */
using SpellBar = std::array<std::optional<size_t>, NumSpellSlots>;

/** The keep loaded and `bar` holds the profile's choice for this game: applies a joiner's earlier picks. */
void LobbyOnLevelLoaded(SpellBar &bar);
/** The host chose what comes next: picks carry over into that game. */
void LobbyNoteRematch();
void LobbyOnGameEnded();
/** Opens the panel when a joiner's first prompt is due, and closes it when a raid locks it. */
void LobbyTick();

// `mouse` and `spellBar` (the slots' combined rectangle) are in UI pixels.

/** @return true if Esc closed the panel. */
bool LobbyOnEscape();
/** True over the panel or its button, when they are showing. */
bool LobbyIsMouseOver(Point mouse, const Rectangle &spellBar);
/** @return true if the click was the panel's. `bar` changes if it picked or removed a spell. */
bool LobbyOnLeftClick(Point mouse, const Rectangle &spellBar, SpellBar &bar);
void DrawLobby(const Surface &out, Point mouse, const Rectangle &spellBar, const SpellBar &bar);

/** Everyone in the game and which one is the host; in co-op only. Clicks pass through it. */
void DrawMasters(const Surface &out);

} // namespace devilution::keeper
