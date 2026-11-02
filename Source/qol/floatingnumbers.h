/**
 * @file floatingnumbers.h
 *
 * Adds floating numbers QoL feature
 */
#pragma once

#include <string>

#include "DiabloUI/ui_flags.hpp"
#include "engine/displacement.hpp"
#include "engine/point.hpp"
#include "engine/surface.hpp"

namespace devilution {

/** [DARK KEEP] A small picture beside a floating number saying what kind it is, so damage
 * dealt, damage taken, healing and experience read apart at a glance. */
enum class FloatingNumberIcon : uint8_t {
	None,
	Dealt,      // sword: damage you (or your companion) dealt
	Taken,      // skull: damage you took
	Heal,       // cross: life restored to you
	Experience, // arrow: experience gained
};

void AddFloatingNumber(Point pos, Displacement offset, std::string text, UiFlags style, int id = 0, bool reverseDirection = false, FloatingNumberIcon icon = FloatingNumberIcon::None);
void DrawFloatingNumbers(const Surface &out, Point viewPosition, Displacement offset);
void ClearFloatingNumbers();

} // namespace devilution
