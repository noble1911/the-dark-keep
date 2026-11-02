# The Dark Keep: balance by calculation

Nothing here has been played. Every number comes from a paper model of the campaign, and it can only say
how levels compare with each other and where the tables contradict themselves. It can't say whether a
fight feels fair. The list of what only play can settle is at the end.

Run the model with `python scripts/dev/balance_model.py`. It reads the tables out of the C++ and
`monstdat.tsv`, so it always describes the tree it runs in. `--root PATH` points it at another checkout,
`--coop` gives monsters full life, and `--solve-strength FIRST LAST` proposes a strength table.

## The model

A typical player clears each level first time, with no theft. After each level they spend Renown 55% on kinds, 35% on spells and 10% on traps, climbing a few monster families.

For each raid, gold goes 25% to upgrades and 25% to traps (up to the 16 slots). The rest buys the best
one- or two-kind army that gold and Pop allow. Selling refunds in full between raids, so an army is
effectively rebought each raid, and all gold earned so far counts as the budget.

**R** is how long the army lasts divided by how long the party lasts. Above 1 the army should win.
**R+** adds spells and traps: it is R / (1 - f), where f is the share of the party's life they remove,
capped at 0.8.

What goes into R:

- **Heroes:** the first 4 in a party are real Player bots, the rest are monster heroes. A bot's life
  includes its potions, each worth half its life.
- **Fear:** a hero below a quarter of its life rolls once to run; a bot only rolls once out of potions,
  and a monster champion never runs. The model takes half of hurt heroes to be alone, which always run.
  A runner stops fighting early, and half of runners get away, paying no bounty or XP.
- **Hit chances, from the engine:**
  - Monster melee against monster heroes rolls the monster's `toHit`, and armour plays no part.
  - Monster missiles and traps against monster heroes hit on `90 - armour - distance`, floor 5%.
  - Against bots, monsters use the player formula, including level difference and block.
  - Master spells hit monster heroes about 48% (the master is a Warrior, magic 10) and bots a flat 40%.
- **Monsters:** life is halved in single player, as the engine does. Each monster takes at least one
  blow, so swarms get credit for absorbing hits. An army can't use more than `MaxLairs` lairs.
- **The Keep Lord:** left out of R and reported on his own, because he stands at the end of the keep.
  See "The Keep Lord" below.

Assumptions that are not in the source are named at the top of the script:

- A raid lasts 90 seconds.
- A typical player lands 50% of what mana and cooldowns allow.
- Half the traps get walked past, and a party takes 12 seconds to pass a trap, in and out.
- There is a 0.3 second pause between swings.

R ignores corridors, pathing, hero AI, fear, stun and possession. Treat a difference of 0.3 as noise: the
army chosen changes the result from level to level.

## What was wrong

Numbers are from the revision before this pass (6025bb987), run through the same model.

1. **Late levels were impossible.** R+ on each level's last raid fell from 0.23 at level 1 to 0.03 at
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
   campaign level 20 did 741 per victim for 10 mana, while Wall did 250 and Wave 89. Apocalypse at 15 did
   519 to every hero in sight for 35 mana.
6. **Guardian did almost nothing.** Its shots take their damage from the Firebolt formula, using the
   master's magic and a Guardian spell level of 0: about 6.5 per shot at every level.
7. **Shock Trap hits repeatedly.** Its lightning rolls to hit on most ticks it lives, so the listed
   damage applied about 8 times.
8. **Progress ran ahead of the campaign.** Keeper level 20, the top unlock rank, arrived by the end of level 16.
   All 111 kinds were eligible with 4 levels to go, so rank stopped mattering.
9. **Pop made higher tiers pointless.** At 1 Pop per 12 power, DPS times life per Pop squared was flat
   across tiers, and 1-Pop units such as Bone Gasher (66) beat Reaper (38).
10. **Starter prices.** Bones at 65 gold and 2 Pop, and Archer at 100 and 2, have 1 life each in single
    player. Their whole family was priced off that.

## What changed

Every constant stays where it lived. New constants sit beside the code that uses them.

| Where | Constant | Before | After | Why |
|---|---|---|---|---|
| keeper_campaign.cpp | `CampaignLevels` strength | 100 to 400 | 100 to 215 | Solved for R+ near 0.9 to 1.6 on last raids, then smoothed |
| keeper_campaign.cpp | `CampaignLevels` start gold, levels 1 to 5 | 1200 to 1400 | 1500 | Levels 1 to 5 came out harder than the ones after |
| keeper_campaign.cpp | `CustomRules` start gold | 1200 | 1500 | Same repricing as the campaign's early levels |
| keeper_campaign.cpp | `HeroArmorStrengthPercent` (new) | 100 in effect | 50 | Keeps ranged monsters and traps useful (`ScaleArmorByHeroStrength`, heroes only) |
| keeper_heroes.cpp | `HeroStats`, `HeroMin/MaxDamagePerTenRaids` (new) | Warrior 30+10w life, 2+w to 6+2w damage, 10+3w armour | 33+7w, 3+0.6w to 7+1.2w, 11+2w | Raid 1 is unchanged; growth per raid is about 2/3 |
| keeper_heroes.cpp | same, Rogue | 20+7w, 1+w to 4+2w, 5+2w | 22+5w, 2+0.6w to 5+1.2w, 5+2w | |
| keeper_heroes.cpp | same, Sorcerer | 15+6w, 2+w to 5+2w, 3+w | 17+4w, 3+0.6w to 6+1.2w, 3+w | |
| keeper_heroes.cpp | `LordLifeBase`, `LordLifePerLevel`, `LordArmorBase` (new) | 600 life, 40 armour | 600 + 75 per level, 40 | Both scaled by full strength: see "The Keep Lord" |
| keeper_heroes.cpp | `HeroBountyBase`, `HeroBountyPerWave` (new) | 20 + 5w | 30 + 8w | Runners pay nothing, so kills pay more |
| keeper_bots.cpp | `BotBountyBase`, `BotBountyPerWave` (new) | 30 + 10w | 45 + 15w | Same |
| keeper_bots.cpp | `LevelsPerCampaignLevel` | 2 | 1 | See finding 3; accuracy table below |
| keeper_bots.cpp | `BotStatsPerRaid` (new) | 3, vitality scaled | 2, vitality not scaled | See finding 2 |
| keeper_bots.cpp | `BotPotionsBase`, `BotRaidsPerPotion` (new) | 2 + w/2 | 1 + w/4 | Potions counted for up to 4.5 times a bot's life |
| keeper_game.cpp | `SupplyPerLevel` | 2 | 3 | Level 20 reaches 109 Pop, not 90 |
| keeper_game.cpp | `RaisesPerEngineSpellLevel` (new) | 1 | 2 | The double-counting fix: see below |
| keeper_game.cpp | `HalfEngineLevelDamagePercent` (new) | none | 6 | Nova and Fireball gain on the raises with no engine level |
| keeper_game.cpp | `SpellManaCostAt` formula | step of base/15 or 1, floor at half | falls evenly to half at level 15 | So every raise of every spell makes it cheaper |
| keeper_game.cpp | `ApocalypseDamagePerLevel` | 20 | 8 | Apocalypse at 15 does 289 per victim, not 519 |
| keeper_game.cpp | `SpellKinds` Apocalypse | 70 mana, 20s | 80 mana, 25s | Cost floor 40 mana |
| keeper_game.cpp | `SpellKinds` Wave | 35 mana, 6s | 25 mana, 4s | Weakest damage per mana of any spell |
| keeper_game.cpp | `GuardianDamageBase`, `GuardianDamagePerRaise` (new) | about 6.5 a shot, always | 15 a shot, +3% per raise | See finding 6. Master level is left out because it already adds shots |
| keeper_game.cpp | `TrapDamagePerLevel` | 25 | 15 | A maxed trap was worth 11 times a level 1 trap; now about 6 times |
| keeper_game.cpp | `TrapRearmPerLevel`, `TrapRearmFloorPercent` | 6, 40 | 5, 50 | Same |
| keeper_game.hpp | `TrapKinds` gold | Fire 80, Arrow 50, Shock 120, Snare 100 | 100, 75, 140, 150 | Traps need no Pop, and they beat early monsters by far |
| keeper_game.hpp | `TrapKinds` damage | Arrow 8+3w, Shock 15+6w | Arrow 6+2w, Shock 4+2w | Shock now accounts for its repeat hits; both are still rough |
| keeper_roster.cpp | `KindPopPowerDivisor` | 12 | 20 | Higher tiers now pay per Pop |
| keeper_roster.cpp | Bones, Archer, Brute overrides | 65/2, 100/2, 260/4 | 40/1, 60/1, 220/3 | See finding 10 |
| keeper_profile.cpp | `XpPerLevelStep` | 100 | 120 | Keeper level is now about campaign level + 2 or 3 |
| keeper_profile.cpp | `RenownPerLevel` | 3 | 4 | 84 Renown by the end, not 69 |
| keeper_upgrades.cpp | `UpgradeNextCost` | fixed prices | price scaled by hero strength | Tiers add percentages, so they are worth more as armies grow |

**Logic changes**, each as small as the balance needed:

- `ApplyHeroStats` reads a table instead of a switch.
- `ApplyCast` sets a Guardian's shot damage, as it already did for Apocalypse, and adds the half step to
  Nova and Fireball.
- `SpellEngineLevel()` is new. The placement preview in keeper.cpp uses it, so the Chain Lightning radius
  it shows stays correct.
- `SpellManaCostScaled()` is new. Casting and readiness checks now use the exact cost in hundredths, and
  the displayed cost rounds up.
- `CanBuildAt` refuses a build when no lair or trap slot is free, with a reason. Before, the host dropped
  such a request silently. The Pop cap can pass `MaxLairs` (96) from about level 18. That is deliberate,
  since larger monsters use fewer lairs per Pop, and the slot check catches an all-small keep.

**Unchanged on purpose:**

- `MasterLevelsPerCampaignLevel` stays 2. Master level is the only damage growth for Wall, Inferno, Chain,
  Shock Wall, Wave and Ring, and it roughly keeps pace with hero life.
- Mana regeneration stays at 1.2 per second, with +25% per tier.
- Party sizes, raid counts, XP awards, Renown prices and tribute are unchanged. Gold income falls because
  tribute and bounties use the lower strength, and because runners pay nothing.

## The campaign, level by level

| Lvl | Name | Raids | Strength % | Start gold | Pop | Gold earned + start | KL after | Renown after | Kinds owned | Heroes last raid | Party life | Party DPS |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | Old Cellars | 5 | 100 -> 100 | 1200 -> 1500 | 16-32 | 3118 | 3 | 8 | 5 | 5 | 936 | 29 |
| 2 | Rat Warrens | 5 | 105 -> 105 | 1250 -> 1500 | 19-35 | 3195 | 4 | 12 | 9 | 5 | 979 | 29 |
| 3 | Sunken Chapel | 5 | 110 -> 110 | 1300 -> 1500 | 22-38 | 3277 | 5 | 16 | 11 | 5 | 1027 | 31 |
| 4 | Bone Pits | 6 | 115 -> 115 | 1350 -> 1500 | 25-45 | 4148 | 6 | 20 | 11 | 6 | 1232 | 41 |
| 5 | Black Vaults | 6 | 125 -> 120 | 1400 -> 1500 | 28-48 | 4270 | 7 | 24 | 13 | 6 | 1290 | 44 |
| 6 | Weeping Halls | 6 | 135 -> 125 | 1500 -> 1500 | 31-51 | 4749 | 8 | 28 | 14 | 7 | 1482 | 51 |
| 7 | Ashen Crypts | 7 | 145 -> 130 | 1600 -> 1600 | 34-58 | 6006 | 9 | 32 | 14 | 7 | 1665 | 60 |
| 8 | Iron Gaol | 7 | 155 -> 135 | 1700 -> 1700 | 37-61 | 6390 | 10 | 36 | 15 | 8 | 1840 | 71 |
| 9 | Maggot Deeps | 7 | 165 -> 140 | 1800 -> 1800 | 40-64 | 6672 | 11 | 40 | 16 | 8 | 1910 | 72 |
| 10 | Hollow Court | 8 | 180 -> 140 | 1950 -> 1950 | 43-71 | 8247 | 12 | 44 | 17 | 9 | 2572 | 88 |
| 11 | Blood Galleries | 8 | 195 -> 140 | 2100 -> 2100 | 46-74 | 8397 | 13 | 48 | 18 | 9 | 2572 | 90 |
| 12 | Shrouded Nave | 8 | 210 -> 145 | 2250 -> 2250 | 49-77 | 9186 | 14 | 52 | 18 | 9 | 2663 | 94 |
| 13 | Gallows Reach | 8 | 225 -> 150 | 2400 -> 2400 | 52-80 | 9729 | 15 | 56 | 19 | 10 | 2954 | 107 |
| 14 | Cinder Keep | 9 | 240 -> 155 | 2600 -> 2600 | 55-87 | 12081 | 16 | 60 | 19 | 11 | 3424 | 130 |
| 15 | Howling Stair | 9 | 260 -> 160 | 2800 -> 2800 | 58-90 | 12599 | 17 | 64 | 20 | 11 | 3532 | 136 |
| 16 | Tomb of Kings | 9 | 280 -> 165 | 3000 -> 3000 | 61-93 | 13745 | 18 | 68 | 20 | 12 | 3773 | 149 |
| 17 | Silent Abbey | 9 | 300 -> 170 | 3200 -> 3200 | 64-96 | 14312 | 19 | 72 | 21 | 12 | 3888 | 154 |
| 18 | Chains of Night | 10 | 325 -> 180 | 3500 -> 3500 | 67-103 | 17931 | 20 | 76 | 21 | 13 | 4670 | 198 |
| 19 | Burning Sepulcre | 10 | 350 -> 190 | 3800 -> 3800 | 70-106 | 19026 | 21 | 80 | 21 | 13 | 4927 | 209 |
| 20 | Throne of Ash | 10 | 400 -> 215 | 4200 -> 4200 | 73-109 | 23230 | 22 | 84 | 22 | 14 | 5806 | 244 |

"Gold earned + start" counts the expected bounties, with none of it spent, so it is a ceiling.

## Combat: last raid of each level

Army names show what the model's player would field, not a recommendation.

| Lvl/raid | Life before | Life after | Best army before | Best army after | R before | R after | Swarm before | R+ before | Swarm after | R+ after | R+ co-op | Lord: party / quarter / would kill in |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1/5 | 1528 | 936 | 5 Brute | 7 Brute + 4 Fallen | 0.15 | 0.74 | 16 Bones: 0.02 | 0.23 | 32 Fallen: 0.08 | 1.29 | 2.27 | 28s / 86s / 67s |
| 2/5 | 1599 | 979 | 30 Fiend | 7 Brute + 4 Fiend | 0.15 | 0.74 | 17 Bones: 0.01 | 0.28 | 35 Fallen: 0.08 | 1.42 | 2.50 | 33s / 104s / 69s |
| 3/5 | 1702 | 1027 | 24 Blink + 1 Fallen | 22 Carcass | 0.19 | 0.65 | 18 Bones: 0.01 | 0.35 | 38 Fallen: 0.08 | 1.31 | 2.32 | 37s / 112s / 70s |
| 4/6 | 2392 | 1232 | 4 Winged + 18 Blink | 10 Brute + 2 Fiend | 0.13 | 0.56 | 21 Bones: 0.01 | 0.24 | 45 Fallen: 0.06 | 1.27 | 2.23 | 32s / 138s / 86s |
| 5/6 | 2646 | 1290 | 13 Gloom + 4 Fallen | 10 Brute + 2 Fiend | 0.14 | 0.49 | 22 Bones: 0.01 | 0.27 | 48 Fallen: 0.06 | 1.25 | 2.16 | 34s / 149s / 88s |
| 6/6 | 3119 | 1482 | 16 Gloom + 1 Zombie | 9 Stone Clan + 4 Brute | 0.12 | 0.48 | 23 Bones: 0.00 | 0.22 | 51 Fallen: 0.05 | 1.34 | 2.29 | 32s / 119s / 94s |
| 7/7 | 3755 | 1665 | 48 Blink + 4 Fallen | 2 Stone Clan + 19 Flesh Clan | 0.09 | 0.34 | 26 Bones: 0.01 | 0.17 | 58 Fallen: 0.06 | 1.24 | 2.43 | 30s / 117s / 111s |
| 8/7 | 4225 | 1840 | 3 Gloom + 48 Blink | 12 Fire Clan + 2 Burning Axe | 0.08 | 0.42 | 27 Bones: 0.00 | 0.14 | 61 Fallen: 0.05 | 1.37 | 2.36 | 30s / 135s / 119s |
| 9/7 | 4594 | 1910 | 10 Gloom + 36 Blink | 10 Night Clan + 1 Fiend | 0.07 | 0.44 | 28 Bones: 0.00 | 0.15 | 64 Fallen: 0.05 | 1.63 | 2.70 | 33s / 147s / 123s |
| 10/8 | 6543 | 2572 | 13 Gargoyle + 23 Fiend | 13 Night Clan + 2 Flesh Clan | 0.05 | 0.43 | 31 Bones: 0.00 | 0.09 | 71 Fallen: 0.04 | 1.17 | 1.93 | 29s / 112s / 183s |
| 11/8 | 7284 | 2572 | 14 Gargoyle + 22 Blink | 8 Night Clan + 9 Brute | 0.05 | 0.32 | 32 Bones: 0.00 | 0.09 | 74 Fallen: 0.05 | 0.91 | 1.68 | 29s / 118s / 188s |
| 12/8 | 8134 | 2663 | 12 Blood Claw + 18 Blink | 11 Night Clan + 8 Brute | 0.07 | 0.37 | 33 Bones: 0.00 | 0.11 | 77 Fallen: 0.05 | 1.37 | 2.50 | 31s / 128s / 194s |
| 13/8 | 9328 | 2954 | 11 Blood Claw + 24 Blink | 12 Night Clan + 8 Brute | 0.06 | 0.32 | 34 Bones: 0.00 | 0.09 | 80 Fallen: 0.05 | 1.01 | 1.85 | 30s / 138s / 206s |
| 14/9 | 11418 | 3424 | 18 Blood Claw + 2 Blink | 11 Night Clan + 17 Stone Clan | 0.05 | 0.31 | 37 Bones: 0.00 | 0.07 | 87 Fallen: 0.05 | 0.92 | 1.79 | 27s / 140s / 264s |
| 15/9 | 12817 | 3532 | 8 Death Wing + 28 Blink | 18 Night Clan + 7 Stone Clan | 0.04 | 0.32 | 38 Bones: 0.00 | 0.05 | 90 Fallen: 0.05 | 0.96 | 1.88 | 29s / 151s / 278s |
| 16/9 | 14575 | 3773 | 8 Death Wing + 30 Blink | 15 Stone Clan + 16 Night Clan | 0.03 | 0.35 | 39 Bones: 0.00 | 0.04 | 93 Fallen: 0.05 | 1.07 | 2.08 | 30s / 164s / 305s |
| 17/9 | 16139 | 3888 | 7 Death Wing + 38 Blink | 12 Stone Clan + 19 Night Clan | 0.03 | 0.36 | 40 Bones: 0.00 | 0.04 | 96 Fallen: 0.05 | 1.11 | 1.74 | 32s / 180s / 326s |
| 18/10 | 22439 | 4670 | 7 Reaper + 9 Blink | 32 Night Clan + 4 Fire Clan | 0.03 | 0.46 | 43 Bones: 0.00 | 0.04 | 96 Fallen: 0.04 | 1.15 | 1.72 | 27s / 105s / 404s |
| 19/10 | 25114 | 4927 | 8 Reaper | 24 Night Clan + 54 Carcass | 0.03 | 0.53 | 44 Bones: 0.00 | 0.04 | 96 Fallen: 0.04 | 1.32 | 1.81 | 29s / 115s / 407s |
| 20/10 | 31426 | 5806 | 8 Reaper + 2 Blink | 11 Reaper + 2 Night Clan | 0.03 | 0.62 | 45 Bones: 0.00 | 0.03 | 96 Fallen: 0.03 | 1.35 | 2.67 | 34s / 230s / 419s |

Against the same model, R+ went from 0.23, 0.27, 0.09, 0.05, 0.03 at levels 1, 5, 10, 15, 20 to 1.29,
1.25, 1.17, 0.96, 1.35, and to 0.91 to 1.63 across all 20 levels. The Lord's column reads "lasts against the
whole party / a quarter of it / would kill the whole party alone".

## The Keep Lord

R leaves the Lord out, so he has his own figure: how long he alone lasts if heroes reach him. Before this
fix he lasted 7 seconds against a level 20 party and 39 against a quarter of it, so a small leak could end
the keep quickly. His life grew only with strength, while party damage grows with party size as well.

| Level | Life before | Life now | Lasts against party: before | now | Against a quarter: before | now |
|---|---|---|---|---|---|---|
| 1 | 600 | 600 | 28s | 28s | 86s | 86s |
| 5 | 720 | 1080 | 21s | 34s | 90s | 149s |
| 10 | 840 | 1785 | 12s | 29s | 47s | 112s |
| 15 | 960 | 2640 | 9s | 29s | 45s | 151s |
| 20 | 1290 | 4353 | 7s | 34s | 39s | 230s |

"Before" here is the first commit of this pass. Life shown has no upgrades, and a level's Lord Life tiers
add to it. His armour now takes full strength again, 86 at level 20: halving it was for monster missiles
and traps, and those never target him. Monster Rogues and Sorcerers hit him at the 5% floor there.

## Custom maps

Custom maps use `CustomRules` and count as level 1 for Pop and master level, whatever the profile holds.

| Keeper | Keeper level | Start gold | Heroes | Party life | Best army | R | R+ | Lord lasts |
|---|---|---|---|---|---|---|---|---|
| New keeper | 1 | 1500 | 6 | 1160 | 10 Brute + 2 Fallen | 0.67 | 1.27 | 18s |
| After level 5 | 7 | 1500 | 6 | 1160 | 11 Stone Clan + 1 Brute | 0.72 | 1.60 | 18s |
| After level 10 | 12 | 1500 | 6 | 1160 | 10 Brute + 1 Zombie | 0.66 | 2.28 | 18s |
| After level 20 | 22 | 1500 | 6 | 1160 | 1 Crypt Demon + 1 Reaper | 1.67 | 5.25 | 18s |

A new keeper can hold, and a veteran's unlocks make it easy. That follows from custom maps having a single
set of rules.

A level's first raid is far easier than its last:

| Lvl/raid | Heroes | Party life | Army | R | R+ |
|---|---|---|---|---|---|
| 1/1 | 2 | 151 | 4 Brute + 3 Zombie | 17.80 | 89.01 |
| 5/1 | 2 | 181 | 1 Carcass + 4 Brute | 8.80 | 43.99 |
| 10/1 | 3 | 266 | 4 Night Clan | 3.08 | 15.42 |
| 15/1 | 4 | 452 | 4 Fire Clan + 4 Brute | 2.04 | 10.18 |
| 20/1 | 6 | 686 | 2 Reaper + 1 Night Clan | 3.76 | 18.81 |

So most of each level is a build-up, and the difficulty is in its last two or three raids. That follows
from heroes growing with the raid number and is not new, though the gap is smaller than it was.

## XP, Renown and unlocks

| Lvl | KL after: before | KL after: now | Renown: before | Renown: now | Kinds eligible: before | Kinds eligible: now | Renown for all eligible (now) | Kinds owned (now) | Key spells (now) |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 3 | 3 | 6 | 8 | 17 | 17 | 12 | 5 |  |
| 5 | 8 | 7 | 21 | 24 | 43 | 39 | 56 | 13 | Nova 4, Fire 1 |
| 10 | 13 | 12 | 36 | 44 | 61 | 59 | 119 | 17 | Nova 4, Fire 3, Chai 1 |
| 15 | 19 | 17 | 54 | 64 | 104 | 91 | 263 | 20 | Nova 4, Fire 6, Chai 1 |
| 20 | 24 | 22 | 69 | 84 | 111 | 111 | 370 | 22 | Nova 4, Fire 7, Chai 1, Apoc 1 |

Catalogue: all kinds 370 Renown, spell unlocks 29, one item raised 1 to 15 costs 44.

- **Warband by level 5, 10, 15, 20:** 13, 17, 20 and 22 kinds owned, with spells to match. Nobody can
  own everything, and that seems right: respec is free, so a player can change direction.
- **Unlock gating:** keeper level 18 (Apocalypse) is reached by the end of level 16 and rank 20 by the end
  of level 18. Before, those came by the end of levels 14 and 16. Chain arrives about level 7, Fireball
  about level 4.
- **Dead zones:** none. Every level adds keeper levels, so Renown arrives every level and there is always
  something to buy. Rank stays a limit on what is available until late in the campaign.
- **Existing profiles:** a profile keeps its XP but drops keeper levels under the steeper curve. Purchases
  stay, and Renown can show 0 for a while. Nobody has a profile that matters yet.

## Gold and Pop

Now:

| Lvl | Start | Tribute | Bounties | Total | Pop first | Pop last | Army gold first | Army gold last | Upgrade tiers |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1500 | 650 | 968 | 3118 | 16 | 32 | 1000 | 1680 | 2 |
| 5 | 1500 | 1050 | 1720 | 4270 | 28 | 48 | 960 | 2290 | 2 |
| 10 | 1950 | 1960 | 4337 | 8247 | 43 | 71 | 1280 | 4430 | 3 |
| 15 | 2800 | 2720 | 7079 | 12599 | 58 | 90 | 1880 | 7055 | 4 |
| 20 | 4200 | 4350 | 14680 | 23230 | 73 | 109 | 2710 | 13785 | 5 |

Before:

| Lvl | Start | Tribute | Bounties | Total | Pop first | Pop last | Army gold first | Army gold last | Upgrade tiers |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1200 | 650 | 644 | 2494 | 16 | 32 | 720 | 1300 | 2 |
| 5 | 1400 | 1092 | 1183 | 3675 | 24 | 44 | 900 | 2090 | 2 |
| 10 | 1950 | 2520 | 3677 | 8147 | 34 | 62 | 1350 | 4740 | 4 |
| 15 | 2800 | 4420 | 7553 | 14773 | 44 | 76 | 1725 | 6280 | 8 |
| 20 | 4200 | 8100 | 17852 | 30152 | 54 | 90 | 2390 | 9680 | 20 |

- **Binding limit:** gold, on every raid of every level in the model. Before, Pop bound last raids from level 14,
and gold piled up unspent. Pop is now close enough to matter when a player saves well.
- **Cheap swarm:** not dominant. A full keep of the cheapest kind scores R 0.03 to 0.08 on last raids and
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
| 5 | 9 | Nova | 4 | 7 | 31.2 | 6.0 | 64 | 55 | 1.7 | 4% |
| 5 | 9 | Nova | 15 | 13 | 17.5 | 6.0 | 123 | 105 | 6.0 | 8% |
| 5 | 9 | Wall | 1 | 4 | 25.0 | 4.0 | 100 | 85 | 3.4 | 7% |
| 5 | 9 | Wall | 15 | 11 | 12.5 | 4.0 | 100 | 85 | 6.8 | 7% |
| 5 | 9 | Guard | 1 | 4 | 30.0 | 8.0 | 120 | 51 | 1.7 | 4% |
| 5 | 9 | Guard | 15 | 11 | 15.0 | 8.0 | 315 | 134 | 9.0 | 10% |
| 5 | 9 | Stone | 1 | 4 | 20.0 | 5.0 | 0 | 0 | 0.0 | 0% |
| 5 | 9 | Stone | 15 | 11 | 10.0 | 5.0 | 0 | 0 | 0.0 | 0% |
| 5 | 9 | Inferno | 1 | 2 | 15.0 | 2.0 | 49 | 42 | 2.8 | 3% |
| 5 | 9 | Inferno | 15 | 9 | 7.5 | 2.0 | 49 | 42 | 5.5 | 3% |
| 5 | 9 | Fireball | 1 | 3 | 20.0 | 3.0 | 57 | 49 | 2.4 | 4% |
| 5 | 9 | Fireball | 15 | 10 | 10.0 | 3.0 | 130 | 111 | 11.1 | 9% |
| 10 | 19 | Nova | 4 | 7 | 31.2 | 6.0 | 88 | 78 | 2.5 | 3% |
| 10 | 19 | Nova | 15 | 13 | 17.5 | 6.0 | 169 | 150 | 8.6 | 6% |
| 10 | 19 | Wall | 1 | 4 | 25.0 | 4.0 | 150 | 133 | 5.3 | 5% |
| 10 | 19 | Wall | 15 | 11 | 12.5 | 4.0 | 150 | 133 | 10.7 | 5% |
| 10 | 19 | Guard | 1 | 4 | 30.0 | 8.0 | 195 | 87 | 2.9 | 3% |
| 10 | 19 | Guard | 15 | 11 | 15.0 | 8.0 | 420 | 187 | 12.4 | 7% |
| 10 | 19 | Stone | 1 | 4 | 20.0 | 5.0 | 0 | 0 | 0.0 | 0% |
| 10 | 19 | Stone | 15 | 11 | 10.0 | 5.0 | 0 | 0 | 0.0 | 0% |
| 10 | 19 | Inferno | 1 | 2 | 15.0 | 2.0 | 86 | 77 | 5.1 | 3% |
| 10 | 19 | Inferno | 15 | 9 | 7.5 | 2.0 | 86 | 77 | 10.2 | 3% |
| 10 | 19 | Fireball | 3 | 4 | 18.6 | 3.0 | 96 | 85 | 4.6 | 3% |
| 10 | 19 | Fireball | 15 | 10 | 10.0 | 3.0 | 195 | 173 | 17.3 | 7% |
| 10 | 19 | Chain | 1 | 3 | 30.0 | 5.0 | 86 | 153 | 5.1 | 6% |
| 10 | 19 | Chain | 15 | 10 | 15.0 | 5.0 | 126 | 225 | 15.0 | 9% |
| 10 | 19 | Shock Wall | 1 | 3 | 30.0 | 5.0 | 180 | 240 | 8.0 | 9% |
| 10 | 19 | Shock Wall | 15 | 10 | 15.0 | 5.0 | 180 | 240 | 16.0 | 9% |
| 10 | 19 | Wave | 1 | 3 | 25.0 | 4.0 | 49 | 65 | 2.6 | 3% |
| 10 | 19 | Wave | 15 | 10 | 12.5 | 4.0 | 49 | 65 | 5.2 | 3% |
| 20 | 39 | Nova | 4 | 7 | 31.2 | 6.0 | 137 | 125 | 4.0 | 2% |
| 20 | 39 | Nova | 15 | 13 | 17.5 | 6.0 | 261 | 239 | 13.6 | 4% |
| 20 | 39 | Wall | 1 | 4 | 25.0 | 4.0 | 250 | 229 | 9.1 | 4% |
| 20 | 39 | Wall | 15 | 11 | 12.5 | 4.0 | 250 | 229 | 18.3 | 4% |
| 20 | 39 | Guard | 1 | 4 | 30.0 | 8.0 | 345 | 158 | 5.3 | 3% |
| 20 | 39 | Guard | 15 | 11 | 15.0 | 8.0 | 630 | 288 | 19.2 | 5% |
| 20 | 39 | Stone | 1 | 4 | 20.0 | 5.0 | 0 | 0 | 0.0 | 0% |
| 20 | 39 | Stone | 15 | 11 | 10.0 | 5.0 | 0 | 0 | 0.0 | 0% |
| 20 | 39 | Inferno | 1 | 2 | 15.0 | 2.0 | 161 | 147 | 9.8 | 3% |
| 20 | 39 | Inferno | 15 | 9 | 7.5 | 2.0 | 161 | 147 | 19.7 | 3% |
| 20 | 39 | Fireball | 7 | 6 | 15.7 | 3.0 | 203 | 185 | 11.8 | 3% |
| 20 | 39 | Fireball | 15 | 10 | 10.0 | 3.0 | 325 | 297 | 29.7 | 5% |
| 20 | 39 | Chain | 1 | 3 | 30.0 | 5.0 | 161 | 295 | 9.8 | 5% |
| 20 | 39 | Chain | 15 | 10 | 15.0 | 5.0 | 236 | 432 | 28.8 | 7% |
| 20 | 39 | Shock Wall | 1 | 3 | 30.0 | 5.0 | 300 | 411 | 13.7 | 7% |
| 20 | 39 | Shock Wall | 15 | 10 | 15.0 | 5.0 | 300 | 411 | 27.4 | 7% |
| 20 | 39 | Wave | 1 | 3 | 25.0 | 4.0 | 89 | 122 | 4.9 | 2% |
| 20 | 39 | Wave | 15 | 10 | 12.5 | 4.0 | 89 | 122 | 9.8 | 2% |
| 20 | 39 | Ring | 1 | 3 | 40.0 | 8.0 | 250 | 343 | 8.6 | 6% |
| 20 | 39 | Ring | 15 | 10 | 20.0 | 8.0 | 250 | 343 | 17.1 | 6% |
| 20 | 39 | Apocalypse | 1 | 1 | 80.0 | 25.0 | 136 | 499 | 6.2 | 9% |
| 20 | 39 | Apocalypse | 15 | 8 | 40.0 | 25.0 | 289 | 1058 | 26.5 | 18% |

Before, at level 20:

| Lvl | Master clvl | Spell | Raised to | Engine lvl | Mana | Cooldown s | Dmg/victim | Expected total | Per mana | % of party life |
|---|---|---|---|---|---|---|---|---|---|---|
| 20 | 39 | Nova | 3 | 8 | 31 | 6.0 | 145 | 133 | 4.3 | 0% |
| 20 | 39 | Nova | 15 | 20 | 17 | 6.0 | 596 | 545 | 32.0 | 2% |
| 20 | 39 | Wall | 1 | 4 | 25 | 4.0 | 250 | 229 | 9.1 | 1% |
| 20 | 39 | Wall | 15 | 18 | 12 | 4.0 | 250 | 229 | 19.0 | 1% |
| 20 | 39 | Guard | 1 | 4 | 30 | 8.0 | 150 | 68 | 2.3 | 0% |
| 20 | 39 | Guard | 15 | 18 | 15 | 8.0 | 195 | 89 | 5.9 | 0% |
| 20 | 39 | Stone | 1 | 4 | 20 | 5.0 | 0 | 0 | 0.0 | 0% |
| 20 | 39 | Stone | 15 | 18 | 10 | 5.0 | 0 | 0 | 0.0 | 0% |
| 20 | 39 | Inferno | 1 | 2 | 15 | 2.0 | 161 | 147 | 9.8 | 0% |
| 20 | 39 | Inferno | 15 | 16 | 7 | 2.0 | 161 | 147 | 21.1 | 0% |
| 20 | 39 | Fireball | 5 | 7 | 16 | 3.0 | 228 | 209 | 13.0 | 1% |
| 20 | 39 | Fireball | 15 | 17 | 10 | 3.0 | 741 | 677 | 67.7 | 2% |
| 20 | 39 | Chain | 1 | 3 | 30 | 5.0 | 161 | 295 | 9.8 | 1% |
| 20 | 39 | Chain | 15 | 17 | 15 | 5.0 | 312 | 570 | 38.0 | 2% |
| 20 | 39 | Shock Wall | 1 | 3 | 30 | 5.0 | 300 | 411 | 13.7 | 1% |
| 20 | 39 | Shock Wall | 15 | 17 | 15 | 5.0 | 300 | 411 | 27.4 | 1% |
| 20 | 39 | Wave | 1 | 3 | 35 | 6.0 | 89 | 122 | 3.5 | 0% |
| 20 | 39 | Wave | 15 | 17 | 17 | 6.0 | 89 | 122 | 7.2 | 0% |
| 20 | 39 | Ring | 1 | 3 | 40 | 8.0 | 250 | 343 | 8.6 | 1% |
| 20 | 39 | Ring | 15 | 17 | 20 | 8.0 | 250 | 343 | 17.1 | 1% |
| 20 | 39 | Apocalypse | 1 | 1 | 70 | 20.0 | 136 | 499 | 7.1 | 2% |
| 20 | 39 | Apocalypse | 15 | 15 | 35 | 20.0 | 519 | 1897 | 54.2 | 6% |

**Double counting.** Nova, Fireball and Apocalypse multiply a master-level term by a spell-level term.
Nova and Fireball compound 12.5% per engine level, so raising 14 times multiplied them by 5.2 on top of
master level's 4.2. Raises now give half an engine level, which makes that 2.3, and Apocalypse's 8% per
raise gives at most 2.1 where it was 3.8.

**Every raise now does something.** Engine levels only move on every second raise. On the others, Nova and
Fireball take 6% more damage. Mana cost now falls evenly to half at level 15, so every raise of every spell
is cheaper to cast, by about 0.5 to 2.9 mana. Odd raises also lengthen walls and add Guardian shots.

**Dominant or useless.**

- Spell damage per mana at 15 raises and level 20 now spans 9.8 to 29.7. Before it spanned 5.9 to 67.7.
- Guardian, a rank 1 spell, is no longer the best per mana: 19.2 at 15 raises against Fireball's 29.7. That
  already counts all of a cast's shots. Casts can stack (3 at once at level 20, with 24-second life and the
  8-second cooldown), but each only fires what the table counts, about 26 DPS for all three.
- Wave remains the weakest damage spell, though the "hits per victim" figure for it is the roughest guess
  in the model.
- Wall and Ring don't scale with raises, except that they last longer and cost less.

**Apocalypse.** It is available from about level 17, and the model's player buys it at level 18. At raise 1 it costs 80 mana, 9% of a last raid per cast. At 15
it costs 40 mana and 25 seconds, 18% per cast, 3 or 4 casts a raid if the master does nothing else. That
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
| 1 | 2748 | 9710 | 311% | 2 | 29 |
| 5 | 3679 | 11652 | 273% | 2 | 29 |
| 10 | 7015 | 13594 | 165% | 3 | 29 |
| 15 | 10738 | 15536 | 123% | 4 | 29 |
| 20 | 19823 | 20866 | 90% | 5 | 29 |

Before:

| Lvl | Gold before last raid | All upgrades cost | % of gold earned in the level | Tiers bought by last raid | Tiers on offer |
|---|---|---|---|---|---|
| 1 | 2249 | 9710 | 389% | 2 | 29 |
| 5 | 3271 | 9710 | 264% | 2 | 29 |
| 10 | 7112 | 9710 | 119% | 4 | 29 |
| 15 | 12810 | 9710 | 66% | 8 | 29 |
| 20 | 26052 | 9710 | 32% | 20 | 29 |

Before, level 20 could buy everything for 32% of its gold. Now the full set costs 90% of a level 20's
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
6. **Heroes running away.** How often hurt heroes are alone, and how many runners get caught, are guesses.
   They set about a third of bounties and XP. If income looks off, look here first. Falling back isn't
   modelled.
7. **Trap avoidance.** Heroes route around known traps and Rogues disarm them. The model only applies flat
   factors. Trap prices are the least certain gold numbers here.
8. **Late monster heroes' armour.** Check whether ranged monsters feel worthwhile at 40% to 60% to hit.
9. **Co-op.** Monsters have full life with more than one player, so R+ is about 1.9 to 2.7 even before
   counting a second master's spells. `--coop` shows this. Co-op may be too easy, and nothing scales the
   party by player count.
10. **Hero theft.** Gold assumes nothing is stolen. Heavy theft would make gold tighter than shown.
11. **Possession, patrols and bait** aren't modelled at all.

Late levels were judged on models, not on play. If the playtest stream reports that a level is out of
line, adjust that level's strength first: it is the one number meant to carry a level's difficulty.
