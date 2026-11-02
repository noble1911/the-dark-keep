/**
 * @file keeper/keeper_campaign.cpp
 *
 * Stub of the campaign contract; the campaign work fills this in.
 */
#include "keeper/keeper_campaign.hpp"

namespace devilution::keeper {

namespace {

KeepSetup currentSetup;

} // namespace

const KeepSetup &CurrentSetup()
{
	return currentSetup;
}

void SetCurrentSetup(const KeepSetup &setup)
{
	currentSetup = setup;
}

int CampaignRank()
{
	return 1;
}

} // namespace devilution::keeper
