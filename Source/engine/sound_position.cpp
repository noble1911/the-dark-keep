#include "engine/sound_position.hpp"

#include "engine/sound_defs.hpp"
#include "keeper/keeper.hpp"
#include "levels/gendung.h"
#include "player.h"

namespace devilution {

bool CalculateSoundPosition(Point soundPosition, int *plVolume, int *plPan)
{
	// [DARK KEEP] The master has no body (it is parked off-map); listen from the camera instead.
	const Point playerPosition = keeper::IsActive() ? ViewPosition : Point { MyPlayer->position.tile };
	const Displacement delta = soundPosition - playerPosition;

	const int pan = (delta.deltaX - delta.deltaY) * 256;
	*plPan = std::clamp(pan, PAN_MIN, PAN_MAX);

	const int volume = playerPosition.ApproxDistance(soundPosition) * -64;

	if (volume <= ATTENUATION_MIN)
		return false;

	*plVolume = volume;

	return true;
}

} // namespace devilution
