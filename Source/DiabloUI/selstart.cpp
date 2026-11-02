#include "selstart.h"

#include "options.h"

namespace devilution {

// [DARK KEEP] Hellfire is mandatory, so the "Enter Hellfire / Switch to Diablo" prompt is gone.
void UiSelStartUpGameOption()
{
	GetOptions().GameMode.gameMode.SetValue(StartUpGameMode::Hellfire);
	GetOptions().Mods.SetHellfireEnabled(true);
	SaveOptions();
}

} // namespace devilution
