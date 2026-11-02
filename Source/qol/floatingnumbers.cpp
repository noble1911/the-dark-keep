#include "floatingnumbers.h"

#include <cstdint>
#include <ctime>
#include <deque>
#include <iterator>
#include <optional>
#include <fmt/format.h>
#include <string>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "engine/palette.h"
#include "engine/render/clx_render.hpp"
#include "engine/render/text_render.hpp"
#include "engine/surface.hpp"
#include "options.h"
#include "utils/str_cat.hpp"
#include "utils/surface_to_clx.hpp"

namespace devilution {

namespace {

struct FloatingNumber {
	Point startPos;
	Displacement startOffset;
	Displacement endOffset;
	std::string text;
	uint32_t time;
	uint32_t lastMerge;
	UiFlags style;
	int id;
	bool reverseDirection;
	FloatingNumberIcon icon;
};

std::deque<FloatingNumber> FloatingQueue;

// --- Icons [DARK KEEP] ------------------------------------------------------------------
//
// Hand-drawn 11x11 pixel art ('#' = ink), each in one colour with a dark outline so it
// reads over any floor. Quantised against the live palette, which changes per dungeon
// type, and rebuilt when it does -- as the gem and socket art are.

constexpr int IconSize = 11;

struct IconArt {
	const char *rows[IconSize];
	SDL_Color color;
};

constexpr IconArt IconArts[] = {
	// Dealt: a sword, point up and to the right.
	{ { "..........#",
	      ".........##",
	      "........##.",
	      ".......##..",
	      "..#...##...",
	      "...#.##....",
	      "....##.....",
	      "...#.##....",
	      "..#...#....",
	      ".##........",
	      "##........." },
	    { 232, 196, 96, 255 } },
	// Taken: a skull.
	{ { "...#####...",
	      "..#######..",
	      ".#########.",
	      ".##..#..##.",
	      ".##..#..##.",
	      ".#########.",
	      "..###.###..",
	      "...#####...",
	      "...#.#.#...",
	      "...#.#.#...",
	      "..........." },
	    { 228, 64, 52, 255 } },
	// Heal: a cross.
	{ { "...........",
	      "....###....",
	      "....###....",
	      "....###....",
	      ".#########.",
	      ".#########.",
	      ".#########.",
	      "....###....",
	      "....###....",
	      "....###....",
	      "..........." },
	    { 84, 208, 84, 255 } },
	// Experience: an arrow, up.
	{ { ".....#.....",
	      "....###....",
	      "...#####...",
	      "..#######..",
	      ".#########.",
	      "....###....",
	      "....###....",
	      "....###....",
	      "....###....",
	      "....###....",
	      "..........." },
	    { 104, 148, 255, 255 } },
};

OptionalOwnedClxSpriteList IconSprites[std::size(IconArts)];
unsigned IconSpritesPaletteGeneration = 0;
bool IconSpritesBuilt = false;

/** Nearest palette entry to `color`, skipping the animated low indices. */
uint8_t NearestIconPaletteIndex(SDL_Color color)
{
	int best = 32;
	int64_t bestDistance = INT64_MAX;
	for (int i = 32; i < 256; i++) {
		const SDL_Color &c = logical_palette[i];
		const int rmean = (color.r + c.r) / 2;
		const int dr = color.r - c.r;
		const int dg = color.g - c.g;
		const int db = color.b - c.b;
		const int64_t distance = ((512 + rmean) * dr * dr) / 256 + 4LL * dg * dg + ((767 - rmean) * db * db) / 256;
		if (distance < bestDistance) {
			bestDistance = distance;
			best = i;
		}
	}
	return static_cast<uint8_t>(best);
}

/** The sprite for `icon`, (re)built for the live palette; nullopt for None. */
std::optional<ClxSprite> GetIconSprite(FloatingNumberIcon icon)
{
	if (icon == FloatingNumberIcon::None)
		return std::nullopt;
	if (!IconSpritesBuilt || IconSpritesPaletteGeneration != PaletteGeneration) {
		const uint8_t outline = NearestIconPaletteIndex(SDL_Color { 16, 12, 8, 255 });
		for (size_t i = 0; i < std::size(IconArts); i++) {
			const IconArt &art = IconArts[i];
			const uint8_t ink = NearestIconPaletteIndex(art.color);
			// One pixel of border all round, for the outline.
			OwnedSurface surface { IconSize + 2, IconSize + 2 };
			for (int y = 0; y < IconSize + 2; y++) {
				for (int x = 0; x < IconSize + 2; x++)
					*surface.at(x, y) = 0;
			}
			const auto inked = [&art](int x, int y) {
				return x >= 0 && y >= 0 && x < IconSize && y < IconSize && art.rows[y][x] == '#';
			};
			for (int y = -1; y <= IconSize; y++) {
				for (int x = -1; x <= IconSize; x++) {
					if (inked(x, y)) {
						*surface.at(x + 1, y + 1) = ink;
					} else if (inked(x - 1, y) || inked(x + 1, y) || inked(x, y - 1) || inked(x, y + 1)) {
						*surface.at(x + 1, y + 1) = outline;
					}
				}
			}
			IconSprites[i].emplace(SurfaceToClx(surface, 1, 0));
		}
		IconSpritesBuilt = true;
		IconSpritesPaletteGeneration = PaletteGeneration;
	}
	return (*IconSprites[static_cast<size_t>(icon) - 1])[0];
}

constexpr int IconGap = 2;

void ClearExpiredNumbers()
{
	while (!FloatingQueue.empty()) {
		const FloatingNumber &num = FloatingQueue.front();
		if (num.time > SDL_GetTicks())
			break;

		FloatingQueue.pop_front();
	}
}

GameFontTables GetGameFontSize(UiFlags flags)
{
	if (HasAnyOf(flags, UiFlags::FontSize30))
		return GameFont30;
	if (HasAnyOf(flags, UiFlags::FontSize24))
		return GameFont24;
	return GameFont12;
}

} // namespace

void AddFloatingNumber(Point pos, Displacement offset, std::string text, UiFlags style, int id, bool reverseDirection, FloatingNumberIcon icon)
{
	Displacement endOffset;
	if (!reverseDirection)
		endOffset = { 0, -140 };
	else
		endOffset = { 0, 140 };

	for (auto &num : FloatingQueue) {
		if (id != 0 && num.id == id && (SDL_GetTicks() - static_cast<int>(num.lastMerge)) <= 100) {
			num.text = text;
			num.lastMerge = SDL_GetTicks();
			num.style = style;
			num.startPos = pos;
			num.icon = icon;
			return;
		}
	}
	FloatingNumber num {
		pos, offset, endOffset, text,
		static_cast<uint32_t>(SDL_GetTicks() + 2500),
		static_cast<uint32_t>(SDL_GetTicks()),
		style | UiFlags::Outlined, id, reverseDirection, icon
	};
	FloatingQueue.push_back(num);
}

void DrawFloatingNumbers(const Surface &out, Point viewPosition, Displacement offset)
{
	for (auto &floatingNum : FloatingQueue) {
		Displacement worldOffset = viewPosition - floatingNum.startPos;
		worldOffset = worldOffset.worldToScreen() + offset + Displacement { TILE_WIDTH / 2, -TILE_HEIGHT / 2 } + floatingNum.startOffset;

		if (*GetOptions().Graphics.zoom) {
			worldOffset *= 2;
		}

		Point screenPosition { worldOffset.deltaX, worldOffset.deltaY };

		const int lineWidth = GetLineWidth(floatingNum.text, GetGameFontSize(floatingNum.style));
		// [DARK KEEP] An icon, if any, sits left of the number; the pair is centred together.
		const std::optional<ClxSprite> icon = GetIconSprite(floatingNum.icon);
		const int iconWidth = icon ? icon->width() + IconGap : 0;
		screenPosition.x -= (lineWidth + iconWidth) / 2;
		const uint32_t timeLeft = floatingNum.time - SDL_GetTicks();
		const float mul = 1 - (timeLeft / 2500.0F);
		screenPosition += floatingNum.endOffset * mul;

		if (icon) {
			// ClxDraw takes the bottom-left corner; line the icon up with the 12px text.
			ClxDraw(out, screenPosition + Displacement { 0, icon->height() - 1 }, *icon);
			screenPosition.x += iconWidth;
		}
		DrawString(out, floatingNum.text, Rectangle { screenPosition, { lineWidth, 0 } },
		    { .flags = floatingNum.style });
	}

	ClearExpiredNumbers();
}

void ClearFloatingNumbers()
{
	srand(static_cast<unsigned int>(time(nullptr)));

	FloatingQueue.clear();
}

} // namespace devilution
