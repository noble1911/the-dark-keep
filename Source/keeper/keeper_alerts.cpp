/**
 * @file keeper/keeper_alerts.cpp
 *
 * Banners, live alerts and their sounds. See keeper_alerts.hpp.
 */
#include "keeper/keeper_alerts.hpp"

#include <algorithm>
#include <array>
#include <string_view>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include <fmt/format.h>

#include "effects.h"
#include "engine/palette.h"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "keeper/keeper_bots.hpp"
#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_editor.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_profile.hpp"
#include "keeper/keeper_tutorial.hpp"
#include "keeper/keeper_ui.hpp"
#include "monster.h"
#include "player.h"
#include "tables/itemdat.h"

namespace devilution::keeper {

namespace {

/** A hero this few steps from the vault sets off the warning; it clears further out, so it doesn't flicker. */
constexpr int VaultWarningSteps = 14;
constexpr int VaultClearSteps = 20;
constexpr int VaultReachedSteps = 2;
/** Heroes this close to the Lord are the ones hurting him: Rogues and Sorcerers strike from a distance. */
constexpr int LordAttackerTiles = 9;
/** The Lord counts as under attack for this long after he last lost life. */
constexpr int LordAlertTicks = 3 * 20;
/** The same warning sound again no sooner than this. */
constexpr int AlertSoundTicks = 15 * 20;
constexpr uint32_t BannerMillis = 4500;

constexpr int BannerHeight = 74;
constexpr int BannerMaxWidth = 420;
constexpr int AlertRowHeight = 32;
constexpr int AlertIconSize = 28;
constexpr int AlertMaxWidth = 380;
constexpr size_t MaxAlertRows = 3;

struct Banner {
	std::string title;
	std::string text;
	UiFlags color = UiFlags::ColorGold;
	/** The raid's heroes, by class. */
	bool showParty = false;
	std::array<int, NumHeroClasses> party {};
	uint32_t until = 0;
};

struct Alert {
	std::string text;
	UiFlags color;
	UiIcon icon;
	Point tile;
};

Banner banner;
std::vector<Alert> alerts;
std::vector<Rectangle> alertRects;
std::vector<Point> alertTiles;

uint32_t ticks = 0;
bool started = false;
Phase lastPhase = Phase::Setup;
int lastLordHp = 0;
Point lordTile;
uint32_t lordHurtTick = 0;
bool lordWasHurt = false;
std::string lordAttackers;
bool lastBossDown = false;
bool vaultWarned = false;
uint32_t nextVaultSound = 0;
uint32_t nextLordSound = 0;
int spellsCast = 0;
int levelAtStart = 1;
int lastKeeperLevel = 1;
std::vector<UnlockItem> affordableAtStart;

constexpr std::array<int, NumHeroClasses> HeroIcons { ICURS_SHORT_SWORD, ICURS_SHORT_BOW, ICURS_SHORT_STAFF };

std::string_view ClassWord(HeroClassKind cls)
{
	return HeroClassName(cls);
}

std::string WithArticle(HeroClassKind cls)
{
	const std::string_view name = ClassWord(cls);
	return fmt::format("{} {}", name.empty() || name[0] == 'A' || name[0] == 'E' || name[0] == 'I' || name[0] == 'O' || name[0] == 'U' ? "an" : "a", name);
}

/** "a Warrior", "a Warrior and a Rogue", or "3 heroes". */
std::string DescribeHeroes(const std::vector<HeroMark> &heroes)
{
	std::array<int, NumHeroClasses> counts {};
	for (const HeroMark &hero : heroes)
		counts[static_cast<size_t>(hero.cls)]++;
	std::vector<std::string> parts;
	for (size_t i = 0; i < NumHeroClasses; i++) {
		if (counts[i] == 1)
			parts.push_back(WithArticle(static_cast<HeroClassKind>(i)));
		else if (counts[i] > 1)
			parts.push_back(fmt::format("{} {}s", counts[i], ClassWord(static_cast<HeroClassKind>(i))));
	}
	if (parts.empty())
		return {};
	if (parts.size() == 1)
		return parts[0];
	if (parts.size() == 2)
		return fmt::format("{} and {}", parts[0], parts[1]);
	return fmt::format("{} heroes", heroes.size());
}

void ShowBanner(std::string title, std::string text, UiFlags color)
{
	banner = {};
	banner.title = std::move(title);
	banner.text = std::move(text);
	banner.color = color;
	banner.until = SDL_GetTicks() + BannerMillis;
}

void OnPhaseChanged(Phase from, Phase to)
{
	if (to == Phase::Raid) {
		const bool champions = State.wave >= NumWaves();
		ShowBanner(champions ? fmt::format("Raid {}: the champions!", State.wave) : fmt::format("Raid {}", State.wave),
		    fmt::format("{} of {}: guard the Lord and the vault", State.wave, NumWaves()), UiFlags::ColorRed);
		banner.showParty = true;
		for (const HeroClassKind cls : PartyForWave(State.wave))
			banner.party[static_cast<size_t>(cls)]++;
		PlaySFX(SfxID::DoorOpen);
		vaultWarned = false;
		lordWasHurt = false;
		lordAttackers.clear();
	} else if (to == Phase::Victory) {
		PlaySFX(SfxID::QuestDone);
	} else if (to == Phase::Defeat) {
		PlaySFX(SfxID::WarriorDeath);
	} else if (from == Phase::Raid && to == Phase::Breather) {
		PlaySFX(SfxID::ItemGold);
	}
}

/** Works out who is doing what right now. */
void UpdateAlerts(const std::vector<HeroMark> &heroes)
{
	alerts.clear();
	if (State.phase != Phase::Raid) {
		// Not drawn from now on, so their last rectangles mustn't go on taking clicks.
		alertRects.clear();
		alertTiles.clear();
		return;
	}

	const Monster *boss = FindBoss();
	if (State.bossDown) {
		const std::optional<Point> portal = PortalPosition();
		const auto caster = std::find_if(heroes.begin(), heroes.end(), [](const HeroMark &hero) { return hero.portalCaster; });
		if (portal) {
			alerts.push_back({ "The portal is open: stop the heroes reaching it!", UiFlags::ColorRed, ItemIcon(ICURS_SCROLL_OF), *portal });
		} else if (caster != heroes.end()) {
			alerts.push_back({ fmt::format("Kill the {} opening a portal ({}%)!", ClassWord(caster->cls), std::max(PortalChannelPercent(), 0)), UiFlags::ColorRed, ItemIcon(HeroIcons[static_cast<size_t>(caster->cls)]), caster->tile });
		} else if (!heroes.empty()) {
			alerts.push_back({ "The Lord is down: stop the heroes escaping!", UiFlags::ColorRed, MonsterIcon(MT_NBLACK), heroes.front().tile });
		}
	} else if (boss != nullptr && lordWasHurt && ticks - lordHurtTick < LordAlertTicks) {
		const int percent = boss->maxHitPoints > 0 ? 100 * boss->hitPoints / boss->maxHitPoints : 0;
		alerts.push_back({ lordAttackers.empty() ? fmt::format("The Keep Lord is under attack! ({}%)", percent) : fmt::format("The Lord is under attack by {}! ({}%)", lordAttackers, percent), UiFlags::ColorRed, MonsterIcon(MT_NBLACK), boss->position.tile });
	}

	// Thieves on the way out: there is still time to get the gold back.
	const auto thief = std::max_element(heroes.begin(), heroes.end(), [](const HeroMark &a, const HeroMark &b) { return a.carried < b.carried; });
	if (thief != heroes.end() && thief->carried > 0)
		alerts.push_back({ fmt::format("A {} runs for the exit with {} gold!", ClassWord(thief->cls), thief->carried), UiFlags::ColorGold, ItemIcon(ICURS_GOLD_MEDIUM), thief->tile });

	// Heroes closing on the vault. Once the Lord is down, the portal is all that matters.
	if (State.bossDown) {
		if (alerts.size() > MaxAlertRows)
			alerts.resize(MaxAlertRows);
		return;
	}
	std::vector<HeroMark> near;
	int nearest = VaultClearSteps + 1;
	for (const HeroMark &hero : heroes) {
		if (hero.carried != 0 || hero.fleeing)
			continue;
		const int steps = GoalDistance(HeroGoal::Vault, hero.tile);
		nearest = std::min(nearest, steps);
		if (steps <= VaultWarningSteps)
			near.push_back(hero);
	}
	if (nearest > VaultClearSteps)
		vaultWarned = false;
	if (!near.empty()) {
		if (!vaultWarned && ticks >= nextVaultSound) {
			PlaySFX(SfxID::ItemGold);
			nextVaultSound = ticks + AlertSoundTicks;
		}
		vaultWarned = true;
		const std::string who = DescribeHeroes(near);
		alerts.push_back({ nearest <= VaultReachedSteps ? fmt::format("{} at the vault!", who) : fmt::format("{} near the vault!", who), UiFlags::ColorOrange, ItemIcon(ICURS_GOLD_LARGE), State.vault });
	}
	if (alerts.size() > MaxAlertRows)
		alerts.resize(MaxAlertRows);
}

void UpdateLord(const std::vector<HeroMark> &heroes)
{
	const Monster *boss = FindBoss();
	// One blow can take him from whole to gone, so a Lord who has just vanished counts as hurt too.
	const int hp = boss != nullptr ? boss->hitPoints : 0;
	if (boss != nullptr)
		lordTile = boss->position.tile;
	if (State.phase == Phase::Raid && lastLordHp > 0 && hp < lastLordHp) {
		lordWasHurt = true;
		lordHurtTick = ticks;
		std::vector<HeroMark> close;
		for (const HeroMark &hero : heroes) {
			if (hero.tile.WalkingDistance(lordTile) <= LordAttackerTiles)
				close.push_back(hero);
		}
		if (!close.empty())
			lordAttackers = DescribeHeroes(close);
		if (ticks >= nextLordSound) {
			PlaySFX(SfxID::TriggerTrap);
			nextLordSound = ticks + AlertSoundTicks;
		}
	}
	lastLordHp = hp;
}

std::vector<UnlockItem> AffordableUnlocks()
{
	std::vector<UnlockItem> items;
	for (size_t kind = 0; kind < NumLairKinds; kind++) {
		if (!OwnsKind(kind) && CanUnlockKind(kind))
			items.push_back({ UnlockItem::Type::Monster, kind });
	}
	for (size_t spell = 0; spell < NumSpellKinds; spell++) {
		if (OwnedSpellLevel(spell) == 0 && CanRaiseSpell(spell))
			items.push_back({ UnlockItem::Type::Spell, spell });
	}
	for (size_t trap = 0; trap < NumTrapKinds; trap++) {
		if (OwnedTrapLevel(trap) == 0 && CanRaiseTrap(trap))
			items.push_back({ UnlockItem::Type::Trap, trap });
	}
	return items;
}

} // namespace

std::vector<HeroMark> LivingHeroMarks()
{
	std::vector<HeroMark> heroes;
	for (size_t i = 0; i < MaxBots && FirstBotId + i < Players.size(); i++) {
		const Player &bot = Players[FirstBotId + i];
		if (const std::optional<BotSummary> summary = GetBotSummary(bot); summary)
			heroes.push_back({ bot.position.tile, summary->cls, summary->carried, summary->fleeing, IsPortalCaster(bot) });
	}
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (monster.isInvalid || monster.hitPoints <= 0 || monster.mode == MonsterMode::Death)
			continue;
		if (const std::optional<HeroSummary> summary = GetHeroSummary(monster); summary)
			heroes.push_back({ monster.position.tile, summary->cls, summary->carried, summary->fleeing, IsPortalCaster(monster) });
	}
	return heroes;
}

void AlertsOnLevelLoaded()
{
	banner = {};
	alerts.clear();
	alertRects.clear();
	alertTiles.clear();
	ticks = 0;
	started = false;
	lastLordHp = 0;
	lordWasHurt = false;
	lordAttackers.clear();
	lastBossDown = false;
	vaultWarned = false;
	nextVaultSound = 0;
	nextLordSound = 0;
	spellsCast = 0;
	levelAtStart = KeeperLevel();
	lastKeeperLevel = levelAtStart;
	affordableAtStart = AffordableUnlocks();
}

void AlertsNoteSpellCast()
{
	spellsCast++;
}

int SpellsCastThisMap()
{
	return spellsCast;
}

std::string LordSlayers()
{
	return lordAttackers;
}

int KeeperLevelAtMapStart()
{
	return levelAtStart;
}

std::vector<UnlockItem> NewlyAffordableUnlocks()
{
	std::vector<UnlockItem> items = AffordableUnlocks();
	items.erase(std::remove_if(items.begin(), items.end(), [](const UnlockItem &item) {
		return std::find(affordableAtStart.begin(), affordableAtStart.end(), item) != affordableAtStart.end();
	}),
	    items.end());
	return items;
}

void AlertsTick()
{
	if (IsEditorActive())
		return;
	ticks++;
	if (IsAwaitingKeepSnapshot()) {
		lastPhase = State.phase;
		return;
	}
	if (!started) {
		started = true;
		lastPhase = State.phase;
		if (!IsEditorTestRun())
			ShowBanner(CurrentSetup().mode == GameMode::Campaign ? fmt::format("Level {}: {}", CurrentSetup().campaignLevel, CurrentRules().name) : std::string("Custom Map"),
			    fmt::format("Survive {} raids. Guard the Lord and the vault.", NumWaves()), UiFlags::ColorGold);
	}
	if (State.phase != lastPhase) {
		OnPhaseChanged(lastPhase, State.phase);
		lastPhase = State.phase;
	}
	const std::vector<HeroMark> heroes = LivingHeroMarks();
	UpdateLord(heroes);
	if (State.bossDown && !lastBossDown && State.phase == Phase::Raid) {
		ShowBanner("The Keep Lord has fallen!", "Stop the heroes before they portal out", UiFlags::ColorRed);
		PlaySFX(SfxID::WarriorDeath);
	}
	lastBossDown = State.bossDown;
	UpdateAlerts(heroes);
}

int DrawBanner(const Surface &out, int left, int width, int y)
{
	if (banner.until == 0 || SDL_GetTicks() >= banner.until)
		return y;
	const int boxWidth = std::min(width, BannerMaxWidth);
	const Rectangle rect { { left + (width - boxWidth) / 2, y }, { boxWidth, BannerHeight } };
	DrawDungeonPanel(out, rect);
	DrawString(out, FitText(banner.title, boxWidth - 8), { rect.position + Displacement { 0, 6 }, { boxWidth, 30 } }, { .flags = UiFlags::FontSize24 | UiFlags::AlignCenter | banner.color });

	int rowY = rect.position.y + 38;
	if (banner.showParty) {
		// A picture per class with how many: the party at a glance.
		constexpr int IconWidth = 14;
		int classes = 0;
		for (const int count : banner.party)
			classes += count > 0 ? 1 : 0;
		const int slot = (boxWidth - 8) / std::max(classes, 1);
		int x = rect.position.x + 4;
		for (size_t i = 0; i < NumHeroClasses; i++) {
			if (banner.party[i] == 0)
				continue;
			const std::string label = FitText(fmt::format("{} {}{}", banner.party[i], ClassWord(static_cast<HeroClassKind>(i)), banner.party[i] > 1 ? "s" : ""), slot - IconWidth - 4);
			const int width = IconWidth + 2 + GetLineWidth(label);
			const int startX = x + (slot - width) / 2;
			DrawUiIcon(out.subregion(startX, rowY - 1, IconWidth, 32), ItemIcon(HeroIcons[i]));
			DrawString(out, label, { { startX + IconWidth + 2, rowY + 8 }, { slot, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite });
			x += slot;
		}
	} else {
		DrawString(out, FitText(banner.text, boxWidth - 8), { { rect.position.x, rowY + 6 }, { boxWidth, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorWhite });
	}
	return y + BannerHeight + 4;
}

int DrawAlerts(const Surface &out, int left, int width, int y, Point mouse)
{
	alertRects.clear();
	alertTiles.clear();
	const int boxWidth = std::min(width, AlertMaxWidth);
	const int x = left + (width - boxWidth) / 2;
	for (const Alert &alert : alerts) {
		const Rectangle rect { { x, y }, { boxWidth, AlertRowHeight } };
		const bool hovered = rect.contains(mouse);
		DrawDungeonPanel(out, rect);
		if (hovered)
			DrawFrame(out, rect, PAL16_YELLOW + 2);
		const Rectangle image { rect.position + Displacement { 2, 2 }, { AlertIconSize, AlertIconSize } };
		FillRect(out, image.position.x, image.position.y, image.size.width, image.size.height, 0);
		DrawUiIcon(out.subregion(image.position.x, image.position.y, image.size.width, image.size.height), alert.icon);
		const int textX = image.position.x + AlertIconSize + 4;
		const int textWidth = rect.position.x + boxWidth - textX - 4;
		DrawString(out, FitLines(alert.text, textWidth, 28, 13), { { textX, y + 3 }, { textWidth, 28 } }, { .flags = UiFlags::FontSize12 | alert.color, .lineHeight = 13 });
		alertRects.push_back(rect);
		alertTiles.push_back(alert.tile);
		y += AlertRowHeight + 2;
	}
	return y + (alerts.empty() ? 0 : 2);
}

std::optional<Point> AlertsOnClick(Point mouse)
{
	for (size_t i = 0; i < alertRects.size(); i++) {
		if (alertRects[i].contains(mouse))
			return alertTiles[i];
	}
	return std::nullopt;
}

bool AlertsIsMouseOver(Point mouse)
{
	return AlertsOnClick(mouse).has_value();
}

} // namespace devilution::keeper
