# The Dark Keep: where things stand

Last updated 4 November 2026. Everything here is on `dark-keep-next`. Nobody has played this build by
hand: all testing so far was automated and invisible.

## Play first, and look for

1. Whether placing monsters matters compared with spells and the Lord. Nova, the walls and the Lord have
   all been changed for this. Every changed value is listed in docs/BALANCE.md.
2. Whether you play closer to the harness's "newbie" or "typical". Newbie wins nothing from level 7 up,
   and typical only 1 of 4 on levels 13 and 20. A few runs each, so these are hints.
3. Whether level 1 is now too easy: its numbers predate the softer champions.
4. Whether co-op is too easy. In the few raids run, all monsters survived with the Lord untouched.
5. Whether Catacombs and Caves are bright enough.

## Known problems

- **Co-op:** only ever run on one PC. Real networks are untested. Bots can differ by a few tiles between
  clients (worst seen 10) and are then corrected; life can differ briefly. Monster heroes' fight timers
  aren't sent to a master joining mid-raid.
- **Switch:** the build compiles to a `.nro`, but it has never run on a Switch. Controller support was
  only tested with SDL's virtual controller. The Switch's on-screen keyboard has never been run.
- **Balance, open:** a maxed Shock Wall with all mana wins outright (its mana cost is the lever). Slayers
  are weak because of how they fight, not their price. The Lord still holds level 1's raids 1-4 alone,
  with thieves as the only cost. A maxed Nova or Chain with all mana is unmeasured. Hard is unproven.
- **Wall spells:** raises now only make Wall, Ring and Shock Wall cheaper. Damage and duration don't grow.
- **Hero AI:** the two-unreachable-targets fix was never reproduced live, so it's verified by code only.
- **UI:** only about 24 lair kinds per level leave corpses. Three trap types share an icon. Card bar
  slots differ between games as unlocks grow. The 1-point Lord life gap between clients at raid end is
  unexplained.
- **Startup crashes:** nine early crashes on one build look like a stale incremental build. A clean ASan
  build didn't reproduce them, which supports that but doesn't prove it.

## Last checks

On the final tip's parent `b62f6033c`: builds with and without the test tools and for Switch, level 1 from
a fresh profile, a Nova cast, a 2-client raid, and the parked-Rogue layout. The final commits changed wall
spell values, test tools and docs, measured separately. Not repeated after the balance changes, though
they passed earlier: full victory with medals, 3 and 4 masters, the editor, Continue, barricades, and the
controller walk. ASan ran clean before the last stall and balance changes.

## Design ideas not built

From the genre research, in rough priority: scenario rules in map files, more trap types with effects
that combine, and a skirmish or endless mode. From the player report: stagger heroes at the door, make
the card bar's slots stable, and consider charging for a late respec.

## Test tools

- `scripts/build-desktop.ps1 -Dev` builds with `--keeper-devctl`. A plain build leaves it out.
- `scripts/dev/testrun.ps1`, `coop.ps1` and `ctl.ps1` run invisible games. Each checkout gets its own
  port. A co-op result only counts if netstat shows none of its games connected to 6112.
- `scripts/dev/balance_raids.py` runs raid batches, and `--campaign` plays a level from raid 1 on real
  gold. The game's random numbers repeat from the same start, so runs must vary something.
- `rosterqa`, `spellqa` and `trapqa` sweep every monster, spell and trap through `ctl.ps1`.
- If a build crashes at startup with garbage globals, try `--clean-first` before debugging.
