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
- [x] Rebindable keys
- [x] Raid summary (slain / escaped / fled, gold stolen / recovered, bounties, tribute, monsters lost)
- [x] First-map tips (levels 1 and 2), remembered in the profile; text and input names live in keeper_tutorial.cpp
- [x] Help overlay (H / F1 / the ? button), and hover hints for the status column, tabs and scout line
- [x] Raid alerts: banners, what is attacking the Lord, heroes near the vault, thieves, the portal, off-screen arrows, sounds
- [x] End-of-map screen: totals, XP, level-up, newly affordable unlocks, next step; a defeat says why and what to try
- [x] Co-op: joiners' raid summaries use the host's numbers
- [x] The game's own name instead of the Hellfire logo; no other game's movies; hero options hidden from Settings

## 2. Unit placement
- [x] Single monsters instead of groups, priced per monster; up to 96, can stand side by side
- [x] Population cap (Pop): monsters take 1-4 room, the keep holds 16 + 4 per raid
- [x] Selling: click your monster / trap / bait / lock, Del to sell (full refund outside raids, half in one)
- [x] Rally points: right-click with a monster selected; it paths there and guards it
- [x] Patrol routes for keep monsters
- [x] Drag-select several monsters / rally a group at once

## 3. Interaction with the players (heroes)
- [x] Masters' spells hit bot heroes too (Guardian shoots them, Stone Curse petrifies them and spares your monsters)
- [x] Heroes step out of fire walls and wait instead of walking into them
- [x] Better hero AI reactions to traps, bait and locked doors
- [x] Bots react to bait
- [ ] Bot gear that scales with the raid
- [ ] Co-op: host-authoritative bot combat (bots are simulated per client today)

## 4. New levels
- [x] Level editor and `.dkmap` maps (docs/MAP-FORMAT.md), all tilesets; Cathedral is the fullest
- [x] 20-level campaign with saved rank, random layouts for now
- [x] Menus without class, difficulty or the Hellfire prompt
- [x] Monsters from the whole game, unlocked with Renown
- [x] Replace the campaign's random layouts with editor maps (assets/keeper/campaign/level_NN.dkmap): all 20 levels are baked

## 5. Upgrade paths
- [x] Temporary monster upgrades with gold; spells and trap types level up permanently with Renown (1-15)
- [x] Keeper screen: XP, keeper level, Renown, stats, Bestiary, Spellbook, Traps, Spell Bar, free respec
- [x] 11 spells unlocked with Renown, six equipped at a time
- [x] Trap types: Fire, Arrow, Shock, Snare
- [ ] Summon Golem spell (its AI attacks keep monsters and ignores bot heroes)
- [x] Keep Lord upgrades
- [x] Persistent progression: XP and Renown (gold upgrades last one map)
- [ ] Co-op: joiners pick their spell bar in a lobby step (for now: their own choice, filtered by the host's unlocks)
- [ ] Tune XP, Renown prices and difficulty

## Known issues
- [ ] Balance: scaled on paper across the campaign, never playtested
- [x] Mid-game joiners see structures built before they joined
- [ ] Joining mid-raid: hero loot and morale, bot state and the portal aren't sent, so clients can drift
- [ ] Everything in docs/NEXT-PHASE.md needs a first playtest
