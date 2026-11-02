/**
 * @file keeper/keeper_coop.cpp
 *
 * See keeper_coop.hpp.
 */
#include "keeper/keeper_coop.hpp"

#include <algorithm>
#include <array>
#include <string_view>

#include <fmt/format.h>

#include "engine/rectangle.hpp"
#include "engine/render/text_render.hpp"
#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_menu.hpp"
#include "keeper/keeper_ui.hpp"
#include "player.h"
#include "utils/ui_fwd.h"

namespace devilution::keeper {

namespace {

/** A joiner arriving late hears who is in the Keeper within this long. */
constexpr int KeeperStatusResendTicks = 5 * 20;
constexpr int BannerWidth = 420;
constexpr int BannerHeight = 22;

std::array<bool, MAX_PLRS> inKeeper {};
bool statusSent = false;
int statusResendTicks = 0;
int startingTicks = 0;
int hostLeftTicks = 0;
bool hostLeft = false;

std::string FormatWait(int ticks)
{
	const int seconds = (ticks + 19) / 20;
	return fmt::format("{}:{:02}", seconds / 60, seconds % 60);
}

} // namespace

void CoopOnLevelLoaded()
{
	inKeeper = {};
	statusSent = false;
	startingTicks = 0;
	hostLeft = false;
	hostLeftTicks = 0;
	CloseKeeperOverlay();
}

void CoopOnGameEnded()
{
	CloseKeeperOverlay();
}

void CoopTick()
{
	if (startingTicks > 0)
		startingTicks--;
	const bool open = IsKeeperOverlayOpen();
	if (open != statusSent || (open && --statusResendTicks <= 0)) {
		statusSent = open;
		statusResendTicks = KeeperStatusResendTicks;
		CoopNoteInKeeper(MyPlayerId, open);
		RequestKeeperStatus(open);
	}
}

void CoopOpenKeeper()
{
	OpenKeeperOverlay();
}

void CoopNoteInKeeper(size_t playerId, bool status)
{
	if (playerId < inKeeper.size())
		inKeeper[playerId] = status;
}

bool CoopIsInKeeper(size_t playerId)
{
	return playerId < inKeeper.size() && inKeeper[playerId];
}

bool CoopAnyMasterInKeeper()
{
	for (size_t id = 0; id < NetPlayerSlots; id++) {
		if (inKeeper[id] && Players[id].plractive)
			return true;
	}
	return false;
}

void CoopNoteStarting(int ticks)
{
	startingTicks = ticks;
}

int CoopStartingTicks()
{
	return startingTicks;
}

std::string CoopGameOverLine()
{
	if (startingTicks > 0)
		return fmt::format(" Next map in {}.", FormatWait(startingTicks));
	for (size_t id = 0; id < NetPlayerSlots; id++) {
		if (id != MyPlayerId && inKeeper[id] && Players[id].plractive)
			return fmt::format(" {} is in the Keeper.", Players[id]._pName);
	}
	return IsJoinedGame() ? " Waiting for the host to choose what is next." : "";
}

void DrawCoopKeeperBanner(const Surface &out)
{
	if (startingTicks <= 0)
		return;
	const int width = std::min(BannerWidth, out.w());
	const Rectangle banner { { (out.w() - width) / 2, out.h() - BannerHeight - 4 }, { width, BannerHeight } };
	DrawPanel(out, banner);
	DrawString(out, fmt::format("Next map in {}. Anything you buy is already saved.", FormatWait(startingTicks)), { banner.position + Displacement { 0, 4 }, { width, 14 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorGold });
}

void CoopNoteHostLeft()
{
	if (hostLeft)
		return;
	hostLeft = true;
	hostLeftTicks = CoopHostLeftNoticeTicks;
	CloseKeeperOverlay();
}

bool CoopHasHostLeft()
{
	return hostLeft;
}

bool CoopHostLeftTick()
{
	return --hostLeftTicks <= 0;
}

void DrawCoopHostLeft(const Surface &out, int hudTop)
{
	if (!hostLeft)
		return;
	const int width = std::min(BannerWidth, out.w());
	const Rectangle banner { { (out.w() - width) / 2, hudTop / 2 - 30 }, { width, 60 } };
	DrawPanel(out, banner);
	DrawString(out, "The host has left, so this game is over.", { banner.position + Displacement { 0, 12 }, { width, 14 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorGold });
	DrawString(out, fmt::format("Back to the menu in {}. (Esc: now)", FormatWait(hostLeftTicks)), { banner.position + Displacement { 0, 32 }, { width, 14 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorWhite });
}

} // namespace devilution::keeper
