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

1. **Screen budget.** Everything that is always on screen during play takes at most 10% of the screen height, at every supported size. Today's figure is 20% to 29%.
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

### Step 4: Scale and menus
Small and independent.

- A UI scale setting in the options, defaulting to today's rule.
- Front-end list screens drawn at the same scale.

### Who does what

| Step | Size | Suggested model | Can run alongside |
| --- | --- | --- | --- |
| 1. Slim strip | Large, many hit tests | Fable or Opus | 3 and 4 |
| 2. Families on the strip | Medium, new interaction | Fable or Opus | After 1 |
| 3. Keeper screen | Medium, one file | Sonnet | 1, 2 and 4 |
| 4. Scale and menus | Small | Sonnet | 1 and 3 |

Each agent works in its own hand-made worktree under `../dark-keep-worktrees`, tests invisibly with `scripts/dev/testrun.ps1` and `ctl.ps1`, and reports before and after captures.

## Decisions made

Agreed with Ron on 11 November 2026.

1. **Budget.** 10% of screen height is the ceiling.
2. **Monster picking.** Family cards with a tier pop-up. No loadout.
3. **Names on cards.** Names come off the cards and go in the hover text.
4. **Step 4.** Menu scaling and the UI scale setting are in this round.
