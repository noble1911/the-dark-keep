/**
 * @file keeper/keeper_scout.cpp
 *
 * The route preview and scout card. See keeper_scout.hpp.
 */
#include "keeper/keeper_scout.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include <fmt/format.h>

#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_goals.hpp"
#include "keeper/keeper_heroes.hpp"
#include "keeper/keeper_input.hpp"
#include "keeper/keeper_profile.hpp"
#include "keeper/keeper_raid.hpp"
#include "keeper/keeper_raidtypes.hpp"
#include "keeper/keeper_roster.hpp"
#include "keeper/keeper_ui.hpp"
#include "monster.h"

namespace devilution::keeper {

namespace {

bool routePreviewOn = true;

RoutePreview preview;
bool previewValid = false;
uint32_t previewKey = 0;

/** Changes whenever what the route depends on does: the locks, the two ends, the door being hovered. */
uint32_t RouteKey(std::optional<Point> hoverDoor)
{
	const auto mix = [](uint32_t hash, Point tile) {
		return (hash ^ static_cast<uint32_t>(tile.x * 256 + tile.y + 1)) * 16777619U;
	};
	uint32_t key = 2166136261U ^ CurrentSetup().seed;
	key = mix(key, State.entrance);
	key = mix(key, State.vault);
	for (const DoorLock &lock : State.locks) {
		if (lock.active)
			key = mix(key, lock.position);
	}
	return hoverDoor ? mix(key, *hoverDoor) : key;
}

bool RouteHas(const std::vector<Point> &route, Point tile)
{
	return std::find(route.begin(), route.end(), tile) != route.end();
}

void BuildPreview(std::optional<Point> hoverDoor)
{
	preview = {};
	preview.route = EstimateHeroRoute({});
	std::vector<Point> open = EstimateHeroRoute({ .ignoreLocks = true });
	if (open != preview.route)
		preview.withoutLocks = std::move(open);
	for (const DoorLock &lock : State.locks) {
		if (lock.active && RouteHas(preview.route, lock.position))
			preview.bashedLocks.push_back(lock.position);
	}
	if (!hoverDoor)
		return;
	std::vector<Point> locked = EstimateHeroRoute({ .extraLock = hoverDoor });
	if (locked != preview.route) {
		const int extra = static_cast<int>(locked.size()) - static_cast<int>(preview.route.size());
		preview.withHoverLock = std::move(locked);
		preview.hoverVerdict = fmt::format("Turns them away: {:+} steps", extra);
		preview.hoverTurnsThem = true;
	} else if (RouteHas(preview.route, *hoverDoor)) {
		preview.hoverVerdict = "They'd just bash it down";
	} else {
		preview.hoverVerdict = "Not on their route";
	}
}

// ---- scout card --------------------------------------------------------------------

/**
 * How the card reads the odds: the keep's staying power over the party's, in percent, like R in docs/BALANCE.md.
 * Only what the card says: traps, spells and the map aren't in it. Below the first the keep is outmatched, and so on.
 */
constexpr int OddsOutmatchedPercent = 60;
constexpr int OddsRiskyPercent = 100;
constexpr int OddsEvenPercent = 160;
/** At least this wide, the card has room for the big font: a TV at 1280x720 has to be read from the sofa. */
constexpr int BigCardMinUiWidth = 960;

/** Everything on the card that depends on the screen size. */
struct CardLayout {
	bool big;
	int width;
	int margin;
	UiFlags rowFont;
	int titleHeight;
	int rowHeight;
	int textHeight;
	int portraitWidth;
	int portraitHeight;
	/** x of each column in a hero row, from the row's left: name, level, life, damage, armour. */
	std::array<int, 5> columns;
};

constexpr CardLayout SmallCard { false, 312, 6, UiFlags::FontSize12, 16, 14, 14, 21, 30, { { 0, 122, 154, 200, 268 } } };
constexpr CardLayout BigCard { true, 560, 8, UiFlags::FontSize24, 30, 25, 15, 34, 48, { { 0, 262, 318, 394, 490 } } };

const CardLayout &LayoutFor(const Surface &out)
{
	return out.w() >= BigCardMinUiWidth ? BigCard : SmallCard;
}

bool scoutCardOn = true;

/** Identical heroes make one row. */
struct HeroRow {
	ScoutedHero hero;
	int count;
};

struct ScoutInfo {
	bool valid = false;
	uint32_t key = 0;
	std::vector<ScoutedHero> party;
	std::vector<HeroRow> rows;
	int partyLife = 0;
	int partyDamage = 0;
};
ScoutInfo scout;

/** The party only changes with what the host says about the raid, so it isn't rebuilt every frame. */
uint32_t ScoutKey()
{
	return (static_cast<uint32_t>(State.wave) * 16777619U) ^ CurrentSetup().seed ^ (static_cast<uint32_t>(RaidMasters()) << 8) ^ (static_cast<uint32_t>(GameDifficulty()) << 12);
}

const ScoutInfo &GetScoutInfo()
{
	const uint32_t key = ScoutKey();
	if (scout.valid && scout.key == key)
		return scout;
	scout = {};
	scout.valid = true;
	scout.key = key;
	scout.party = ScoutParty();
	for (const ScoutedHero &hero : scout.party) {
		// Potions count for half the hero's life each, as in docs/BALANCE.md.
		scout.partyLife += hero.life + hero.potions * hero.life / 2;
		scout.partyDamage += (hero.minDamage + hero.maxDamage) / 2;
		const auto same = std::find_if(scout.rows.begin(), scout.rows.end(), [&hero](const HeroRow &row) {
			return row.hero.cls == hero.cls && row.hero.full == hero.full;
		});
		if (same != scout.rows.end())
			same->count++;
		else
			scout.rows.push_back({ hero, 1 });
	}
	return scout;
}

struct KeepStrength {
	int monsters = 0;
	int life = 0;
	int damage = 0;
	int lordLife = 0;
};

KeepStrength MeasureKeep()
{
	KeepStrength keep;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (!IsKeepTarget(monster))
			continue;
		if (IsBoss(monster)) {
			keep.lordLife = monster.hitPoints >> 6;
			continue;
		}
		keep.monsters++;
		keep.life += monster.hitPoints >> 6;
		keep.damage += (monster.minDamage + monster.maxDamage) / 2;
	}
	return keep;
}

/** A hero's picture, shrunk once per class and card size. */
const KindImage &PortraitOf(HeroClassKind cls, const CardLayout &layout)
{
	static std::array<std::array<KindImage, NumHeroClasses>, 2> images;
	KindImage &image = images[layout.big ? 1 : 0][static_cast<size_t>(cls)];
	if (!image.built) {
		if (const std::optional<ClxSprite> sprite = HeroPortrait(cls); sprite)
			image = MakeSpriteImage(*sprite, layout.portraitWidth - 2, layout.portraitHeight - 2);
	}
	return image;
}

std::string_view GreedWord(const std::vector<ScoutedHero> &party)
{
	int sight = 0;
	for (const ScoutedHero &hero : party)
		sight += RaidBaitSight(BaitSightOf(hero.cls));
	const int average = party.empty() ? 0 : sight / static_cast<int>(party.size());
	return average >= 9 ? "high" : (average >= 6 ? "fair" : "low");
}

std::string_view CourageWord()
{
	const int fear = CurrentRaidInfo().fearPercent;
	return fear < 100 ? "bold" : (fear > 100 ? "skittish" : "steady");
}

std::string ResistWords()
{
	const RaidTypeInfo &type = CurrentRaidInfo();
	std::string words;
	const auto add = [&words](bool has, std::string_view word) {
		if (has)
			words += fmt::format("{}{}", words.empty() ? "" : ", ", word);
	};
	add((type.resistances & RESIST_FIRE) != 0, "fire");
	add((type.resistances & RESIST_LIGHTNING) != 0, "lightning");
	add((type.resistances & RESIST_MAGIC) != 0, "magic");
	return words.empty() ? "nothing" : words;
}

/** The card's height for what it holds: the heroes are the only part that varies. */
int CardHeight(const CardLayout &layout, const ScoutInfo &info)
{
	return layout.margin * 2 + layout.titleHeight + (GoalsApply() ? 6 : 5) * layout.textHeight + layout.portraitHeight + 4 + layout.textHeight + static_cast<int>(info.rows.size()) * layout.rowHeight
	    + layout.textHeight + 2 * layout.textHeight + 8 + 6 + layout.textHeight;
}

/** The two small buttons in the card's last line. */
Rectangle CloseButton(const Rectangle &card, const CardLayout &layout)
{
	return { { card.position.x + layout.margin, card.position.y + card.size.height - layout.margin - layout.textHeight }, { card.size.width / 2 - layout.margin, layout.textHeight } };
}

Rectangle RouteButton(const Rectangle &card, const CardLayout &layout)
{
	return { { card.position.x + card.size.width / 2, card.position.y + card.size.height - layout.margin - layout.textHeight }, { card.size.width / 2 - layout.margin, layout.textHeight } };
}

void DrawHeroRow(const Surface &out, const CardLayout &layout, const HeroRow &row, int x, int y)
{
	const ScoutedHero &hero = row.hero;
	const UiFlags color = hero.full ? UiFlags::ColorGold : UiFlags::ColorWhite;
	const int nameWidth = layout.columns[1] - 4;
	const std::string name = fmt::format("{} {}{}{}", row.count, hero.full ? "" : "minor ", HeroClassName(hero.cls), row.count > 1 ? "s" : "");
	const auto cell = [&](int column, const std::string &text, UiFlags textColor) {
		DrawString(out, text, { { x + layout.columns[column], y }, { layout.width, layout.rowHeight } }, { .flags = layout.rowFont | textColor });
	};
	cell(0, FitText(name, nameWidth), color);
	cell(1, fmt::format("{}", hero.level), UiFlags::ColorWhite);
	cell(2, fmt::format("{}", hero.life), UiFlags::ColorRed);
	cell(3, fmt::format("{}-{}", hero.minDamage, hero.maxDamage), UiFlags::ColorOrange);
	cell(4, fmt::format("{}", hero.armor), UiFlags::ColorBlue);
}

} // namespace

bool IsRoutePreviewVisible()
{
	return routePreviewOn && (State.phase == Phase::Setup || State.phase == Phase::Breather);
}

void ToggleRoutePreview()
{
	routePreviewOn = !routePreviewOn;
}

bool IsRoutePreviewOn()
{
	return routePreviewOn;
}

const RoutePreview &GetRoutePreview(std::optional<Point> hoverDoor)
{
	const uint32_t key = RouteKey(hoverDoor);
	if (!previewValid || key != previewKey) {
		BuildPreview(hoverDoor);
		previewKey = key;
		previewValid = true;
	}
	return preview;
}

bool IsScoutCardOn()
{
	return scoutCardOn;
}

void ToggleScoutCard()
{
	scoutCardOn = !scoutCardOn;
}

std::optional<Rectangle> ScoutCardRect(const Surface &out, int hudTop)
{
	if (!scoutCardOn || (State.phase != Phase::Setup && State.phase != Phase::Breather) || IsRaidSummaryVisible() || IsAwaitingKeepSnapshot())
		return std::nullopt;
	const CardLayout &layout = LayoutFor(out);
	const int height = std::min(CardHeight(layout, GetScoutInfo()), hudTop - 8);
	return Rectangle { { out.w() - layout.width - 6, 6 }, { layout.width, height } };
}

void DrawScoutCard(const Surface &out, int hudTop, Point mouse)
{
	const std::optional<Rectangle> rect = ScoutCardRect(out, hudTop);
	if (!rect)
		return;
	const CardLayout &layout = LayoutFor(out);
	const ScoutInfo &info = GetScoutInfo();
	DrawPanel(out, *rect);

	const int x = rect->position.x + layout.margin;
	const int inner = rect->size.width - 2 * layout.margin;
	int y = rect->position.y + layout.margin;
	const bool champions = State.wave >= NumWaves();
	const UiFlags titleFont = layout.big ? UiFlags::FontSize24 : UiFlags::FontSize12;
	const RaidTypeInfo &type = CurrentRaidInfo();
	const std::string title = champions ? fmt::format("Raid {}: {} champions", State.wave, type.name) : fmt::format("Raid {} of {}: {}", State.wave, NumWaves(), type.name);
	DrawString(out, title, { { x, y }, { inner, layout.titleHeight } }, { .flags = titleFont | UiFlags::ColorGold });
	// How hard this game is set, whoever chose it. Normal on one's own needs no saying.
	const int masters = RaidMasters();
	const Difficulty difficulty = GameDifficulty();
	if (masters > 1 || difficulty != Difficulty::Normal) {
		const std::string setting = masters > 1 ? fmt::format("{}, {} masters", DifficultyName(difficulty), masters) : std::string(DifficultyName(difficulty));
		DrawString(out, setting, { { x, y }, { inner, layout.textHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignRight | UiFlags::ColorWhite });
	}
	y += layout.titleHeight;
	DrawString(out, FitLines(type.blurb, inner, 2 * layout.textHeight, layout.textHeight), { { x, y }, { inner, 2 * layout.textHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorOrange, .lineHeight = layout.textHeight });
	y += 2 * layout.textHeight;
	DrawString(out, fmt::format("{} {}.  Greed: {}.  Courage: {}.", info.party.size(), info.party.size() == 1 ? "hero" : "heroes", GreedWord(info.party), CourageWord()),
	    { { x, y }, { inner, layout.textHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite });
	y += layout.textHeight;
	DrawString(out, fmt::format("Resists: {}.", ResistWords()), { { x, y }, { inner, layout.textHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite });
	y += layout.textHeight;

	// A picture for every hero, gold framed for the full ones. They shrink together if a big party wouldn't fit.
	const int count = static_cast<int>(info.party.size());
	const int cellWidth = count > 0 ? std::min(layout.portraitWidth, inner / count) : layout.portraitWidth;
	for (int i = 0; i < count; i++) {
		const ScoutedHero &hero = info.party[i];
		const Rectangle cell { { x + i * cellWidth, y }, { cellWidth - 1, layout.portraitHeight } };
		FillRect(out, cell.position.x, cell.position.y, cell.size.width, cell.size.height, 0);
		if (cellWidth == layout.portraitWidth) {
			DrawKindImage(out.subregion(cell.position.x + 1, cell.position.y + 1, layout.portraitWidth - 2, layout.portraitHeight - 2), PortraitOf(hero.cls, layout));
		} else {
			// Too narrow for the whole figure: it stands cut off at the cell's edges, centred.
			const KindImage &image = PortraitOf(hero.cls, layout);
			const int inset = std::max(image.width - (cell.size.width - 2), 0) / 2;
			const int drawn = std::min(image.width, cell.size.width - 2);
			for (int row = 0; row < image.height && row < cell.size.height - 2; row++) {
				for (int column = 0; column < drawn; column++) {
					const uint8_t pixel = image.pixels[static_cast<size_t>(row) * image.width + column + inset];
					if (pixel != 0)
						out.SetPixel({ cell.position.x + 1 + column, cell.position.y + 1 + row }, pixel);
				}
			}
		}
		DrawFrame(out, cell, hero.full ? PanelFrameColor : PAL16_GRAY + 8);
	}
	y += layout.portraitHeight + 4;

	const std::array<std::string_view, 5> headers { "Hero", "Lvl", "Life", "Damage", "AC" };
	for (size_t i = 0; i < headers.size(); i++)
		DrawString(out, headers[i], { { x + layout.columns[i], y }, { layout.width, layout.textHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorUiSilverDark });
	y += layout.textHeight;
	for (const HeroRow &row : info.rows) {
		DrawHeroRow(out, layout, row, x, y);
		y += layout.rowHeight;
	}
	y += 4;

	// The party against what stands in the keep right now.
	const KeepStrength keep = MeasureKeep();
	const int odds = info.partyLife * info.partyDamage > 0 ? static_cast<int>(100LL * keep.life * keep.damage / (static_cast<int64_t>(info.partyLife) * info.partyDamage)) : 0;
	DrawString(out, fmt::format("Party: life {}, damage {}", info.partyLife, info.partyDamage), { { x, y }, { inner, layout.textHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorRed });
	y += layout.textHeight;
	DrawString(out, fmt::format("Keep: {} {}, life {}, damage {}", keep.monsters, keep.monsters == 1 ? "monster" : "monsters", keep.life, keep.damage), { { x, y }, { inner, layout.textHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
	y += layout.textHeight;
	const std::string_view verdict = keep.monsters == 0 ? "No defenders yet!"
	    : odds < OddsOutmatchedPercent                  ? "Outmatched"
	    : odds < OddsRiskyPercent                       ? "Risky"
	    : odds < OddsEvenPercent                        ? "An even fight"
	                                                    : "In your favour";
	const UiFlags verdictColor = odds < OddsRiskyPercent ? UiFlags::ColorRed : (odds < OddsEvenPercent ? UiFlags::ColorOrange : UiFlags::ColorGold);
	const int barWidth = inner / 2;
	const Rectangle bar { { x, y + 2 }, { barWidth, 8 } };
	FillRect(out, bar.position.x, bar.position.y, bar.size.width, bar.size.height, PAL16_RED + 6);
	const int fill = barWidth * odds / std::max(odds + 100, 1);
	if (fill > 0)
		FillRect(out, bar.position.x, bar.position.y, fill, bar.size.height, PAL16_YELLOW + 2);
	DrawFrame(out, bar, PAL16_GRAY + 8);
	DrawString(out, fmt::format("{} (est.)", verdict), { { x + barWidth + 6, y }, { inner - barWidth - 6, layout.textHeight } }, { .flags = UiFlags::FontSize12 | verdictColor });
	y += layout.textHeight;

	// Spend it or guard it.
	DrawString(out, fmt::format("Call now: +{} gold.  Vault interest: +{} gold.", EarlyCallBonusNow(), VaultInterestOn(State.vaultGold)), { { x, y }, { inner, layout.textHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
	y += layout.textHeight;

	if (GoalsApply()) {
		// Lost goals in red, medals already won on this level in gold.
		int goalX = x;
		for (size_t goal = 0; goal < NumLevelGoals; goal++) {
			const std::string_view name = GoalInfo(goal).name;
			const bool lost = (FailedGoals() & (1U << goal)) != 0;
			const bool held = (ProfileMedals(CurrentSetup().campaignLevel) & (1U << goal)) != 0;
			DrawString(out, name, { { goalX, y }, { inner, layout.textHeight } }, { .flags = UiFlags::FontSize12 | (lost ? UiFlags::ColorRed : (held ? UiFlags::ColorGold : UiFlags::ColorWhite)) });
			goalX += GetLineWidth(name) + 10;
		}
		y += layout.textHeight;
	}

	const Rectangle close = CloseButton(*rect, layout);
	const Rectangle route = RouteButton(*rect, layout);
	DrawString(out, fmt::format("{}: hide card", KeyLabel(KeeperAction::ScoutCard)), { close.position, close.size },
	    { .flags = UiFlags::FontSize12 | (close.contains(mouse) ? UiFlags::ColorGold : UiFlags::ColorWhite) });
	DrawString(out, fmt::format("{}: route {}", KeyLabel(KeeperAction::RoutePreview), IsRoutePreviewOn() ? "on" : "off"), { route.position, route.size },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignRight | (route.contains(mouse) ? UiFlags::ColorGold : UiFlags::ColorWhite) });
}

bool OnScoutCardClick(const Surface &out, int hudTop, Point mouse)
{
	const std::optional<Rectangle> rect = ScoutCardRect(out, hudTop);
	if (!rect || !rect->contains(mouse))
		return false;
	const CardLayout &layout = LayoutFor(out);
	if (CloseButton(*rect, layout).contains(mouse))
		ToggleScoutCard();
	else if (RouteButton(*rect, layout).contains(mouse))
		ToggleRoutePreview();
	return true;
}

} // namespace devilution::keeper
