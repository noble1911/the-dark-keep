#include "lua/modules/floatingnumbers.hpp"

#include <optional>
#include <string>

#include <sol/sol.hpp>

#include "engine/point.hpp"
#include "lua/metadoc.hpp"
#include "qol/floatingnumbers.h"

namespace devilution {

sol::table LuaFloatingNumbersModule(sol::state_view &lua)
{
	sol::table table = lua.create_table();

	// [DARK KEEP] `icon` names a picture to show beside the number: "dealt", "taken", "heal"
	// or "xp" (see FloatingNumberIcon). Anything else, or none, shows the number alone.
	LuaSetDocFn(table, "add", "(text: string, pos: Point, style: UiFlags, id: integer = 0, reverseDirection: boolean = false, icon: string = nil)",
	    "Add a floating number",
	    [](const std::string &text, Point pos, UiFlags style, std::optional<int> id, std::optional<bool> reverseDirection, std::optional<std::string> icon) {
		    FloatingNumberIcon picture = FloatingNumberIcon::None;
		    if (icon == "dealt")
			    picture = FloatingNumberIcon::Dealt;
		    else if (icon == "taken")
			    picture = FloatingNumberIcon::Taken;
		    else if (icon == "heal")
			    picture = FloatingNumberIcon::Heal;
		    else if (icon == "xp")
			    picture = FloatingNumberIcon::Experience;
		    AddFloatingNumber(pos, { 0, 0 }, text, style, id.value_or(0), reverseDirection.value_or(false), picture);
	    });

	return table;
}

} // namespace devilution
