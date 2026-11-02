# The Dark Keep

**A co-op dungeon-master / tower-defence total conversion of the classic Diablo engine**,
built on [DevilutionX](https://github.com/diasurgical/devilutionX).

You don't play the hero — you *are* the dungeon. Parties of AI heroes raid your keep to
steal its treasure and escape. With up to three friends, raise monster lairs, set traps,
lock doors and leave out fool's gold, then rain spells on the intruders — or possess one
of your monsters and fight them yourself — before they get away with your gold.

> Early development. Requires your own copies of the original game data. This project
> contains no Blizzard assets.

## How it plays

- **One keep, seven raids.** A new game drops you straight into the keep (a fixed
  Cathedral level). You get four minutes to prepare, then raids of Warriors, Rogues and
  Sorcerers arrive at the entrance, with a short respite between each. The seventh raid is
  the champions'.
- **The vault is your treasury.** Everything you build is paid from the vault the heroes
  are trying to rob. Gold you spend can't be stolen — but it isn't in the vault either.
- **Protect the Keep Lord.** Your boss guards the vault. Heroes win by killing him and then
  escaping: one of them channels a Town Portal (kill the caster to interrupt it) and the
  party runs for it. If any hero gets out after the Lord falls, the keep is lost. Wipe out
  the party first and the Lord rises again for the next raid.
- **Thieves.** Heroes who reach the vault grab what they can carry and run for the exit.
  Kill a thief and the gold comes back (plus a bounty); let one escape and it's gone, which
  leaves you less to build with.
- **Raids pay tribute.** Each raid you outlast earns gold for the next.
- **Real heroes.** Up to four heroes per raid are real Diablo characters — Warriors with
  swords, Rogues with bows, Sorcerers casting Firebolt — who drink potions, flee when
  beaten and cast a real Town Portal. Bigger parties are filled out with lesser heroes.
- **Heroes are people too.** Rogues may spot and disarm traps, greedy heroes detour to
  fool's gold they can see, Warriors bash through locked doors fastest, and badly hurt
  heroes may lose their nerve, drop their loot and flee.
- **Co-op.** Up to four masters share one keep and one vault; each has their own mana.

## Controls

| Input | Action |
|---|---|
| WASD / arrows / screen edge | Move the camera |
| Mouse wheel, `+` / `-` | Zoom in / out |
| Space / V | Jump to the entrance / the vault |
| Card bar | Click a card (Lairs, Traps & Tricks), then click in the keep. A preview shows where it can go and what it reaches. Upgrade cards buy on click; the Bestiary lists every monster and when it unlocks |
| Tab | Next card tab (mouse wheel over the bar scrolls it) |
| 1–9 | The Nth visible card on the current tab |
| Z X C F | Spells, cast instantly at the cursor: Lightning Nova, Fire Wall, Guardian, Stone Curse (or click the spell bar, then the keep) |
| Left click a hero | Inspect its stats and gear (Esc / right click to close); the raid roster (top right) does the same and jumps the camera to the hero |
| Esc / click | Dismiss the end-of-raid summary |
| Left click your monster / trap / bait / lock | Select it (shown in the info panel) |
| Left drag | Select a group of your monsters; selling, rally points and patrols apply to all of them |
| Del / Backspace | Sell the selection: full refund outside a raid, half during one |
| Right click (monster selected) | Set its rally point: it walks there and guards that spot. Right click its lair to clear |
| Shift + right click (monster selected) | Add a patrol waypoint (up to 6) |
| G / T | Lock a door / place fool's-gold bait |
| Left click | Place / cast the selected tool (or order a possessed monster) |
| Right click / Esc | Cancel the tool (Esc also releases a possessed monster) |
| E | Possess the monster under the cursor / release it |
| R | Open the gates early (start the next raid) |

Most keys can be rebound under Settings > Keymapping.

The campaign has 20 levels. Clearing one raises your rank, which unlocks more monsters. A level
offers up to 16 kinds at once. Maps made in the Map Editor (main menu) go in the `maps` folder of
the save directory and can be played from Custom Map; see [docs/MAP-FORMAT.md](docs/MAP-FORMAT.md).

Building during a raid is allowed, but not within sight of a hero, and new lairs take a few
seconds to summon. Each monster takes up room in the keep (Pop: Zombie and Fallen 1, Bones and
Archer 2, Brute 4, stronger monsters up to 14); the keep holds 16 in raid 1 of the first level, 2 more per
campaign level and 4 more after every raid. Sell monsters to
make room.

## Building and running (Windows)

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-desktop.ps1
powershell -ExecutionPolicy Bypass -File scripts\run-desktop.ps1 -DataDir <folder-with-DIABDAT.MPQ>
```

Saves and settings live in `%AppData%\diasurgical\the-dark-keep\`, separate from any other
DevilutionX install. CI builds Windows and Nintendo Switch packages.

Developer shortcuts: `--keeper-autostart` (straight into a single-player keep),
`--keeper-host` / `--keeper-join <ip>` (co-op over TCP without menus); see `scripts/dev/`.

- Design & roadmap: [docs/MOD-PLAN.md](docs/MOD-PLAN.md)
- Game plan / todo: [docs/GAME-PLAN.md](docs/GAME-PLAN.md)

Based on DevilutionX — see [LICENSE.md](LICENSE.md).
