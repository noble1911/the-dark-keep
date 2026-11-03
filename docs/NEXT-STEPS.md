# The Dark Keep: where things stand

Written at the end of the long build-out session of 2 November 2026. Everything here is on
`dark-keep-next`. Nobody has played this combined build by hand: all testing was automated and invisible.

## Play first, and look for

1. Whether raids feel readable and whether placing monsters matters. The automated player found that
   Nova at the door and Lord upgrades decided early raids. Nova has been cut since; Lord pricing hasn't.
2. Level 1: strong play wins every time and sloppy play often loses.
3. Levels 10 and 20 at full support may now be too easy (6 of 6 each, a small sample).
4. Whether Catacombs and Caves are now bright enough.

## Known problems

- **Unexplained crash:** a copy of the combined build died with a stack overflow (0xC00000FD, in
  ntdll) at 23:47 during a check. Never reproduced or investigated.
- **Raid stall:** a hero can stand shooting across a gap while nothing reaches her. One raid ran about
  5 minutes. See the branch `next3/stallfix` if it isn't merged.
- **Co-op:** only ever run on one PC. Real networks and more than 4 masters are untested. Bots and
  monsters can differ a little between clients. New bot fields (kite, heal, hazard, progress) aren't
  sent to a master joining mid-raid.
- **Switch:** the build compiles to a `.nro`, but it has never run on a Switch. Controller support
  was only tested with SDL's virtual controller.
- **Balance:** see docs/BALANCE.md. Raid types, interest, early call, thieves and Easy and Hard
  have never been measured together. Single-spell strength needs re-measuring after the Inferno, Wave
  and Chain fixes.
- **UI:** the tutorial tip overlaps the scout card, the pause banner overlaps the tip, "Select: Open
  the gat" is clipped on the controller prompt, unlock panel names clip ("Fallen Bl."), and only
  about 24 lair kinds per level leave corpses.

## Not tested on the combined build

Building monsters and traps in the smoke test (its one build command was refused), the editor's F5 and
F10, solo Next, co-op Next with 3 masters, the controller on the Keeper screen, the victory summary and
medals. Each was tested on its own branch unless docs/GAME-PLAN.md says otherwise.

## Design ideas not built

From the genre research, in rough priority: scenario rules in map files, more trap types with effects
that combine, and a skirmish or endless mode. From the player report: stagger heroes at the door,
raise Lord upgrade prices, make the card bar's slots stable, and consider charging for a late respec.

## Test tools

- `scripts/build-desktop.ps1 -Dev` builds with `--keeper-devctl`. A plain build leaves it out.
- `scripts/dev/testrun.ps1`, `coop.ps1` and `ctl.ps1` run invisible games. Each checkout gets its own
  port. A co-op result only counts if netstat shows none of its games connected to 6112.
- `scripts/dev/balance_raids.py` runs raid batches. The game's random numbers repeat from the same
  start, so runs must vary something to be independent.
- `rosterqa`, `spellqa` and `trapqa` sweep every monster, spell and trap through `ctl.ps1`.
- If a build crashes at startup with garbage globals, try `--clean-first` before debugging.
