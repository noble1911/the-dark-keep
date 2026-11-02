/**
 * @file keeper/keeper_ui.hpp
 *
 * Small drawing helpers shared by the keep's HUD and overlays.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "engine/displacement.hpp"
#include "engine/palette.h"
#include "engine/rectangle.hpp"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "engine/surface.hpp"

namespace devilution::keeper {

constexpr uint8_t PanelFrameColor = PAL16_YELLOW + 4;

inline void DrawFrame(const Surface &out, const Rectangle &rect, uint8_t color)
{
	DrawHorizontalLine(out, rect.position, rect.size.width, color);
	DrawHorizontalLine(out, rect.position + Displacement { 0, rect.size.height - 1 }, rect.size.width, color);
	DrawVerticalLine(out, rect.position, rect.size.height, color);
	DrawVerticalLine(out, rect.position + Displacement { rect.size.width - 1, 0 }, rect.size.height, color);
}

/** A darkened, framed panel over the view. */
inline void DrawPanel(const Surface &out, const Rectangle &rect)
{
	DrawHalfTransparentRectTo(out, rect.position.x, rect.position.y, rect.size.width, rect.size.height);
	DrawHalfTransparentRectTo(out, rect.position.x, rect.position.y, rect.size.width, rect.size.height);
	DrawFrame(out, rect, PanelFrameColor);
}

/** A solid panel in the Keeper screen's style. Index 255 is white in the dungeon palettes, hence the grey. */
inline void DrawDungeonPanel(const Surface &out, const Rectangle &rect)
{
	FillRect(out, rect.position.x, rect.position.y, rect.size.width, rect.size.height, PAL16_GRAY + 14);
	DrawFrame(out, rect, PAL16_RED + 10);
	DrawFrame(out, { rect.position + Displacement { 1, 1 }, { rect.size.width - 2, rect.size.height - 2 } }, PAL16_GRAY + 12);
}

inline int CountLinesOfText(std::string_view text)
{
	return 1 + static_cast<int>(std::count(text.begin(), text.end(), '\n'));
}

/** Cuts `text` down until it fits in `width` pixels, marking the cut. Cuts fall between UTF-8 characters: player names can be anything. */
inline std::string FitText(std::string_view text, int width)
{
	std::string fitted(text);
	if (GetLineWidth(fitted) <= width)
		return fitted;
	while (!fitted.empty() && GetLineWidth(fitted + ".") > width) {
		while (!fitted.empty() && (static_cast<unsigned char>(fitted.back()) & 0xC0) == 0x80)
			fitted.pop_back();
		if (!fitted.empty())
			fitted.pop_back();
	}
	while (!fitted.empty() && fitted.back() == ' ')
		fitted.pop_back();
	return fitted + ".";
}

/** Word-wraps `text` to at most `height` pixels of lines, marking any cut. */
inline std::string FitLines(std::string_view text, int width, int height, int lineHeight)
{
	const std::string wrapped = WordWrapString(text, width);
	const int maxLines = std::max(height / lineHeight, 1);
	size_t end = 0;
	for (int line = 0; line < maxLines; line++) {
		end = wrapped.find('\n', end);
		if (end == std::string::npos)
			return wrapped;
		end++;
	}
	return FitText(wrapped.substr(0, end - 1) + "...", width * maxLines);
}

} // namespace devilution::keeper
