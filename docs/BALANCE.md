# The Dark Keep: balance by calculation

Nothing here has been played. Every number comes from a paper model of the campaign, and it can only say
how levels compare with each other and where the tables contradict themselves. It can't say whether a
fight feels fair. The list of what only play can settle is at the end.

Run the model with `python scripts/dev/balance_model.py`. It reads the tables out of the C++ and
`monstdat.tsv`, so it always describes the tree it runs in. `--root PATH` points it at another checkout,
`--coop` gives monsters full life, and `--solve-strength FIRST LAST` proposes a strength table.

## The model

A typical player clears each level first time and kills every hero, with no theft. After each level
they spend Renown 55% on kinds, 35% on spells and 10% on traps, climbing a few monster families.

For each raid, gold goes 25% to upgrades and 25% to traps (up to the 16 slots). The rest buys the best
one- or two-kind army that gold and Pop allow. Selling refunds in full between raids, so an army is
effectively rebought each raid, and all gold earned so far counts as the budget.

**R** is how long the army lasts divided by how long the party lasts. Above 1 the army should win.
**R+** adds spells and traps: it is R / (1 - f), where f is the share of the party's life they remove,
capped at 0.8.

What goes into R:

- **Heroes:** the first 4 in a party are real Player bots, the rest are monster heroes. A bot's life
  includes its potions, each worth half its life.
- **Hit chances, from the engine:**
  - Monster melee against monster heroes rolls the monster's `toHit`, and armour plays no part.
  - Monster missiles and traps against monster heroes hit on `90 - armour - distance`, floor 5%.
  - Against bots, monsters use the player formula, including level difference and block.
  - Master spells hit monster heroes about 48% (the master is a Warrior, magic 10) and bots a flat 40%.
- **Monsters:** life is halved in single player, as the engine does. Each monster takes at least one
  blow, so swarms get credit for absorbing hits.
- **The Keep Lord:** left out of R and reported on his own, because he stands at the end of the keep.

Assumptions that are not in the source are named at the top of the script:

- A raid lasts 90 seconds.
- A typical player lands 50% of what mana and cooldowns allow.
- Half the traps get walked past, and a party takes 12 seconds to pass a trap, in and out.
- There is a 0.3 second pause between swings.

R ignores corridors, pathing, hero AI, fear, stun and possession. Treat a difference of 0.3 as noise: the
army chosen changes the result from level to level.

## What was wrong

Numbers are from the revision before this pass (6025bb987), run through the same model.

1. **Late levels were impossible.** R+ on each level's last raid fell from 0.28 at level 1 to 0.03 at
   level 20. Strength multiplies hero life and damage together, so 400% is worth 16 times the power, and
   the higher raid numbers of long levels multiply on top. Monsters don't scale at all. Only Pop and
   upgrades lift an army, and those allow about 8 to 50 times its level 1 power, against a party that
   grew far more.
2. **Bots grew on the square of strength.** Vitality was scaled and then life was scaled again. A level
   20 Warrior bot had 1932 life and 7 potions, against a monster Warrior's 780.
3. **Bot level inflation.** The +2 levels per campaign level meant to help accuracy also raised damage
   and the monsters' miss chance, and took a Warrior bot's melee block to 100% by level 10.
4. **Armour shut off ranged monsters and traps.** Scaled armour put monster Warriors at the 5% hit floor
   by level 15, and Rogues by level 20. Ranged kinds cost 25% extra for this.
5. **Double counting on spells.** Master level and spell level both feed damage. A level 15 Fireball at
   campaign level 20 did 770 per victim for 10 mana, while Wall did 260 and Wave 89. Apocalypse at 15 did
   667 to every hero in sight for 35 mana.
6. **Guardian did almost nothing.** Its shots take their damage from the Firebolt formula, using the
   master's magic and a Guardian spell level of 0: about 6.5 per shot at every level.
7. **Shock Trap hits repeatedly.** Its lightning rolls to hit on most ticks it lives, so the listed
   damage applied about 8 times.
8. **Progress ran ahead of the campaign.** Keeper level 20, the top unlock rank, arrived by the end of level 15.
   All 111 kinds were eligible with 5 levels to go, so rank stopped mattering.
9. **Pop made higher tiers pointless.** At 1 Pop per 12 power, DPS times life per Pop squared was flat
   across tiers, and 1-Pop units such as Bone Gasher (66) beat Reaper (38).
10. **Starter prices.** Bones at 65 gold and 2 Pop, and Archer at 100 and 2, have 1 life each in single
    player. Their whole family was priced off that.

## What changed

Every constant stays where it lived. New constants sit beside the code that uses them.

| Where | Constant | Before | After | Why |
|---|---|---|---|---|
| keeper_campaign.cpp | `CampaignLevels` strength | 100 to 400 | 100 to 215 | Solved for R+ near 0.9 to 1.4 on last raids, then smoothed |
| keeper_campaign.cpp | `CampaignLevels` start gold, levels 1 to 5 | 1200 to 1400 | 1500 | Levels 1 to 5 came out harder than the ones after |
| keeper_campaign.cpp | `HeroArmorStrengthPercent` (new) | 100 in effect | 50 | Keeps ranged monsters and traps useful (`ScaleArmorByHeroStrength`, also the Lord) |
| keeper_heroes.cpp | `HeroStats`, `HeroMin/MaxDamagePerTenRaids` (new) | Warrior 30+10w life, 2+w to 6+2w damage, 10+3w armour | 33+7w, 3+0.6w to 7+1.2w, 11+2w | Raid 1 is unchanged; growth per raid is about 2/3 |
| keeper_heroes.cpp | same, Rogue | 20+7w, 1+w to 4+2w, 5+2w | 22+5w, 2+0.6w to 5+1.2w, 5+2w | |
| keeper_heroes.cpp | same, Sorcerer | 15+6w, 2+w to 5+2w, 3+w | 17+4w, 3+0.6w to 6+1.2w, 3+w | |
| keeper_bots.cpp | `LevelsPerCampaignLevel` | 2 | 1 | See finding 3; accuracy table below |
| keeper_bots.cpp | `BotStatsPerRaid` (new) | 3, vitality scaled | 2, vitality not scaled | See finding 2 |
| keeper_bots.cpp | `BotPotionsBase`, `BotRaidsPerPotion` (new) | 2 + w/2 | 1 + w/4 | Potions counted for up to 4.5 times a bot's life |
| keeper_game.cpp | `SupplyPerLevel` | 2 | 3 | Level 20 reaches 109 Pop, not 90 |
| keeper_game.cpp | `RaisesPerEngineSpellLevel` (new) | 1 | 2 | The double-counting fix: see below |
| keeper_game.cpp | `ApocalypseDamagePerLevel` | 20 | 8 | Apocalypse at 15 does 372 per victim, not 667 |
| keeper_game.cpp | `SpellKinds` Apocalypse | 70 mana, 20s | 80 mana, 25s | Cost floor 40 mana |
| keeper_game.cpp | `SpellKinds` Wave | 35 mana, 6s | 25 mana, 4s | Weakest damage per mana of any spell |
| keeper_game.cpp | `GuardianDamageBase`, `GuardianDamagePerMasterLevel`, `GuardianDamagePerRaise` (new) | about 6.5 a shot, always | 6 + 0.7 per master level, +3% per raise | See finding 6 |
| keeper_game.cpp | `TrapDamagePerLevel` | 25 | 15 | A maxed trap was worth 11 times a level 1 trap; now about 6 times |
| keeper_game.cpp | `TrapRearmPerLevel`, `TrapRearmFloorPercent` | 6, 40 | 5, 50 | Same |
| keeper_game.hpp | `TrapKinds` gold | Fire 80, Arrow 50, Shock 120, Snare 100 | 100, 75, 140, 150 | Traps need no Pop, and they beat early monsters by far |
| keeper_game.hpp | `TrapKinds` damage | Arrow 8+3w, Shock 15+6w | Arrow 6+2w, Shock 4+2w | Shock now accounts for its repeat hits; both are still rough |
| keeper_roster.cpp | `KindPopPowerDivisor` | 12 | 20 | Higher tiers now pay per Pop |
| keeper_roster.cpp | Bones, Archer, Brute overrides | 65/2, 100/2, 260/4 | 40/1, 60/1, 220/3 | See finding 10 |
| keeper_profile.cpp | `XpPerLevelStep` | 100 | 140 | Keeper level is now about campaign level + 2 or 3 |
| keeper_profile.cpp | `RenownPerLevel` | 3 | 4 | 88 Renown by the end, not 78, even with fewer keeper levels |
| keeper_upgrades.cpp | `UpgradeNextCost` | fixed prices | price scaled by hero strength | Tiers add percentages, so they are worth more as armies grow |

**Logic changes**, each as small as the balance needed:

- `ApplyHeroStats` reads a table instead of a switch.
- `ApplyCast` sets a Guardian's shot damage, as it already did for Apocalypse.
- `SpellEngineLevel()` is new. The placement preview in keeper.cpp uses it, so the Chain Lightning radius
  it shows stays correct.

**Unchanged on purpose:**

- `MasterLevelsPerCampaignLevel` stays 2. Master level is the only damage growth for Wall, Inferno, Chain,
  Shock Wall, Wave and Ring, and it roughly keeps pace with hero life.
- Mana regeneration stays at 1.2 per second, with +25% per tier.
- Party sizes, raid counts, XP awards, Renown prices, bounty and tribute formulas are unchanged. Gold
  income falls because those formulas use the lower strength.

## The campaign, level by level

| Lvl | Name | Raids | Strength % | Start gold | Pop | Gold earned + start | KL after | Renown after | Kinds owned | Heroes last raid | Party life | Party DPS |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | Old Cellars | 5 | 100 -> 100 | 1200 -> 1500 | 16-32 | 3155 | 3 | 8 | 5 | 5 | 1026 | 29 |
| 2 | Rat Warrens | 5 | 105 -> 105 | 1250 -> 1500 | 19-35 | 3233 | 4 | 12 | 9 | 5 | 1073 | 29 |
| 3 | Sunken Chapel | 5 | 110 -> 110 | 1300 -> 1500 | 22-38 | 3319 | 5 | 16 | 11 | 5 | 1125 | 31 |
| 4 | Bone Pits | 6 | 115 -> 115 | 1350 -> 1500 | 25-45 | 4182 | 6 | 20 | 11 | 6 | 1343 | 41 |
| 5 | Black Vaults | 6 | 125 -> 120 | 1400 -> 1500 | 28-48 | 4308 | 7 | 24 | 13 | 6 | 1406 | 44 |
| 6 | Weeping Halls | 6 | 135 -> 125 | 1500 -> 1500 | 31-51 | 4772 | 8 | 28 | 14 | 7 | 1603 | 51 |
| 7 | Ashen Crypts | 7 | 145 -> 130 | 1600 -> 1600 | 34-58 | 6068 | 9 | 32 | 14 | 7 | 1800 | 60 |
| 8 | Iron Gaol | 7 | 155 -> 135 | 1700 -> 1700 | 37-61 | 6406 | 10 | 36 | 15 | 8 | 1980 | 71 |
| 9 | Maggot Deeps | 7 | 165 -> 140 | 1800 -> 1800 | 40-64 | 6693 | 11 | 40 | 16 | 8 | 2055 | 72 |
| 10 | Hollow Court | 8 | 180 -> 140 | 1950 -> 1950 | 43-71 | 8264 | 12 | 44 | 17 | 9 | 2728 | 88 |
| 11 | Blood Galleries | 8 | 195 -> 140 | 2100 -> 2100 | 46-74 | 8414 | 13 | 48 | 18 | 9 | 2728 | 90 |
| 12 | Shrouded Nave | 8 | 210 -> 145 | 2250 -> 2250 | 49-77 | 9228 | 14 | 52 | 18 | 9 | 2825 | 94 |
| 13 | Gallows Reach | 8 | 225 -> 150 | 2400 -> 2400 | 52-80 | 9721 | 15 | 56 | 19 | 10 | 3122 | 107 |
| 14 | Cinder Keep | 9 | 240 -> 155 | 2600 -> 2600 | 55-87 | 12064 | 16 | 60 | 19 | 11 | 3608 | 130 |
| 15 | Howling Stair | 9 | 260 -> 160 | 2800 -> 2800 | 58-90 | 12592 | 17 | 64 | 20 | 11 | 3722 | 136 |
| 16 | Tomb of Kings | 9 | 280 -> 165 | 3000 -> 3000 | 61-93 | 13702 | 18 | 68 | 20 | 12 | 3969 | 149 |
| 17 | Silent Abbey | 9 | 300 -> 170 | 3200 -> 3200 | 64-96 | 14280 | 19 | 72 | 21 | 12 | 4090 | 154 |
| 18 | Chains of Night | 10 | 325 -> 180 | 3500 -> 3500 | 67-103 | 17900 | 20 | 76 | 21 | 13 | 4897 | 198 |
| 19 | Burning Sepulcre | 10 | 350 -> 190 | 3800 -> 3800 | 70-106 | 18987 | 21 | 80 | 21 | 13 | 5166 | 209 |
| 20 | Throne of Ash | 10 | 400 -> 215 | 4200 -> 4200 | 73-109 | 23198 | 23 | 88 | 22 | 14 | 6077 | 244 |

"Gold earned + start" assumes every hero dies and none of it is spent, so it is a ceiling.

## Combat: last raid of each level

Army names show what the model's player would field, not a recommendation.

| Lvl/raid | Life before | Life after | Best army before | Best army after | R before | R after | Swarm before | R+ before | Swarm after | R+ after | R+ co-op | Lord lasts / would kill in |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1/5 | 1624 | 1026 | 5 Brute + 6 Fallen | 7 Brute + 5 Bones | 0.18 | 0.70 | 16 Bones: 0.02 | 0.28 | 32 Fallen: 0.07 | 1.22 | 2.13 | 28s / 73s |
| 2/5 | 1700 | 1073 | 1 Brute + 29 Fiend | 8 Brute | 0.19 | 0.73 | 17 Bones: 0.01 | 0.37 | 35 Fallen: 0.07 | 1.37 | 2.47 | 29s / 76s |
| 3/5 | 1809 | 1125 | 28 Blink | 22 Carcass + 1 Fiend | 0.23 | 0.62 | 18 Bones: 0.01 | 0.45 | 38 Fallen: 0.07 | 1.22 | 2.17 | 29s / 77s |
| 4/6 | 2517 | 1343 | 38 Blink | 10 Brute + 2 Fiend | 0.17 | 0.51 | 21 Bones: 0.01 | 0.35 | 45 Fallen: 0.05 | 1.19 | 2.09 | 22s / 94s |
| 5/6 | 2784 | 1406 | 39 Blink + 2 Zombie | 1 Flesh Clan + 10 Brute | 0.14 | 0.46 | 22 Bones: 0.01 | 0.32 | 48 Fallen: 0.05 | 1.12 | 1.97 | 21s / 96s |
| 6/6 | 3272 | 1603 | 5 Gargoyle + 19 Blink | 14 Stone Clan + 1 Burning Axe | 0.10 | 0.47 | 23 Bones: 0.01 | 0.21 | 51 Fallen: 0.05 | 1.24 | 2.10 | 18s / 102s |
| 7/7 | 3940 | 1800 | 9 Winged + 34 Blink | 5 Stone Clan + 16 Flesh Clan | 0.11 | 0.35 | 26 Bones: 0.01 | 0.22 | 58 Fallen: 0.05 | 1.12 | 2.20 | 16s / 121s |
| 8/7 | 4426 | 1980 | 13 Unseen + 28 Blink | 12 Fire Clan + 4 Ghoul | 0.11 | 0.41 | 27 Bones: 0.00 | 0.23 | 61 Fallen: 0.05 | 1.33 | 2.27 | 14s / 129s |
| 9/7 | 4813 | 2055 | 12 Weaver + 19 Zombie | 10 Night Clan + 4 Fiend | 0.11 | 0.44 | 28 Bones: 0.01 | 0.21 | 64 Fallen: 0.05 | 1.44 | 2.41 | 15s / 133s |
| 10/8 | 6813 | 2728 | 20 Gargoyle + 2 Hidden | 14 Night Clan + 3 Burning Axe | 0.06 | 0.44 | 31 Bones: 0.00 | 0.11 | 71 Fallen: 0.04 | 1.12 | 1.87 | 12s / 195s |
| 11/8 | 7587 | 2728 | 13 Blood Claw + 12 Hidden | 8 Night Clan + 10 Brute | 0.08 | 0.33 | 32 Bones: 0.00 | 0.14 | 74 Fallen: 0.04 | 0.87 | 1.58 | 12s / 200s |
| 12/8 | 8474 | 2825 | 16 Blood Claw + 1 Winged | 9 Night Clan + 12 Brute | 0.11 | 0.38 | 33 Bones: 0.00 | 0.20 | 77 Fallen: 0.05 | 1.35 | 2.42 | 11s / 208s |
| 13/8 | 9704 | 3122 | 16 Blood Claw + 4 Hidden | 10 Night Clan + 12 Brute | 0.06 | 0.32 | 34 Bones: 0.00 | 0.09 | 80 Fallen: 0.04 | 1.01 | 1.82 | 10s / 220s |
| 14/9 | 11869 | 3608 | 10 Death Wing + 7 Winged | 12 Night Clan + 17 Stone Clan | 0.06 | 0.32 | 37 Bones: 0.00 | 0.09 | 87 Fallen: 0.05 | 0.91 | 1.76 | 9s / 281s |
| 15/9 | 13328 | 3722 | 9 Death Wing + 11 Winged | 15 Night Clan + 14 Stone Clan | 0.05 | 0.33 | 38 Bones: 0.00 | 0.07 | 90 Fallen: 0.05 | 0.95 | 1.84 | 9s / 297s |
| 16/9 | 15149 | 3969 | 13 Death Wing | 16 Night Clan + 17 Stone Clan | 0.04 | 0.36 | 39 Bones: 0.00 | 0.06 | 93 Fallen: 0.04 | 1.13 | 2.17 | 8s / 326s |
| 17/9 | 16779 | 4090 | 7 Reaper + 3 Hidden | 18 Night Clan + 16 Stone Clan | 0.06 | 0.37 | 40 Bones: 0.00 | 0.08 | 96 Fallen: 0.05 | 1.19 | 1.86 | 9s / 348s |
| 18/10 | 23228 | 4897 | 7 Reaper + 9 Hidden | 37 Night Clan | 0.04 | 0.48 | 43 Bones: 0.00 | 0.05 | 103 Fallen: 0.04 | 1.22 | 1.84 | 7s / 432s |
| 19/10 | 26004 | 5166 | 7 Reaper + 11 Hidden | 27 Night Clan + 50 Carcass | 0.03 | 0.54 | 44 Bones: 0.00 | 0.04 | 106 Fallen: 0.04 | 1.39 | 1.91 | 7s / 435s |
| 20/10 | 32538 | 6077 | 8 Reaper + 2 Blink | 12 Reaper + 1 Stone Clan | 0.03 | 0.65 | 45 Bones: 0.00 | 0.03 | 109 Fallen: 0.04 | 1.42 | 2.85 | 7s / 447s |

Against the same model, R+ went from 0.28, 0.32, 0.11, 0.07, 0.03 at levels 1, 5, 10, 15, 20 to about
0.9 to 1.4 at every level. The Lord's columns read "lasts against the whole party / would kill the whole
party alone".

A level's first raid is far easier than its last:

| Lvl/raid | Heroes | Party life | Army | R | R+ |
|---|---|---|---|---|---|
| 1/1 | 2 | 172 | 4 Brute + 3 Zombie | 15.58 | 77.89 |
| 5/1 | 2 | 207 | 1 Carcass + 4 Brute | 7.70 | 38.49 |
| 10/1 | 3 | 304 | 4 Night Clan | 2.70 | 13.49 |
| 15/1 | 4 | 516 | 4 Fire Clan + 4 Brute | 1.78 | 8.91 |
| 20/1 | 6 | 793 | 2 Reaper + 1 Night Clan | 3.26 | 16.31 |

So most of each level is a build-up, and the difficulty is in its last two or three raids. That follows
from heroes growing with the raid number and is not new, though the gap is smaller than it was.

## XP, Renown and unlocks

| Lvl | KL after: before | KL after: now | Renown: before | Renown: now | Kinds eligible: before | Kinds eligible: now | Renown for all eligible (now) | Kinds owned (now) | Key spells (now) |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 4 | 3 | 9 | 8 | 25 | 17 | 12 | 5 |  |
| 5 | 9 | 7 | 24 | 24 | 48 | 39 | 56 | 13 | Nova 4, Fire 1 |
| 10 | 15 | 12 | 42 | 44 | 75 | 59 | 119 | 17 | Nova 4, Fire 3, Chai 1 |
| 15 | 20 | 17 | 57 | 64 | 111 | 91 | 263 | 20 | Nova 4, Fire 6, Chai 1 |
| 20 | 27 | 23 | 78 | 88 | 111 | 111 | 370 | 22 | Nova 4, Fire 7, Chai 1, Apoc 1 |

Catalogue: all kinds 370 Renown, spell unlocks 29, one item raised 1 to 15 costs 44.

- **Warband by level 5, 10, 15, 20:** 13, 17, 20 and 22 kinds owned, with spells to match. Nobody can
  own everything, and that seems right: respec is free, so a player can change direction.
- **Unlock gating:** keeper level 18 (Apocalypse) now arrives about level 17, and rank 20 about level 19.
  Before, those came at levels 14 and 16. Chain arrives about level 7, Fireball about level 4.
- **Dead zones:** none. Every level adds keeper levels, so Renown arrives every level and there is always
  something to buy. Rank stays a limit on what is available until late in the campaign.
- **Existing profiles:** a profile keeps its XP but drops keeper levels under the steeper curve. Purchases
  stay, and Renown can show 0 for a while. Nobody has a profile that matters yet.

## Gold and Pop

Now:

| Lvl | Start | Tribute | Bounties | Total | Pop first | Pop last | Army gold first | Army gold last | Upgrade tiers |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1500 | 650 | 1005 | 3155 | 16 | 32 | 1000 | 1740 | 2 |
| 5 | 1500 | 1050 | 1758 | 4308 | 28 | 48 | 960 | 2335 | 2 |
| 10 | 1950 | 1960 | 4354 | 8264 | 43 | 71 | 1280 | 4660 | 3 |
| 15 | 2800 | 2720 | 7072 | 12592 | 58 | 90 | 1880 | 7390 | 4 |
| 20 | 4200 | 4350 | 14648 | 23198 | 73 | 109 | 2710 | 14525 | 5 |

Before:

| Lvl | Start | Tribute | Bounties | Total | Pop first | Pop last | Army gold first | Army gold last | Upgrade tiers |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1200 | 650 | 1005 | 2855 | 16 | 32 | 720 | 1510 | 2 |
| 5 | 1400 | 1092 | 1825 | 4317 | 24 | 44 | 900 | 2420 | 2 |
| 10 | 1950 | 2520 | 5598 | 10068 | 34 | 62 | 1320 | 5820 | 4 |
| 15 | 2800 | 4420 | 11492 | 18712 | 44 | 76 | 1725 | 7540 | 10 |
| 20 | 4200 | 8100 | 27320 | 39620 | 54 | 90 | 2390 | 9680 | 26 |

- **Binding limit:** gold, on every raid of every level in the model. Before, Pop bound last raids from about
level 12, and gold piled up unspent. Pop is now close enough to matter when a player saves well.
- **Cheap swarm:** not dominant. A full keep of the cheapest kind scores R 0.04 to 0.07 on last raids and
  under 1 on most first raids. Pop-1 mid-tier kinds such as Carcass are good until about level 5, which
  seems fine.

## Heroes: bots against monster heroes

Now:

| Lvl/raid | Class | Bot life | Potions | Bot clvl | Bot dmg | Bot AC | Monster life | Monster dmg | Monster AC |
|---|---|---|---|---|---|---|---|---|---|
| 1/1 | W | 70 | 1 | 1 | 4 | 7 | 40 | 6 | 13 |
| 1/1 | R | 45 | 1 | 1 | 2 | 6 | 27 | 4 | 7 |
| 1/1 | S | 30 | 1 | 1 | 10 | 3 | 21 | 5 | 4 |
| 1/5 | W | 153 | 2 | 9 | 8 | 8 | 102 | 10 | 21 |
| 1/5 | R | 103 | 2 | 9 | 4 | 7 | 70 | 8 | 15 |
| 1/5 | S | 69 | 2 | 9 | 14 | 4 | 55 | 9 | 8 |
| 5/6 | W | 198 | 2 | 15 | 12 | 9 | 134 | 12 | 25 |
| 5/6 | R | 134 | 2 | 15 | 8 | 8 | 93 | 10 | 18 |
| 5/6 | S | 90 | 2 | 15 | 14 | 5 | 73 | 11 | 9 |
| 10/8 | W | 264 | 3 | 24 | 19 | 10 | 186 | 16 | 32 |
| 10/8 | R | 182 | 3 | 24 | 12 | 9 | 130 | 14 | 25 |
| 10/8 | S | 121 | 3 | 24 | 16 | 6 | 102 | 15 | 13 |
| 15/9 | W | 321 | 3 | 31 | 28 | 12 | 230 | 20 | 37 |
| 15/9 | R | 222 | 3 | 31 | 18 | 11 | 160 | 18 | 29 |
| 15/9 | S | 148 | 3 | 31 | 18 | 8 | 126 | 18 | 15 |
| 20/1 | W | 150 | 1 | 20 | 12 | 7 | 86 | 12 | 20 |
| 20/1 | R | 96 | 1 | 20 | 8 | 6 | 58 | 8 | 10 |
| 20/1 | S | 64 | 1 | 20 | 10 | 3 | 45 | 10 | 6 |
| 20/10 | W | 457 | 3 | 38 | 40 | 14 | 331 | 30 | 48 |
| 20/10 | R | 318 | 3 | 38 | 26 | 13 | 232 | 26 | 39 |
| 20/10 | S | 212 | 3 | 38 | 20 | 10 | 182 | 28 | 20 |

Before:

| Lvl/raid | Class | Bot life | Potions | Bot clvl | Bot dmg | Bot AC | Monster life | Monster dmg | Monster AC |
|---|---|---|---|---|---|---|---|---|---|
| 1/1 | W | 70 | 2 | 1 | 4 | 7 | 40 | 6 | 13 |
| 1/1 | R | 45 | 2 | 1 | 2 | 6 | 27 | 4 | 7 |
| 1/1 | S | 30 | 2 | 1 | 10 | 3 | 21 | 5 | 4 |
| 1/5 | W | 165 | 4 | 9 | 8 | 9 | 120 | 12 | 25 |
| 1/5 | R | 109 | 4 | 9 | 6 | 8 | 82 | 10 | 15 |
| 1/5 | S | 75 | 4 | 9 | 14 | 5 | 67 | 11 | 8 |
| 5/6 | W | 236 | 5 | 19 | 15 | 10 | 168 | 16 | 35 |
| 5/6 | R | 155 | 5 | 19 | 10 | 9 | 116 | 14 | 21 |
| 5/6 | S | 108 | 5 | 19 | 16 | 6 | 95 | 16 | 11 |
| 10/8 | W | 464 | 6 | 33 | 35 | 14 | 297 | 28 | 61 |
| 10/8 | R | 297 | 6 | 33 | 22 | 13 | 205 | 26 | 37 |
| 10/8 | S | 217 | 6 | 33 | 20 | 10 | 169 | 28 | 19 |
| 15/9 | W | 881 | 6 | 45 | 65 | 19 | 468 | 45 | 96 |
| 15/9 | R | 540 | 6 | 45 | 42 | 18 | 322 | 42 | 59 |
| 15/9 | S | 421 | 6 | 45 | 22 | 15 | 267 | 44 | 31 |
| 20/1 | W | 280 | 2 | 39 | 21 | 7 | 160 | 22 | 52 |
| 20/1 | R | 180 | 2 | 39 | 12 | 6 | 108 | 16 | 28 |
| 20/1 | S | 120 | 2 | 39 | 10 | 3 | 84 | 20 | 16 |
| 20/10 | W | 1932 | 7 | 50 | 110 | 28 | 780 | 76 | 160 |
| 20/10 | R | 1132 | 7 | 50 | 68 | 27 | 540 | 70 | 100 |
| 20/10 | S | 936 | 7 | 50 | 28 | 24 | 448 | 74 | 52 |

Bots are about 1.4 times a monster hero in life and similar in damage, down from up to 2.5 times in
life. Bots also have their potions and real AI.

Chance for ranged monsters and traps to hit monster heroes, and Warrior bots' melee block. Now:

| Lvl/raid | Warrior AC | Rogue AC | Sorcerer AC | Block vs mlvl 5 | Block vs mlvl 20 |
|---|---|---|---|---|---|
| 1/5 | 21 (65%) | 15 (71%) | 8 (78%) | 66% | 36% |
| 5/6 | 25 (61%) | 18 (68%) | 9 (77%) | 82% | 52% |
| 10/8 | 32 (54%) | 25 (61%) | 13 (73%) | 100% | 77% |
| 15/9 | 37 (49%) | 29 (57%) | 15 (71%) | 100% | 97% |
| 20/10 | 48 (38%) | 39 (47%) | 20 (66%) | 100% | 100% |

Before:

| Lvl/raid | Warrior AC | Rogue AC | Sorcerer AC | Block vs mlvl 5 | Block vs mlvl 20 |
|---|---|---|---|---|---|
| 1/5 | 25 (61%) | 15 (71%) | 8 (78%) | 70% | 40% |
| 5/6 | 35 (51%) | 21 (65%) | 11 (75%) | 96% | 66% |
| 10/8 | 61 (25%) | 37 (49%) | 19 (67%) | 100% | 100% |
| 15/9 | 96 (5%) | 59 (27%) | 31 (55%) | 100% | 100% |
| 20/10 | 160 (5%) | 100 (5%) | 52 (34%) | 100% | 100% |

**Warrior bots still reach 100% melee block by about level 10.** Block is dexterity + 30 + 2 x level -
2 x monster level, and it applies while the bot stands or swings. The model counts 80% of attacks as
blockable. I left this alone: no per-player setting exists, and removing the shield would also change
level 1. If Warrior bots feel unkillable in melee, this is why.

Bots' chance to hit monster armour:

| Lvl/raid | AC 10 | AC 30 | AC 50 | AC 80 |
|---|---|---|---|---|
| 1/1 | 71% / 83% | 51% / 63% | 31% / 43% | 5% / 13% |
| 5/1 | 75% / 87% | 55% / 67% | 35% / 47% | 5% / 17% |
| 10/1 | 80% / 92% | 60% / 72% | 40% / 52% | 10% / 22% |
| 15/1 | 85% / 95% | 65% / 77% | 45% / 57% | 15% / 27% |
| 20/1 | 90% / 95% | 70% / 82% | 50% / 62% | 20% / 32% |
| 20/10 | 95% / 95% | 95% / 95% | 87% / 95% | 57% / 88% |

Low at the start of a late level, on purpose: those bots are the weak ones anyway. Monster heroes in melee
always roll 60%, and in ranged attacks `90 - armour - distance`, so high-armour monsters are nearly immune
to monster Rogues and Sorcerers. The Monster Armor upgrade only helps against those and bots.

Monster melee against monster heroes rolls raw `toHit`, without the player formula's +30. A Zombie hits a
monster hero 10% of the time and a level 1 bot about 33%. Early parties are all bots, so this only shows
in late levels with low-tier monsters, and I left it. It may be worth a look if low tiers feel useless.

## Spells

Campaign levels 5, 10 and 20, at the raise the typical player has and at 15. "Expected total" includes hit
chance and the assumed number of victims. Now:

| Lvl | Master clvl | Spell | Raised to | Engine lvl | Mana | Cooldown s | Dmg/victim | Expected total | Per mana | % of party life |
|---|---|---|---|---|---|---|---|---|---|---|
| 5 | 9 | Nova | 4 | 7 | 29 | 6.0 | 68 | 58 | 2.0 | 4% |
| 5 | 9 | Nova | 15 | 13 | 17 | 6.0 | 139 | 118 | 7.0 | 8% |
| 5 | 9 | Wall | 1 | 4 | 25 | 4.0 | 110 | 94 | 3.8 | 7% |
| 5 | 9 | Wall | 15 | 11 | 12 | 4.0 | 110 | 94 | 7.8 | 7% |
| 5 | 9 | Guard | 1 | 4 | 30 | 8.0 | 96 | 41 | 1.4 | 3% |
| 5 | 9 | Guard | 15 | 11 | 15 | 8.0 | 255 | 109 | 7.3 | 8% |
| 5 | 9 | Stone | 1 | 4 | 20 | 5.0 | 0 | 0 | 0.0 | 0% |
| 5 | 9 | Stone | 15 | 11 | 10 | 5.0 | 0 | 0 | 0.0 | 0% |
| 5 | 9 | Inferno | 1 | 2 | 15 | 2.0 | 52 | 45 | 3.0 | 3% |
| 5 | 9 | Inferno | 15 | 9 | 7 | 2.0 | 52 | 45 | 6.4 | 3% |
| 5 | 9 | Fireball | 1 | 3 | 20 | 3.0 | 63 | 53 | 2.7 | 4% |
| 5 | 9 | Fireball | 15 | 10 | 10 | 3.0 | 143 | 122 | 12.2 | 9% |
| 10 | 19 | Nova | 4 | 7 | 29 | 6.0 | 91 | 81 | 2.8 | 3% |
| 10 | 19 | Nova | 15 | 13 | 17 | 6.0 | 185 | 164 | 9.7 | 6% |
| 10 | 19 | Wall | 1 | 4 | 25 | 4.0 | 160 | 142 | 5.7 | 5% |
| 10 | 19 | Wall | 15 | 11 | 12 | 4.0 | 160 | 142 | 11.9 | 5% |
| 10 | 19 | Guard | 1 | 4 | 30 | 8.0 | 247 | 110 | 3.7 | 4% |
| 10 | 19 | Guard | 15 | 11 | 15 | 8.0 | 520 | 231 | 15.4 | 8% |
| 10 | 19 | Stone | 1 | 4 | 20 | 5.0 | 0 | 0 | 0.0 | 0% |
| 10 | 19 | Stone | 15 | 11 | 10 | 5.0 | 0 | 0 | 0.0 | 0% |
| 10 | 19 | Inferno | 1 | 2 | 15 | 2.0 | 90 | 80 | 5.3 | 3% |
| 10 | 19 | Inferno | 15 | 9 | 7 | 2.0 | 90 | 80 | 11.4 | 3% |
| 10 | 19 | Fireball | 3 | 4 | 18 | 3.0 | 103 | 91 | 5.1 | 3% |
| 10 | 19 | Fireball | 15 | 10 | 10 | 3.0 | 208 | 185 | 18.5 | 7% |
| 10 | 19 | Chain | 1 | 3 | 30 | 5.0 | 90 | 160 | 5.3 | 6% |
| 10 | 19 | Chain | 15 | 10 | 15 | 5.0 | 132 | 235 | 15.6 | 9% |
| 10 | 19 | Shock Wall | 1 | 3 | 30 | 5.0 | 192 | 256 | 8.5 | 9% |
| 10 | 19 | Shock Wall | 15 | 10 | 15 | 5.0 | 192 | 256 | 17.1 | 9% |
| 10 | 19 | Wave | 1 | 3 | 25 | 4.0 | 49 | 65 | 2.6 | 2% |
| 10 | 19 | Wave | 15 | 10 | 12 | 4.0 | 49 | 65 | 5.4 | 2% |
| 20 | 39 | Nova | 4 | 7 | 29 | 6.0 | 137 | 125 | 4.3 | 2% |
| 20 | 39 | Nova | 15 | 13 | 17 | 6.0 | 277 | 254 | 14.9 | 4% |
| 20 | 39 | Wall | 1 | 4 | 25 | 4.0 | 260 | 238 | 9.5 | 4% |
| 20 | 39 | Wall | 15 | 11 | 12 | 4.0 | 260 | 238 | 19.8 | 4% |
| 20 | 39 | Guard | 1 | 4 | 30 | 8.0 | 759 | 347 | 11.6 | 6% |
| 20 | 39 | Guard | 15 | 11 | 15 | 8.0 | 1380 | 631 | 42.1 | 10% |
| 20 | 39 | Stone | 1 | 4 | 20 | 5.0 | 0 | 0 | 0.0 | 0% |
| 20 | 39 | Stone | 15 | 11 | 10 | 5.0 | 0 | 0 | 0.0 | 0% |
| 20 | 39 | Inferno | 1 | 2 | 15 | 2.0 | 165 | 151 | 10.1 | 2% |
| 20 | 39 | Inferno | 15 | 9 | 7 | 2.0 | 165 | 151 | 21.6 | 2% |
| 20 | 39 | Fireball | 7 | 6 | 14 | 3.0 | 211 | 193 | 13.8 | 3% |
| 20 | 39 | Fireball | 15 | 10 | 10 | 3.0 | 338 | 309 | 30.9 | 5% |
| 20 | 39 | Chain | 1 | 3 | 30 | 5.0 | 165 | 302 | 10.1 | 5% |
| 20 | 39 | Chain | 15 | 10 | 15 | 5.0 | 242 | 443 | 29.5 | 7% |
| 20 | 39 | Shock Wall | 1 | 3 | 30 | 5.0 | 312 | 428 | 14.3 | 7% |
| 20 | 39 | Shock Wall | 15 | 10 | 15 | 5.0 | 312 | 428 | 28.5 | 7% |
| 20 | 39 | Wave | 1 | 3 | 25 | 4.0 | 89 | 122 | 4.9 | 2% |
| 20 | 39 | Wave | 15 | 10 | 12 | 4.0 | 89 | 122 | 10.2 | 2% |
| 20 | 39 | Ring | 1 | 3 | 40 | 8.0 | 260 | 357 | 8.9 | 6% |
| 20 | 39 | Ring | 15 | 10 | 20 | 8.0 | 260 | 357 | 17.8 | 6% |
| 20 | 39 | Apocalypse | 1 | 1 | 80 | 25.0 | 176 | 642 | 8.0 | 11% |
| 20 | 39 | Apocalypse | 15 | 8 | 40 | 25.0 | 372 | 1361 | 34.0 | 22% |

Before, at level 20:

| Lvl | Master clvl | Spell | Raised to | Engine lvl | Mana | Cooldown s | Dmg/victim | Expected total | Per mana | % of party life |
|---|---|---|---|---|---|---|---|---|---|---|
| 20 | 39 | Nova | 4 | 9 | 29 | 6.0 | 173 | 158 | 5.5 | 0% |
| 20 | 39 | Nova | 15 | 20 | 17 | 6.0 | 633 | 578 | 34.0 | 2% |
| 20 | 39 | Wall | 1 | 4 | 25 | 4.0 | 260 | 238 | 9.5 | 1% |
| 20 | 39 | Wall | 15 | 18 | 12 | 4.0 | 260 | 238 | 19.8 | 1% |
| 20 | 39 | Guard | 1 | 4 | 30 | 8.0 | 150 | 68 | 2.3 | 0% |
| 20 | 39 | Guard | 15 | 18 | 15 | 8.0 | 195 | 89 | 5.9 | 0% |
| 20 | 39 | Stone | 1 | 4 | 20 | 5.0 | 0 | 0 | 0.0 | 0% |
| 20 | 39 | Stone | 15 | 18 | 10 | 5.0 | 0 | 0 | 0.0 | 0% |
| 20 | 39 | Inferno | 1 | 2 | 15 | 2.0 | 165 | 151 | 10.1 | 0% |
| 20 | 39 | Inferno | 15 | 16 | 7 | 2.0 | 165 | 151 | 21.6 | 0% |
| 20 | 39 | Fireball | 4 | 6 | 17 | 3.0 | 211 | 193 | 11.3 | 1% |
| 20 | 39 | Fireball | 15 | 17 | 10 | 3.0 | 770 | 704 | 70.4 | 2% |
| 20 | 39 | Chain | 1 | 3 | 30 | 5.0 | 165 | 302 | 10.1 | 1% |
| 20 | 39 | Chain | 15 | 17 | 15 | 5.0 | 319 | 583 | 38.9 | 2% |
| 20 | 39 | Shock Wall | 1 | 3 | 30 | 5.0 | 312 | 428 | 14.3 | 1% |
| 20 | 39 | Shock Wall | 15 | 17 | 15 | 5.0 | 312 | 428 | 28.5 | 1% |
| 20 | 39 | Wave | 1 | 3 | 35 | 6.0 | 89 | 122 | 3.5 | 0% |
| 20 | 39 | Wave | 15 | 17 | 17 | 6.0 | 89 | 122 | 7.2 | 0% |
| 20 | 39 | Ring | 1 | 3 | 40 | 8.0 | 260 | 357 | 8.9 | 1% |
| 20 | 39 | Ring | 15 | 17 | 20 | 8.0 | 260 | 357 | 17.8 | 1% |
| 20 | 39 | Apocalypse | 3 | 3 | 62 | 20.0 | 246 | 899 | 14.5 | 3% |
| 20 | 39 | Apocalypse | 15 | 15 | 35 | 20.0 | 667 | 2439 | 69.7 | 7% |

**Double counting.** Nova, Fireball and Apocalypse multiply a master-level term by a spell-level term.
Nova and Fireball compound 12.5% per engine level, so raising 14 times multiplied them by 5.2 on top of
master level's 3.7. Raises now give half an engine level, which makes that 2.3, and Apocalypse's 8% per
raise gives at most 2.1 where it was 3.8. A raise still also lowers mana cost, down to half, and lengthens
walls and Guardian.

**Dominant or useless.**

- Spell damage per mana at 15 raises now spans about 10 to 42. Before it spanned 6 to 70.
- A level-1 Guardian at level 1 is still weak (0.3 per mana) and only comes good with master level.
- Wave remains the weakest damage spell, though the "hits per victim" figure for it is the roughest guess
  in the model.
- Wall and Ring don't scale with raises, except that they last longer and cost less.

**Apocalypse.** It unlocks about level 17. At raise 1 it costs 80 mana, 11% of a last raid per cast. At 15
it costs 40 mana and 25 seconds, 22% per cast, 3 or 4 casts a raid if the master does nothing else. That
is 50 Renown of the 88 on offer: a real ultimate for someone who commits to it, and short of solving a raid
alone.

**Stone** has no damage to measure. It lasts 8 seconds at first and tops out at 12 (engine level 9), so
raises past about 11 only cut its cost. Petrified heroes are hit every time, which makes it strong support.

## Traps

| Lvl | Trap | Level | Gold | Damage | Rearm s | Hit Warrior / Rogue | DPS while a Rogue stands by it |
|---|---|---|---|---|---|---|---|
| 1 | Fire Trap | 1 | 100 | 45 | 8.0 | 68% / 74% | 4 |
| 1 | Arrow Trap | 1 | 75 | 16 | 3.0 | 68% / 74% | 4 |
| 1 | Shock Trap | 1 | 140 | 14 | 6.0 | 68% / 74% | 2 |
| 5 | Fire Trap | 2 | 100 | 69 | 7.6 | 64% / 71% | 6 |
| 5 | Arrow Trap | 2 | 75 | 24 | 2.85 | 64% / 71% | 6 |
| 5 | Shock Trap | 1 | 140 | 19 | 6.0 | 64% / 71% | 2 |
| 10 | Fire Trap | 3 | 100 | 109 | 7.2 | 57% / 64% | 10 |
| 10 | Arrow Trap | 3 | 75 | 39 | 2.7 | 57% / 64% | 9 |
| 10 | Shock Trap | 1 | 140 | 28 | 6.0 | 57% / 64% | 3 |
| 15 | Fire Trap | 3 | 100 | 135 | 7.2 | 52% / 60% | 11 |
| 15 | Arrow Trap | 4 | 75 | 55 | 2.55 | 52% / 60% | 13 |
| 15 | Shock Trap | 1 | 140 | 35 | 6.0 | 52% / 60% | 4 |
| 20 | Fire Trap | 4 | 100 | 217 | 6.8 | 41% / 50% | 16 |
| 20 | Arrow Trap | 4 | 75 | 79 | 2.55 | 41% / 50% | 15 |
| 20 | Shock Trap | 1 | 140 | 51 | 6.0 | 41% / 50% | 4 |

The last column is the damage rate while a Rogue stands beside the trap. The Shock figure leaves out its
repeat hits, about 8 in the model. The Fire Trap's burning wall isn't counted, though heroes avoid it.

## Upgrades

Now:

| Lvl | Gold before last raid | All upgrades cost | % of gold earned in the level | Tiers bought by last raid | Tiers on offer |
|---|---|---|---|---|---|
| 1 | 2790 | 9710 | 308% | 2 | 29 |
| 5 | 3756 | 11652 | 270% | 2 | 29 |
| 10 | 7228 | 13594 | 164% | 3 | 29 |
| 15 | 11096 | 15536 | 123% | 4 | 29 |
| 20 | 20582 | 20866 | 90% | 5 | 29 |

Before:

| Lvl | Gold before last raid | All upgrades cost | % of gold earned in the level | Tiers bought by last raid | Tiers on offer |
|---|---|---|---|---|---|
| 1 | 2490 | 9710 | 340% | 2 | 29 |
| 5 | 3745 | 9710 | 225% | 2 | 29 |
| 10 | 8736 | 9710 | 96% | 4 | 29 |
| 15 | 16281 | 9710 | 52% | 10 | 29 |
| 20 | 34740 | 9710 | 25% | 26 | 29 |

Before, level 20 could buy everything for 25% of its gold. Now the full set costs 90% of a level 20's
gold, and tiers stay good value throughout. The model's player only buys a few because the army pays more
in its square-law terms, and a real player may well buy more.

## What only play can confirm

Most important first.

1. **Whether R near 1 feels right at all.** Level 1's last raid is the best first test. If it is a walkover
   or a wipe, shift the whole `CampaignLevels` strength column rather than single levels.
2. **How long raids last.** Mana, traps and spell shares all assume 90 seconds.
3. **Whether corridors favour big units.** Square-law sums favour many small monsters, while corridors let
   one Reaper hold a line. Pop divisor 20 may then be too generous to high tiers.
4. **Spell hit counts.** How many hits Nova, Chain Lightning, Flame Wave, Shock Wall and Shock Trap really
   land per cast is read from the code and estimated. These are the most likely numbers to be badly off.
5. **Warrior bots' block**, as above. Also bots' potions: how often they really get to drink.
6. **Trap avoidance.** Heroes route around known traps and Rogues disarm them. The model only applies flat
   factors. Trap prices are the least certain gold numbers here.
7. **Late monster heroes' armour.** Check whether ranged monsters feel worthwhile at 40% to 60% to hit.
8. **Co-op.** Monsters have full life with more than one player, so R+ is about 1.8 to 2.8 even before
   counting a second master's spells. `--coop` shows this. Co-op may be too easy, and nothing scales the
   party by player count.
9. **Hero theft.** Gold assumes nothing is stolen. Heavy theft would make gold tighter than shown.
10. **Possession, patrols and bait** aren't modelled at all.

Late levels were judged on models, not on play. If the playtest stream reports that a level is out of
line, adjust that level's strength first: it is the one number meant to carry a level's difficulty.
