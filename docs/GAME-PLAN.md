# The Dark Keep — game plan

Working todo list, roughly in order. Each section gets fleshed out when we start it;
add sections as new ideas come up. Done items move to [MOD-PLAN.md](MOD-PLAN.md).

## 1. UI
- [x] RTS command bar: status column, tabbed scrollable card bar with portraits (Lairs / Traps & Tricks), info panel for the hovered card
- [x] HUD scales up on big screens (2x at 1440p, 3x at 4K); world overlays stay full resolution
- [x] Unaffordable cards greyed with red frame, spell recharge shade, floating feedback line
- [x] Placement previews: ghost of the monster (red where it can't go), trap/bait reach, spell areas, Stone Curse target
- [x] Floating hit / heal markers with icons (ported from The Red Portal)
- [x] Health bars over hurt units, mana bars over hero bots
- [x] Click a hero to inspect stats, gear, loot and state
- [x] Spells in their own bar; hotkeys cast instantly at the cursor
- [x] Raid roster: every hero in the raid at a glance (top right; click one to inspect and jump to it)
- [ ] Rebindable keys
- [x] Raid summary (slain / escaped / fled, gold stolen / recovered, bounties, tribute, monsters lost)

## 2. Unit placement
- [x] Single monsters instead of groups, priced per monster; up to 96, can stand side by side
- [x] Population cap (Pop): monsters take 1-4 room, the keep holds 16 + 4 per raid
- [x] Selling: click your monster / trap / bait / lock, Del to sell (full refund outside raids, half in one)
- [x] Rally points: right-click with a monster selected; it paths there and guards it
- [ ] Patrol routes for keep monsters
- [ ] Drag-select several monsters / rally a group at once

## 3. Interaction with the players (heroes)
- [x] Masters' spells hit bot heroes too (Guardian shoots them, Stone Curse petrifies them and spares your monsters)
- [x] Heroes step out of fire walls and wait instead of walking into them
- [ ] Better hero AI reactions to traps, bait and locked doors
- [ ] Bots react to bait (only monster heroes do now)
- [ ] Bot gear that scales with the raid
- [ ] Co-op: host-authoritative bot combat (bots are simulated per client today)

## 4. New levels
- [ ] More keep layouts (Catacombs, Caves, Hell, Hellfire tilesets)
- [ ] A campaign / level progression between keeps

## 5. Upgrade paths
- [ ] Upgrades for lairs, traps and spells
- [ ] Keep Lord upgrades
- [ ] Persistent progression between games

## Known issues
- [ ] Balance: an undefended raid 6 can kill the Lord; numbers are untuned
- [ ] Mid-game joiners don't see structures built before they joined
