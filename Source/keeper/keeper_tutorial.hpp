/**
 * @file keeper/keeper_tutorial.hpp
 *
 * What the game tells a new master: first-map tips, the help overlay and the HUD's hover hints.
 *
 * All the words live in tables at the top of keeper_tutorial.cpp, and name their inputs with tokens such
 * as {raid} rather than a key. InputLabel() is the one place that turns an action into a name, so a
 * controller's button names only have to be added there. To document a new HUD item, add a row to the
 * help table (pages grow and split by themselves) and, if it needs a hover hint, a HudArea.
 *
 * Nothing here touches the simulation. Each client shows its own tips from its own profile.
 */
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/point.hpp"
#include "engine/rectangle.hpp"
#include "engine/surface.hpp"
#include "keeper/keeper_input.hpp"
#include "tables/monstdat.h"
#include "tables/spelldat.h"

namespace devilution::keeper {

// ---- names of inputs ---------------------------------------------------------------------------

/** What the player calls the input for an action. The one place to add a controller's button names. */
std::string InputLabel(KeeperAction action, size_t index = 0);
/** Replaces the tokens in a text ({raid}, {spell1}, {help}...) with InputLabel's names. */
std::string ExpandInputs(std::string_view text);

// ---- small pictures ------------------------------------------------------------------------------

struct UiIcon {
	enum class Type : uint8_t {
		None,
		Spell,
		/** An inventory picture: `id` is an ICURS_ value. */
		Item,
		/** A monster: `id` is a _monster_id. */
		Monster,
		/** A roster kind: `id` is the kind. */
		Kind,
	};
	Type type = Type::None;
	int id = 0;
};
constexpr UiIcon SpellIconOf(SpellID spell)
{
	return { UiIcon::Type::Spell, static_cast<int>(spell) };
}
constexpr UiIcon ItemIcon(int cursor)
{
	return { UiIcon::Type::Item, cursor };
}
constexpr UiIcon MonsterIcon(_monster_id monster)
{
	return { UiIcon::Type::Monster, static_cast<int>(monster) };
}
constexpr UiIcon KindIcon(size_t kind)
{
	return { UiIcon::Type::Kind, static_cast<int>(kind) };
}
/** Draws it in the middle of `area`, whatever size that is; a bigger picture is cropped. */
void DrawUiIcon(const Surface &area, const UiIcon &icon);

// ---- first-map tips ------------------------------------------------------------------------------

/** Places on the HUD a tip can point at, in UI pixels. */
struct TipTargets {
	Rectangle cards;
	Rectangle tabs;
	Rectangle ready;
	Rectangle spellBar;
	std::optional<Rectangle> roster;
};

/** The keep loaded: back to the first tip that is due. */
void TutorialOnLevelLoaded();
void TutorialTick();
/** This master cast a spell. */
void TutorialNoteSpellCast();
/** Draws the tip due now (if any) with its top at `y`, inside `left`..`left + width`. */
void DrawTutorialTip(const Surface &out, int left, int width, int y, Point mouse, const TipTargets &targets);
/** @return true if the click was the tip's. */
bool TutorialOnClick(Point mouse);
bool TutorialIsMouseOver(Point mouse);
/** @return true if Esc dismissed a tip. */
bool TutorialOnEscape();
/** The Keeper screen: the tip about spending Renown, in `area`. @return true if a click was the tip's. */
bool DrawKeeperScreenTip(const Surface &out, const Rectangle &area, Point mouse, bool clicked);

// ---- help ----------------------------------------------------------------------------------------

bool IsHelpOpen();
void ToggleHelp();
void CloseHelp();
void DrawHelp(const Surface &out, Point mouse);
void HelpOnClick(Point mouse);
void HelpOnWheel(int delta);

// ---- hover hints --------------------------------------------------------------------------------

enum class HudArea : uint8_t {
	Phase,
	Raids,
	Pop,
	Vault,
	Heroes,
	Mana,
	OpenGates,
	TabLairs,
	TabTraps,
	TabUpgrades,
	HelpButton,
	Scout,
};
struct HudHint {
	std::string title;
	std::string text;
};
HudHint DescribeHudArea(HudArea area);

// ---- what to say after a defeat -------------------------------------------------------------------

struct DefeatAdvice {
	std::string why;
	std::vector<std::string> tryNext;
};
DefeatAdvice AdviseAfterDefeat(uint8_t wave);

/** Dev/test: lists every tip and help row that doesn't fit its box at the narrowest layout. */
std::vector<std::string> CheckTextFits();

} // namespace devilution::keeper
