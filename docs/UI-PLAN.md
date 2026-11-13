# The Dark Keep: UI review, rules and plan

Written 11 November 2026 from the v0.2 build (`a35e0a3bc`). Work started the same day.
Screens were captured invisibly at 3440x1440 with a profile that owns most monsters.

## What is wrong today

| Problem | Measured |
| --- | --- |
| The bottom bar is too tall | 144 UI pixels at every size. That is 20% of the screen at 720p, 1440p and 4K, and 13% at 1080p. |
| The bar blacks out the full width | Its content stops at 1280 UI pixels, but the black band runs edge to edge. On an ultrawide a quarter of the band is empty black. |
| The spell bar sits on top of the bar | Six 60-pixel slots float above the band, so the bottom right corner loses 29% of the height. |
| The info panel is always open | With nothing hovered it shows the controls, which the help screen already covers. |
| One card per monster kind | The Lairs tab lists every unlocked kind in table order. The game has 111 kinds, about 13 fit, and only the first 9 have keys. Reaching the last kind takes eight clicks on a 12-pixel arrow. |
| The Keeper screen is one long list | 39 families, one per row, five rows on screen. Half the list's width is empty and the details panel is mostly blank. |
| Front-end menus are not scaled | At 3440x1440 the level list is a 640x480 box in the middle of the screen. This is the opposite problem and is cheap to fix alongside. |

## Rules

These are the rules every UI change is checked against. Every agent working on the UI reads them first.

1. **Screen budget.** The bottom strip takes at most 10% of the screen height, at every supported size. Two things may go over: the orbs at the strip's ends may poke above it, as Diablo's do, and one status line of at most 2.5% may sit at the top right.
2. **No dead black.** A panel is only as big as what it holds. The keep stays visible beside and between panels.
3. **One strip.** Status, cards and spells share one row at the bottom. Nothing permanent floats above it.
4. **Detail on demand.** Descriptions, stats and controls appear on hover, selection or the help key, and go away afterwards.
5. **Two actions to anything.** Any monster, trap, upgrade or spell the player owns is reachable in at most two clicks or key presses. No list needs paging by arrow.
6. **Families first.** Monsters are shown by family, with tiers inside the family. A flat list of kinds is never shown during play.
7. **Stable slots.** A card keeps its place and key for the whole level, and between levels where it can.
8. **Every visible card has a key.** If it is on the strip, a key or controller press picks it.
9. **One scale rule.** The UI scale comes from one function, with a player setting to override it. Menus use it too.
10. **Mouse and controller alike.** Every change works with both, and the tutorial's pointers still land on the right thing.
11. **Proof by screenshot.** A change is done when invisible captures at 1280x720, 1920x1080, 3440x1440 and Switch TV scale all meet rules 1 to 3, with a profile that owns every monster.
12. **No game rule changes.** Prices, unlocks and the list of kinds a level offers stay as they are. That list is sent over the network, so co-op depends on it.

## Plan

### Step 1: Slim strip
**Done** on branch `ui/strip`. The strip is 72 UI pixels (10% at 720p, 1440p and TV scale, 6.7% at 1080p), in separate panels.
Not done: the map editor keeps its own 128-pixel bar, and the view is still centred for the old panel height.

Halve the bar from 144 to 72 UI pixels and make it a set of panels instead of a band.

- Cards shrink from 62x84 to about 46x56: portrait, key and price. The name moves to the hover text.
- The status column becomes two lines: phase and clock, then raid, pop, vault and mana as icons with numbers.
- The spell slots move into the strip at 44 pixels, to the right of the cards.
- The info panel is replaced by a tooltip above the hovered or selected card.
- The scout line, news banners, raid roster and inspect panel are re-anchored to the new top of the strip.
- Files: mostly `Source/keeper/keeper.cpp`, plus the scout, tutorial, controller and lobby files that read its rectangles.

### Step 2: Families on the strip
**Done** on branch `ui/families`. One card to a family; the wheel, a right click or the card's key again opens its tiers above the card.
Up to twelve families share one Lairs page with dividers; more, and each dungeon has its own tab (the tab key steps through them). Keys are 1 to 0, then [ and ].
A narrow bar (TV scale) shows one tab name between two arrows. A page only scrolls in a window too small for cards 36 pixels wide.

Depends on step 1, since both rewrite the card bar.

- The Lairs tab shows one card per family that has an unlocked kind. That is 39 at most, so two rows of keys cover it at most sizes.
- A family card places the tier last used, starting at the highest one owned. The mouse wheel, right click or a repeat key press steps through tiers in a small pop-up above the card.
- Families are grouped by the dungeon they come from, with a thin divider, so the order means something.
- Keys: 1 to 0 for the first ten, and the tab key steps group by group.

### Step 3: Keeper screen
Independent of steps 1 and 2. It only touches `Source/keeper/keeper_hall.cpp`.

- Families fill the width, two or three family blocks across, so about 15 families show at once instead of five.
- A filter row: all, owned, can afford now, locked.
- Jump buttons by dungeon group down the side instead of a long scroll.
- The empty details panel shows the whole family ladder with costs.

**Done 11 November 2026** (`ui/hall`): families flow across the list (two across at 1280, three from about 1500 UI pixels wide), grouped by dungeon with jump buttons, a filter row (All, Owned, Can afford, Locked), and the family ladder in the details panel. About 14 families show at 1280x720, 19 at 3440x1440 and 30 at 1920x1080.

### Step 4: Scale and menus
Small and independent.

- A UI scale setting in the options, defaulting to today's rule.
- Front-end list screens drawn at the same scale.

Done: the `UI Scale` option (Graphics settings: Auto, 1x to 4x) feeds `KeeperUiScale()`. All front-end screens draw on a canvas of the screen divided by that scale, blown up by a whole factor in `UiFadeIn`, with mouse events mapped back in `UiPollAndRender` (`Source/DiabloUI/diabloui.cpp`). The Keeper screen steps out of it with `UiScaleSuspend`.

### Step 5: Orbs and a status line
Follows step 2, on the same branch.

**Done** on branch `ui/families`. The orbs stand 16 UI pixels above a 72 pixel strip (12 above a 54 pixel one), and the status line is 18 pixels at 720 (12 to 13 on lower screens).
The view is now the whole screen on a 640 pixel wide window too, where the engine used to stop it at the hero panel.

Polish (branch `ui/polish-bar`): the keep's view is centred above the strip (to the nearest two tiles; the status line doesn't count), a tier pop-up row is 20 pixels high,
a dungeon with fewer than three families shares the page before it ("Caves & Hell"), and news sits at the very top unless the status line reaches it.

- The status box leaves the strip, which frees about 250 UI pixels for cards.
- A red orb at the left end of the strip shows the Keep Lord's health. A blue orb at the right end, past the spell slots, shows mana. They use Diablo's own orb art and may poke above the strip.
- One line at the top right holds phase and clock, raid, pop, gold and loot, the speed toggle and help. The raid roster sits under it.
- Gold stays a number: it has no maximum to fill against.
- Open the gates becomes a small button by the tabs.
- On small screens, where the strip is 48 pixels high, the orbs are drawn smaller.

### Who does what

| Step | Size | Suggested model | Can run alongside |
| --- | --- | --- | --- |
| 1. Slim strip | Large, many hit tests | Fable or Opus | 3 and 4 |
| 2. Families on the strip | Medium, new interaction | Fable or Opus | After 1 |
| 3. Keeper screen | Medium, one file | Sonnet | 1, 2 and 4 |
| 4. Scale and menus | Small | Sonnet | 1 and 3 |
| 5. Orbs and a status line | Medium | Fable or Opus | After 2 |

Each agent works in its own hand-made worktree under `../dark-keep-worktrees`, tests invisibly with `scripts/dev/testrun.ps1` and `ctl.ps1`, and reports before and after captures.

## Decisions made

Agreed with Ron on 11 November 2026.

1. **Budget.** 10% of screen height is the ceiling.
2. **Monster picking.** Family cards with a tier pop-up. No loadout.
3. **Names on cards.** Names come off the cards and go in the hover text.
4. **Step 4.** Menu scaling and the UI scale setting are in this round.
5. **Status.** Orbs for the Lord's health and mana at the ends of the strip, poking above it. The rest of the status goes in one line at the top right.
