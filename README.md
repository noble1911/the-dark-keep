# The Dark Keep

**A co-op dungeon-master / tower-defence total conversion of the classic Diablo engine**,
built on [DevilutionX](https://github.com/diasurgical/devilutionX).

You don't play the hero: you *are* the dungeon. Parties of AI heroes raid your keep to steal its
treasure and escape. With up to three friends, raise monsters, set traps, lock doors and leave out
fool's gold, then rain spells on the intruders, or possess one of your monsters and fight them yourself.

> Early development. You need your own copies of the original game data. This project contains no
> Blizzard assets, and the game's data files always stay yours to supply.

## Install

1. You need **Diablo and Hellfire** (GOG sells them together). Find the folder holding `DIABDAT.MPQ`
   and the Hellfire files (`hellfire.mpq`, `hfmonk.mpq`, `hfmusic.mpq`, `hfvoice.mpq`).
2. Get the game: a Windows or Nintendo Switch package from this project's CI builds, or build it
   yourself (Windows):

   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\build-desktop.ps1
   powershell -ExecutionPolicy Bypass -File scripts\run-desktop.ps1 -DataDir <folder-with-DIABDAT.MPQ>
   ```

   With a package, copy the data files in next to `devilutionx.exe`.
3. Choose **Campaign** from the main menu, then **1. Old Cellars**.

Saves and settings live in `%AppData%\diasurgical\the-dark-keep\`, separate from any other DevilutionX install.

## How to play

The first two levels show short tips as you go; close them, or just get on with it. Press **H** or
**F1** in the keep at any time for the controls and rules, or hover over anything on the bar along the bottom.

1. **Prepare.** Heroes come in at the entrance and walk to your vault. Pick a monster card and click
   the floor on their way (**V** jumps to the vault). Everything is paid for from the vault's gold.
2. **Open the gates.** When the timer ends the raid begins, or press **R** to start it now.
3. **Fight.** Point at a hero and press **Z**, **X**, **C** or **F** to cast. Alerts along the top say
   who is attacking the Lord or nearing the vault: click one to look there. Arrows at the screen's edge point
   to heroes out of view.
4. **Rebuild.** Each raid you outlast pays gold. Build more during the respite, then do it again.
5. **Spend Renown.** Survive every raid to clear the level. XP from the map raises your keeper level, each
   level gives Renown, and the Keeper screen spends it on new monsters, spells and traps that last for good.

### What you are protecting

- **The Keep Lord** guards the vault. Heroes win by killing him and then escaping: one of them opens a
  Town Portal (kill the caster to interrupt it) and the party runs for it. If any hero gets out after the
  Lord falls, the keep is lost. Wipe out the party first and the Lord rises again for the next raid.
- **The vault** is your treasury. Gold you spend can't be stolen, but it isn't in the vault either. A hero
  who reaches the vault grabs what they can carry and runs for the exit. Kill the thief and the gold comes
  back with a bounty; let one escape and it's gone.

### The heroes

Up to four heroes per raid are real Diablo characters: Warriors with swords, Rogues with bows, Sorcerers
casting Firebolt, and Monks with staves. They drink potions, flee when beaten and cast a real Town Portal. Bigger parties are filled
out with lesser heroes. Rogues may spot and disarm traps, greedy heroes detour to fool's gold they can see,
Warriors bash through locked doors fastest, and badly hurt heroes may lose their nerve, drop their loot and flee.

### Gold, XP and Renown

**Gold is for the current map:** lairs, traps, locks, bait and the Upgrades tab. **XP is permanent:** heroes
slain, raids survived and levels cleared earn it (first clears pay best). Spend Renown on the Keeper screen
(main menu, level list, and between maps) to unlock monsters, spells and trap types and to level up spells and
traps. A spell's level works as in the base game: stronger, longer or wider depending on the spell, and cheaper
in mana. A trap's level adds damage and speeds its rearm. Choose your six spells there too. Respec is free.

**Pop** is the room your monsters take up. Stronger monsters take more. The keep has more room on later
levels and after every raid; sell monsters to make space. Building during a raid is allowed, but not within
sight of a hero, and new monsters take a few seconds to summon.

### Levels, co-op and your own maps

The campaign has 20 levels; each has several raids, more on later levels, and the last raid of a level is the
champions'. Clearing a level opens the next, and what you've unlocked is available on every level.

**Co-op:** up to four masters share one keep and one vault; each has their own mana. Choose Multiplayer on the main
menu. The game uses the host's unlocks and level, and each player keeps the XP they earn. To stay safe for every
player, a co-op game or the Switch limits how many different kinds of monster one keep can load.

**Maps** made in the Map Editor (main menu) go in the `maps` folder of the save directory and can be played from
Custom Map; see [docs/MAP-FORMAT.md](docs/MAP-FORMAT.md).

## Controls

Most keys can be rebound under Settings > Keymapping.

| Input | Action |
|---|---|
| H or F1, or the **?** button | Help: the controls and how to play |
| WASD / arrows / screen edge | Move the camera |
| Mouse wheel, `+` / `-` | Zoom in / out |
| Space / V | Jump to the entrance / the vault |
| Card bar | Click a card (Lairs, Traps & Tricks), then click in the keep. A preview shows where it can go and what it reaches. Upgrade cards buy on click |
| Tab | Next card tab (mouse wheel over the bar scrolls it) |
| 1–9 | The Nth visible card on the current tab |
| Z X C F Q B | Spell bar slots, cast instantly at the cursor (or click the spell bar, then the keep). Spells only work during a raid |
| Left click a hero | Inspect its stats and gear (Esc / right click to close); the raid roster (top right) does the same and jumps the camera to the hero |
| Left click your monster / trap / bait / lock | Select it (shown in the info panel) |
| Left drag | Select a group of your monsters; selling, rally points and patrols apply to all of them |
| Del / Backspace | Sell the selection: full refund outside a raid, part of it during one |
| Right click (monster selected) | Set its rally point: it walks there and guards that spot. Right click its lair to clear |
| Shift + right click (monster selected) | Add a patrol waypoint |
| G / T | Lock a door / place fool's-gold bait |
| Left click | Place / cast the selected tool (or order a possessed monster) |
| Right click / Esc | Cancel the tool (Esc also releases a possessed monster, closes a summary or dismisses a tip) |
| E | Possess the monster under the cursor / release it |
| R | Open the gates early (start the next raid). In co-op: say you are ready, and the host pressing again starts it |
| O | The scout card: what the next raid brings |
| U | Show or hide the heroes' expected route |
| N | Game speed: 1x, 2x, 3x |
| M | Mark a spot for the other masters |
| J | Single player: stop the preparation clock |
| Click an alert | Jump the camera to what it is about |

## For developers

CI builds Windows and Nintendo Switch packages. Developer shortcuts: `--keeper-autostart` (straight into a
single-player keep), `--keeper-host` / `--keeper-join <ip>[:port]` (co-op over TCP without menus); see `scripts/dev/`.

- Design & roadmap: [docs/MOD-PLAN.md](docs/MOD-PLAN.md)
- Game plan / todo: [docs/GAME-PLAN.md](docs/GAME-PLAN.md)

Based on DevilutionX; see [LICENSE.md](LICENSE.md).
