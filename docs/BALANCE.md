# The Dark Keep: balance

Tuned in two rounds: first by calculation, then against real raids run by a test harness. No person has
played these numbers. Sections below "Measured raids" come from the paper model, recalibrated to the raids,
and only compare levels with each other.

**Not verified against newer features.** Every number here predates bot gear, the Monk, the block cap, thieves
and the Golem (those made heroes stronger: in their own tests level 10 full support went from 7 of 12 raids
held to 4 of 12, and level 20 from 9 of 12 to 6 of 12). It also predates raid types, difficulty, master scaling
at 125-140%, interest and early call, level 1 tips and map changes. Expect the table to need another pass.

## Measured raids

`scripts/dev/balance_raids.py` runs real raids invisibly: the last raid of a level, at the model's army, in a ring
around the vault. `scripts/dev/balance_summary.py` sums them up. Every cell is 8 raids, so one cell can't tell 60%
from 90%: read rows and totals, not single cells.

### Results on the final constants

| Lvl | Army | Traps | Spells | Sloppy | Full |
|---|---|---|---|---|---|
| 1 | 5/8 | 1/8 | 3/8 | 4/8 | 6/8 |
| 5 | 0/8 | 1/8 | 2/8 | 4/8 | 7/8 |
| 10 | 0/8 | 0/8 | 3/8 | 3/8 | 7/8 |
| 15 | 0/8 | 0/8 | 1/8 | 2/8 | 5/8 |
| 18 | - | - | - | - | 8/8 |
| 20 | 0/8 | 1/8 | 2/8 | 0/8 | 6/8 |

- **Army:** the model's army and the Lord, nothing else.
- **Traps:** adds the model's traps along the way in.
- **Spells:** adds spells, at half of what mana allows, aimed at a hero.
- **Sloppy:** traps and spells at half that casting rate (about a quarter of the mana), no upgrades.
- **Full:** traps, spells at half the mana, and the model's upgrades.

To run these again: `--modes army,traps,spells,sloppy,full`, with a plan from `balance_model.py --plan`.

**The Spells and Sloppy columns are overstated.** Those raids cast at the rate worked out with the mana upgrades, though those modes buy
none. For Spells that was 14% over half the mana on levels 1, 5 and 10, 27% on level 15 and 36% on level 20; Sloppy was over its rate by the same
share. The harness now sets the rate per mode. The columns above are from before that fix and were not re-run.

Totals: army 5 of 40, traps 3 of 40, spells 11 of 40, sloppy 13 of 40, full 39 of 48.

Baseline, on round 1's constants and the same harness: army 2 of 30, full 36 of 40.

### Targets, and why

| Mode | Levels 1-5 | Levels 10-15 | Levels 18-20 |
|---|---|---|---|
| Army alone | up to 1 in 8 | up to 1 in 8 | up to 1 in 8 |
| One support alone, or sloppy | 50% or more | 40-60% | 25-50% |
| Full, as the harness plays | 90% or more | 80-90% | 70-85% |

The aim is a win that needs an army and some spells or traps, so an army alone should fail. Early levels are
easier on purpose: they are a new player's first sight of the game, and a real start with five Brutes and
zombies fell at raid 4. Late levels are harder because a player there knows the game.

**Allowing for a human.** The harness places everything with perfect knowledge, acts at once and aims every
spell at a hero's tile, so a person will do worse at that. It also never rallies, possesses, baits, locks doors or
reacts, so a person can do better there. I made no attempt to measure a real player. Instead:

- Spells are limited to half the mana, not all of it.
- Full support is deliberately set above a coin flip.
- The sloppy row is a rough stand-in for a player who casts half as often and buys no upgrades. It is only a stand-in:
  it doesn't model poor placement, because the ring around the vault is the same.

Lost raids can now be retried, which softens a miss.

**Where the table misses its targets.** With 8 raids a cell, misses of one or two raids aren't evidence of anything. The ones
that look real:

- Level 1 army alone won 5 of 8, against a target of 1 in 8. Level 1 doesn't need support yet. I left it: a first level
  that forgives is what the data supports, but it means level 1 teaches less about spells.
- One support alone is under target: spells 11 of 40 and traps 3 of 40, with most cells below their band. Levels mostly
  need both.
- Sloppy play falls well short late (0 of 8 on level 20, 2 of 8 on level 15). A careless player will stall around level 15.
- Level 15 full support was 5 of 8, against a target of 80-90%.

### What the raids taught

1. **Nova was the real distortion.** On level 10, at equal raises and mana, alone with the army, of 6 raids each:

   | Spell | Wins | Life removed |
   |---|---|---|
   | Nova, before | 6/6 | 99% |
   | Wall | 4/6, by stalling: raids ran 176-599s | 99% |
   | Shock Wall, before | 6/6, with 8-10 casts | 99% |
   | Chain | 0/6 | 62% |
   | Nova, at `NovaDamagePercent` 30 | 0/6 | 56% |
   | Shock Wall, at 40 mana and engine level 0 | 0/6 | 53% |
   | Guardian | 0/6 | 38% |
   | Fireball | 0/6 | 33% |
   | Inferno | 0/6 | 28% |
   | No spells (army alone, 4 raids) | 0/4 | 25% |

   Nova won with 3 or 4 casts. The likely reason, from the code rather than measured directly, is that the engine fires its
   ring of 36 balls from the caster, and here it bursts on the target, so a hero there takes most of them. Wins before this
   fix leaned on it, and the old level-to-level differences were largely whether Nova was in the rotation.
2. **Wall stalls raids.** Heroes refuse to enter fire, so repeated walls hold a party for minutes while mana comes back. It
   won 4 of 6 alone. I left this alone: it needs a real player's view of whether it's a good tactic or a dull one.
3. **Hero strength is a weak lever.** Tribute and bounties scale with it, so level 15 won 3 of 8 at both 170% and 145%.
   Bounties and start gold move results more.
4. **Unit tier matters far more than the model thought.** Armies of Reapers won 16 of 16 on level 20, when the model's
   player one level earlier, still on rank 10 Night Clans, won 0 of 8. The model's player now buys the strongest thing on offer.
5. **Rogues disarm most traps.** Usually only 0-8 of up to 16 traps were left at the end of a raid. Traps
   alone added about 15 points of party life on level 5, 15 on level 10 and little on levels 15 and 20, for a quarter of the
   gold. Disarm chances are now lower, and traps' value still needs a person's view.
6. **Runs must differ.** The game's random numbers repeat from the same start, so identical runs came out nearly identical and
   looked far more certain than they were. The harness now varies each layout.
7. **Some raids never end.** Four of the 40 traps-only raids on the final table hit the harness's limit, three on level 1 and one on level 10. On level 1 the party stands in a line at 57,67 to 57,69 with the raid never ending, for 300 real seconds at speed. That looks like heroes refusing to pass a trap in a one-wide corridor. It is a possible stall bug, not a balance number, and needs someone to look at it.

### Round 2 constants

| Where | Constant | Round 1 | Now | Why |
|---|---|---|---|---|
| keeper_campaign.cpp | `CampaignLevels` strength | 100-215 | 90, 95, 105, 110, 120, 125 x5, 130, 135, 140, 140, 145, 145, 150, 165, 180, 205 | Measured per level, with level 1 easier on purpose |
| keeper_campaign.cpp | `CampaignLevels` start gold, levels 1-4 | 1500 | 1800, 1800, 1700, 1600 | Level 1 full support won 3-8 of 8 from one round to the next |
| keeper_campaign.cpp | `CustomRules` | 1500 gold, 100% | 1800 gold, 90% | Matches level 1 |
| keeper_game.cpp | `NovaDamagePercent` (new) | 100 in effect | 30 | See above |
| keeper_game.cpp | `SpellKinds` Shock Wall | 30 mana, engine level 3 | 40 mana, engine level 0 | Its walls lasted 51s and piled up: 6 of 6 alone on level 10. Now 0 of 6, 53% removed |
| keeper_heroes.cpp | `HeroBountyBase`, `HeroBountyPerWave` | 30 + 8w | 36 + 10w | Income moves results where strength doesn't |
| keeper_bots.cpp | `BotBountyBase`, `BotBountyPerWave` | 45 + 15w | 54 + 18w | Same |
| keeper_bots.cpp | `RogueDisarmPercent` | 60 | 40 | Traps were mostly disarmed |
| keeper_heroes.cpp | `RogueDisarmPercent` | 50 | 35 | Same |

### How the model changed to fit

| Assumption | Was | Now | From |
|---|---|---|---|
| Raid length | 90s | 100s to level 15, rising to 220s at 20 | 68-137s, and 139-389s on level 20 |
| `ARMY_EFFICIENCY` on R (new) | 1 | 0.7 | Armies alone removed about 0.7 of R's share of a party |
| Time in fire, `EXPOSURE_SECONDS` | 2s | 0.5s | Wall 11-48 and Ring 12 at master level 35; heroes step out |
| Inferno, `INFERNO_SECONDS` | 2s, 2 victims | 0.55s, 1 victim | 19 at master level 35: a narrow jet |
| Wave hits, `WAVE_HITS` | 2 on 3 victims | 2.5 on 4 | 36-43 on all four bots |
| Guardian, `GUARD_DUTY` | every shot | half | 14-15 a shot, not always with a target |
| `NOVA_HITS` | 2 | 15 | Four casts killed a party |
| Fireball victims | 2 | 1 | 33% alone, barely above the army's 25% |
| `SHOCK_HITS` | 8 | 3 | 42 from 14 a hit |
| `TRAP_EFFICIENCY` | 0.5 | 0.2 | Rogues disarm most traps |
| `SWARM_POWER_FLOOR` (new) | none | 4 | A level 18 army of 64 Carcasses won 1 of 4 |
| Typical player's unlocks | best per Pop | strongest on offer | Reapers against Night Clans, above |

On the final table, R+ from 0.61 to 1.67 all won about 60-100% at full support (R+ 1.21, 0.61, 0.64, 0.72, 1.53, 1.67 on levels 1, 5,
10, 15, 18, 20 against 6, 7, 7, 5, 8, 6 of 8). **That is a loose fit**: maps and unit tier move results by more than R shows. R and R+ can't
forecast one level. Use them to compare, then run raids.

Cast-by-cast sums now fit the single-spell raids on level 10 within a few points, except for Wall, whose stalling the model
doesn't capture.

### Spell data from the coverage stream, not taken in here

Its fixes (next3/coverage, 6d01eb5ab and 6f3300fb5) make Inferno, Wave and Chain really damage the hero clicked on. They did 0 to
that hero before, so the figures here understate those three spells. Its figures at level 10 on a standing hero are Nova centre
777-1365 (before the cut), Shock Wall 1100-1270 (before the cut), Wall 27-46, Inferno 2-20, Wave 0-23, Chain about 190.
Monsters also regenerate about 3 life a second at level 20, which isn't modelled. It also found Slayers weak for their price, Hidden
kinds slow to land a first hit, and Captain (35 gold, rank 2) cheaper than Bones (40 gold, rank 1). None of these three is changed here.

## Limits of the tools

- `trapFires` in the harness's output counts any rise in a trap's cooldown, so a trap placed mid-raid also counts as a firing. The
  harness places traps before the raid, so its figures here are not affected, but read it with care in other tests.
- `TRAP_EFFICIENCY` fits the mean of the traps' measured effect, not each level: they do far more on levels 5 and 10 than on 15 and 20.
- The model's Shock Wall now scales with how long its walls last, fitted to two sets of 6 raids on level 10.

## Measured against calculated

Measured, in raids run by the harness on merged code (2c896dc66 and 25d27ae52, plus 2e6b79be0 for co-op): the tables under
"Measured raids". Co-op: only 3 raids, level 10, full support, two masters both casting, at normal speed on port 6128 (netstat showed no connection to 6112). All 3 won in 98-120s, with 7-10 of the 12 monsters still alive and the Lord barely touched. Solo at the same level won 7 of 8 with about 2 of 14 monsters left. So co-op looks clearly easier, as the doubled monster life and the second master predict, but 3 raids on one level is a hint, not a measurement. Nothing scales the party by player count; that is the obvious next step if play agrees.

Calculated, by the model only: everything below, and all levels other than 1, 5, 10, 15 and 20. Spot checks on levels 3, 8, 12, 18
and 19 were taken on earlier tables, not the final one; level 18 was re-run on the final table.

Run the model with `python scripts/dev/balance_model.py`. It reads the tables out of the C++ and
`monstdat.tsv`, so it always describes the tree it runs in. `--root PATH` points it at another checkout,
`--coop` gives monsters full life, and `--solve-strength FIRST LAST` proposes a strength table.

## The model

The rest of this document is the paper model, re-run on the final constants, with its first-round commentary. Where prose quotes
an R figure or a level, trust the tables and "Measured raids" over the prose.

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
| keeper_campaign.cpp | `CampaignLevels` strength | 100 to 400 | 90 to 205 | See "Round 2 constants" |
| keeper_campaign.cpp | `CampaignLevels` start gold, levels 1 to 5 | 1200 to 1400 | 1800 to 1500 | See "Round 2 constants" |
| keeper_campaign.cpp | `CustomRules` start gold | 1200 | 1800 (with 90% strength) | Same repricing as the campaign's early levels |
| keeper_campaign.cpp | `HeroArmorStrengthPercent` (new) | 100 in effect | 50 | Keeps ranged monsters and traps useful (`ScaleArmorByHeroStrength`, heroes only) |
| keeper_heroes.cpp | `HeroStats`, `HeroMin/MaxDamagePerTenRaids` (new) | Warrior 30+10w life, 2+w to 6+2w damage, 10+3w armour | 33+7w, 3+0.6w to 7+1.2w, 11+2w | Raid 1 is unchanged; growth per raid is about 2/3 |
| keeper_heroes.cpp | same, Rogue | 20+7w, 1+w to 4+2w, 5+2w | 22+5w, 2+0.6w to 5+1.2w, 5+2w | |
| keeper_heroes.cpp | same, Sorcerer | 15+6w, 2+w to 5+2w, 3+w | 17+4w, 3+0.6w to 6+1.2w, 3+w | |
| keeper_heroes.cpp | `LordLifeBase`, `LordLifePerLevel`, `LordArmorBase` (new) | 600 life, 40 armour | 600 + 75 per level, 40 | Both scaled by full strength: see "The Keep Lord" |
| keeper_heroes.cpp | `HeroBountyBase`, `HeroBountyPerWave` (new) | 20 + 5w | 36 + 10w | Runners pay nothing, so kills pay more |
| keeper_bots.cpp | `BotBountyBase`, `BotBountyPerWave` (new) | 30 + 10w | 54 + 18w | Same |
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
| 1 | Old Cellars | 5 | 100 -> 90 | 1200 -> 1800 | 16-32 | 3427 | 3 | 8 | 5 | 5 | 839 | 28 |
| 2 | Rat Warrens | 5 | 105 -> 95 | 1250 -> 1800 | 19-35 | 3516 | 4 | 12 | 9 | 5 | 885 | 29 |
| 3 | Sunken Chapel | 5 | 110 -> 105 | 1300 -> 1700 | 22-38 | 3599 | 5 | 16 | 11 | 5 | 979 | 31 |
| 4 | Bone Pits | 6 | 115 -> 110 | 1350 -> 1600 | 25-45 | 4454 | 6 | 20 | 11 | 6 | 1180 | 41 |
| 5 | Black Vaults | 6 | 125 -> 120 | 1400 -> 1500 | 28-48 | 4616 | 7 | 24 | 13 | 6 | 1290 | 44 |
| 6 | Weeping Halls | 6 | 135 -> 125 | 1500 -> 1500 | 31-51 | 5199 | 8 | 28 | 14 | 7 | 1482 | 51 |
| 7 | Ashen Crypts | 7 | 145 -> 125 | 1600 -> 1600 | 34-58 | 6425 | 9 | 32 | 14 | 7 | 1600 | 59 |
| 8 | Iron Gaol | 7 | 155 -> 125 | 1700 -> 1700 | 37-61 | 6657 | 10 | 36 | 15 | 8 | 1706 | 68 |
| 9 | Maggot Deeps | 7 | 165 -> 125 | 1800 -> 1800 | 40-64 | 6757 | 11 | 40 | 16 | 8 | 1706 | 68 |
| 10 | Hollow Court | 8 | 180 -> 125 | 1950 -> 1950 | 43-71 | 8381 | 12 | 44 | 17 | 9 | 2296 | 83 |
| 11 | Blood Galleries | 8 | 195 -> 130 | 2100 -> 2100 | 46-74 | 8780 | 13 | 48 | 17 | 9 | 2385 | 87 |
| 12 | Shrouded Nave | 8 | 210 -> 135 | 2250 -> 2250 | 49-77 | 9673 | 14 | 52 | 18 | 9 | 2480 | 89 |
| 13 | Gallows Reach | 8 | 225 -> 140 | 2400 -> 2400 | 52-80 | 10266 | 15 | 56 | 18 | 10 | 2758 | 101 |
| 14 | Cinder Keep | 9 | 240 -> 140 | 2600 -> 2600 | 55-87 | 12484 | 16 | 60 | 19 | 11 | 3091 | 122 |
| 15 | Howling Stair | 9 | 260 -> 145 | 2800 -> 2800 | 58-90 | 13031 | 17 | 64 | 20 | 11 | 3200 | 126 |
| 16 | Tomb of Kings | 9 | 280 -> 145 | 3000 -> 3000 | 61-93 | 13935 | 18 | 68 | 20 | 12 | 3314 | 136 |
| 17 | Silent Abbey | 9 | 300 -> 150 | 3200 -> 3200 | 64-96 | 14576 | 19 | 72 | 20 | 12 | 3431 | 143 |
| 18 | Chains of Night | 10 | 325 -> 165 | 3500 -> 3500 | 67-103 | 18839 | 20 | 76 | 21 | 13 | 4281 | 185 |
| 19 | Burning Sepulcre | 10 | 350 -> 180 | 3800 -> 3800 | 70-106 | 20535 | 21 | 80 | 21 | 13 | 4670 | 199 |
| 20 | Throne of Ash | 10 | 400 -> 205 | 4200 -> 4200 | 73-109 | 25389 | 22 | 84 | 21 | 14 | 5535 | 235 |

"Gold earned + start" counts the expected bounties, with none of it spent, so it is a ceiling.

## Combat: last raid of each level

Army names show what the model's player would field, not a recommendation.

| Lvl/raid | Life before | Life after | Best army before | Best army after | R before | R after | Swarm before | R+ before | Swarm after | R+ after | R+ co-op | Lord: party / quarter / would kill in |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1/5 | 1528 | 839 | 5 Brute | 8 Brute | 0.12 | 0.75 | 16 Bones: 0.01 | 0.36 | 32 Fallen: 0.06 | 1.29 | 2.37 | 31s / 95s / 60s |
| 2/5 | 1599 | 885 | 5 Brute | 8 Brute | 0.10 | 0.56 | 17 Bones: 0.01 | 0.50 | 35 Fallen: 0.06 | 1.18 | 2.37 | 35s / 112s / 61s |
| 3/5 | 1702 | 979 | 5 Brute + 2 Horror Axe | 4 Black Death + 6 Brute | 0.08 | 0.53 | 18 Bones: 0.01 | 0.40 | 38 Fallen: 0.05 | 1.25 | 2.22 | 35s / 107s / 61s |
| 4/6 | 2392 | 1180 | 6 Winged + 2 Brute | 7 Brute + 9 Carcass | 0.07 | 0.36 | 21 Bones: 0.00 | 0.33 | 45 Fallen: 0.04 | 0.81 | 1.57 | 36s / 151s / 74s |
| 5/6 | 2646 | 1290 | 8 Winged + 1 Flesh Clan | 9 Brute + 4 Carcass | 0.05 | 0.31 | 22 Bones: 0.00 | 0.26 | 48 Fallen: 0.04 | 0.70 | 1.37 | 41s / 179s / 77s |
| 6/6 | 3119 | 1482 | 5 Stone Clan + 9 Flesh Clan | 9 Stone Clan + 4 Brute | 0.04 | 0.29 | 23 Bones: 0.00 | 0.19 | 51 Fallen: 0.03 | 0.72 | 1.31 | 39s / 143s / 81s |
| 7/7 | 3755 | 1600 | 2 Stone Clan + 18 Flesh Clan | 18 Stone Clan | 0.04 | 0.38 | 26 Bones: 0.00 | 0.17 | 58 Fallen: 0.03 | 1.13 | 1.90 | 35s / 142s / 95s |
| 8/7 | 4225 | 1706 | 4 Stone Clan + 7 Mud Man | 12 Gargoyle | 0.04 | 0.34 | 27 Bones: 0.00 | 0.17 | 61 Fallen: 0.03 | 0.94 | 1.65 | 33s / 158s / 101s |
| 9/7 | 4594 | 1706 | 2 Toad Demon + 14 Stone Clan | 9 Gargoyle + 4 Brute | 0.04 | 0.32 | 28 Bones: 0.00 | 0.15 | 64 Fallen: 0.03 | 1.03 | 1.91 | 35s / 168s / 103s |
| 10/8 | 6543 | 2296 | 8 Toad Demon + 7 Stone Clan | 10 Toad Demon + 1 Flesh Clan | 0.03 | 0.33 | 31 Bones: 0.00 | 0.09 | 71 Fallen: 0.03 | 0.76 | 1.52 | 30s / 123s / 151s |
| 11/8 | 7284 | 2385 | 11 Toad Demon + 1 Mud Man | 11 Toad Demon | 0.03 | 0.34 | 32 Bones: 0.00 | 0.07 | 74 Fallen: 0.03 | 0.77 | 1.54 | 32s / 133s / 159s |
| 12/8 | 8134 | 2480 | 12 Toad Demon + 2 Mud Man | 11 Blood Claw + 1 Mud Man | 0.02 | 0.48 | 33 Bones: 0.00 | 0.05 | 77 Fallen: 0.03 | 1.11 | 2.03 | 35s / 145s / 163s |
| 13/8 | 9328 | 2758 | 15 Toad Demon | 12 Blood Claw + 1 Winged | 0.02 | 0.41 | 34 Bones: 0.00 | 0.04 | 80 Fallen: 0.03 | 0.94 | 1.68 | 34s / 158s / 179s |
| 14/9 | 11418 | 3091 | 18 Toad Demon + 1 Winged | 13 Blood Claw + 4 Mud Man | 0.02 | 0.37 | 37 Bones: 0.00 | 0.04 | 87 Fallen: 0.03 | 0.85 | 1.65 | 29s / 150s / 225s |
| 15/9 | 12817 | 3200 | 18 Toad Demon + 2 Winged | 11 Blood Claw + 6 Toad Demon | 0.02 | 0.36 | 38 Bones: 0.00 | 0.03 | 90 Fallen: 0.03 | 0.86 | 1.66 | 31s / 164s / 236s |
| 16/9 | 14575 | 3314 | 10 Toad Demon + 19 Winged | 10 Blood Claw + 8 Toad Demon | 0.01 | 0.36 | 39 Bones: 0.00 | 0.02 | 93 Fallen: 0.03 | 0.97 | 1.85 | 31s / 172s / 220s |
| 17/9 | 16139 | 3431 | 6 Reaper + 3 Toad Demon | 17 Blood Claw + 2 Toad Demon | 0.03 | 0.33 | 40 Bones: 0.00 | 0.05 | 96 Fallen: 0.03 | 1.10 | 2.03 | 33s / 183s / 229s |
| 18/10 | 22439 | 4281 | 7 Reaper + 2 Toad Demon | 10 Crypt Demon + 2 Mud Man | 0.02 | 0.66 | 43 Bones: 0.00 | 0.04 | 96 Fallen: 0.03 | 1.84 | 3.41 | 34s / 138s / 314s |
| 19/10 | 25114 | 4670 | 8 Reaper | 11 Crypt Demon + 2 Mud Man | 0.02 | 0.52 | 44 Bones: 0.00 | 0.03 | 96 Fallen: 0.02 | 1.56 | 3.12 | 39s / 152s / 311s |
| 20/10 | 31426 | 5535 | 7 Reaper + 3 Toad Demon | 15 Crypt Demon | 0.01 | 0.71 | 45 Bones: 0.00 | 0.02 | 96 Fallen: 0.02 | 1.98 | 3.64 | 46s / 326s / 320s |

The Lord's column reads "lasts against the
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
| New keeper | 1 | 1800 | 6 | 1039 | 12 Brute | 0.82 | 1.46 | 20s |
| After level 5 | 7 | 1800 | 6 | 1039 | 5 Flesh Clan + 9 Brute | 0.82 | 1.56 | 20s |
| After level 10 | 12 | 1800 | 6 | 1039 | 2 Gargoyle + 5 Toad Demon | 1.09 | 2.28 | 20s |
| After level 20 | 22 | 1800 | 6 | 1039 | 2 Crypt Demon + 1 Black Knight | 1.93 | 4.59 | 20s |

A new keeper can hold, and a veteran's unlocks make it easy. That follows from custom maps having a single
set of rules.

A level's first raid is far easier than its last:

| Lvl/raid | Heroes | Party life | Army | R | R+ |
|---|---|---|---|---|---|
| 1/1 | 2 | 135 | 4 Brute | 14.52 | 72.60 |
| 5/1 | 2 | 181 | 1 Carcass + 4 Brute | 6.16 | 30.79 |
| 10/1 | 3 | 236 | 1 Gargoyle + 2 Toad Demon | 2.33 | 11.66 |
| 15/1 | 4 | 407 | 1 Gargoyle + 3 Toad Demon | 1.31 | 6.55 |
| 20/1 | 6 | 655 | 2 Crypt Demon + 1 Blood Claw | 2.30 | 11.52 |

So most of each level is a build-up, and the difficulty is in its last two or three raids. That follows
from heroes growing with the raid number and is not new, though the gap is smaller than it was.

## XP, Renown and unlocks

| Lvl | KL after: before | KL after: now | Renown: before | Renown: now | Kinds eligible: before | Kinds eligible: now | Renown for all eligible (now) | Kinds owned (now) | Key spells (now) |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 3 | 3 | 6 | 8 | 17 | 17 | 12 | 5 |  |
| 5 | 8 | 7 | 21 | 24 | 43 | 39 | 56 | 13 | Nova 4, Fire 1 |
| 10 | 13 | 12 | 36 | 44 | 61 | 59 | 119 | 17 | Nova 4, Fire 3, Chai 1 |
| 15 | 19 | 17 | 54 | 64 | 104 | 91 | 263 | 20 | Nova 4, Fire 6, Chai 1 |
| 20 | 24 | 22 | 69 | 84 | 111 | 111 | 370 | 21 | Nova 4, Fire 7, Chai 1, Apoc 1 |

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
| 1 | 1800 | 584 | 1043 | 3427 | 16 | 32 | 880 | 1760 | 5 |
| 5 | 1500 | 1050 | 2066 | 4616 | 28 | 48 | 960 | 2300 | 5 |
| 10 | 1950 | 1747 | 4684 | 8381 | 43 | 71 | 1125 | 4335 | 7 |
| 15 | 2800 | 2462 | 7769 | 13031 | 58 | 90 | 1545 | 7250 | 9 |
| 20 | 4200 | 4148 | 17041 | 25389 | 73 | 109 | 2460 | 15225 | 12 |

Before:

| Lvl | Start | Tribute | Bounties | Total | Pop first | Pop last | Army gold first | Army gold last | Upgrade tiers |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1200 | 650 | 644 | 2494 | 16 | 32 | 520 | 1300 | 3 |
| 5 | 1400 | 1092 | 1183 | 3675 | 24 | 44 | 780 | 1855 | 5 |
| 10 | 1950 | 2520 | 3677 | 8147 | 34 | 62 | 1110 | 4655 | 9 |
| 15 | 2800 | 4420 | 7553 | 14773 | 44 | 76 | 1680 | 7990 | 14 |
| 20 | 4200 | 8100 | 17852 | 30152 | 54 | 90 | 2390 | 9625 | 22 |

- **Binding limit:** gold, on every raid of every level in the model. Before, Pop bound last raids from level 14,
and gold piled up unspent. Pop is now close enough to matter when a player saves well.
- **Cheap swarm:** not dominant. A full keep of the cheapest kind scores R 0.03 to 0.08 on last raids and
  under 1 on most first raids. Pop-1 mid-tier kinds such as Carcass are good until about level 5, which
  seems fine.

## Heroes: bots against monster heroes

Now:

| Lvl/raid | Class | Bot life | Potions | Bot clvl | Bot dmg | Bot AC | Monster life | Monster dmg | Monster AC |
|---|---|---|---|---|---|---|---|---|---|
| 1/1 | W | 63 | 1 | 1 | 4 | 7 | 36 | 4 | 12 |
| 1/1 | R | 40 | 1 | 1 | 2 | 6 | 24 | 3 | 6 |
| 1/1 | S | 27 | 1 | 1 | 10 | 3 | 18 | 4 | 3 |
| 1/5 | W | 137 | 2 | 9 | 8 | 8 | 91 | 8 | 19 |
| 1/5 | R | 92 | 2 | 9 | 4 | 7 | 63 | 6 | 14 |
| 1/5 | S | 62 | 2 | 9 | 14 | 4 | 49 | 8 | 7 |
| 5/6 | W | 198 | 2 | 15 | 12 | 9 | 134 | 12 | 25 |
| 5/6 | R | 134 | 2 | 15 | 8 | 8 | 93 | 10 | 18 |
| 5/6 | S | 90 | 2 | 15 | 14 | 5 | 73 | 11 | 9 |
| 10/8 | W | 236 | 3 | 24 | 19 | 10 | 166 | 14 | 30 |
| 10/8 | R | 162 | 3 | 24 | 12 | 9 | 116 | 12 | 23 |
| 10/8 | S | 108 | 3 | 24 | 16 | 6 | 91 | 13 | 12 |
| 15/9 | W | 291 | 3 | 31 | 26 | 11 | 208 | 18 | 35 |
| 15/9 | R | 201 | 3 | 31 | 16 | 10 | 145 | 16 | 28 |
| 15/9 | S | 134 | 3 | 31 | 18 | 7 | 114 | 17 | 14 |
| 20/1 | W | 143 | 1 | 20 | 12 | 7 | 82 | 11 | 19 |
| 20/1 | R | 92 | 1 | 20 | 8 | 6 | 55 | 8 | 10 |
| 20/1 | S | 61 | 1 | 20 | 10 | 3 | 43 | 10 | 6 |
| 20/10 | W | 436 | 3 | 38 | 40 | 14 | 315 | 28 | 47 |
| 20/10 | R | 303 | 3 | 38 | 26 | 13 | 221 | 25 | 38 |
| 20/10 | S | 202 | 3 | 38 | 20 | 10 | 174 | 27 | 19 |

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
| 1/5 | 19 (67%) | 14 (72%) | 7 (79%) | 65% | 35% |
| 5/6 | 25 (61%) | 18 (68%) | 9 (77%) | 82% | 52% |
| 10/8 | 30 (56%) | 23 (63%) | 12 (74%) | 100% | 75% |
| 15/9 | 35 (51%) | 28 (58%) | 14 (72%) | 100% | 95% |
| 20/10 | 47 (39%) | 38 (48%) | 19 (67%) | 100% | 100% |

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
| 20/10 | 95% / 95% | 95% / 95% | 86% / 95% | 56% / 86% |

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
| 5 | 9 | Nova | 4 | 7 | 31.2 | 6.0 | 144 | 123 | 3.9 | 10% |
| 5 | 9 | Nova | 15 | 13 | 17.5 | 6.0 | 276 | 235 | 13.4 | 18% |
| 5 | 9 | Wall | 1 | 4 | 25.0 | 4.0 | 25 | 21 | 0.9 | 2% |
| 5 | 9 | Wall | 15 | 11 | 12.5 | 4.0 | 25 | 21 | 1.7 | 2% |
| 5 | 9 | Guard | 1 | 4 | 30.0 | 8.0 | 60 | 26 | 0.9 | 2% |
| 5 | 9 | Guard | 15 | 11 | 15.0 | 8.0 | 158 | 67 | 4.5 | 5% |
| 5 | 9 | Stone | 1 | 4 | 20.0 | 5.0 | 0 | 0 | 0.0 | 0% |
| 5 | 9 | Stone | 15 | 11 | 10.0 | 5.0 | 0 | 0 | 0.0 | 0% |
| 5 | 9 | Inferno | 1 | 2 | 15.0 | 2.0 | 13 | 6 | 0.4 | 0% |
| 5 | 9 | Inferno | 15 | 9 | 7.5 | 2.0 | 13 | 6 | 0.8 | 0% |
| 5 | 9 | Fireball | 1 | 3 | 20.0 | 3.0 | 57 | 24 | 1.2 | 2% |
| 5 | 9 | Fireball | 15 | 10 | 10.0 | 3.0 | 130 | 55 | 5.5 | 4% |
| 10 | 19 | Nova | 4 | 7 | 31.2 | 6.0 | 199 | 176 | 5.6 | 8% |
| 10 | 19 | Nova | 15 | 13 | 17.5 | 6.0 | 380 | 338 | 19.3 | 15% |
| 10 | 19 | Wall | 1 | 4 | 25.0 | 4.0 | 38 | 33 | 1.3 | 1% |
| 10 | 19 | Wall | 15 | 11 | 12.5 | 4.0 | 38 | 33 | 2.7 | 1% |
| 10 | 19 | Guard | 1 | 4 | 30.0 | 8.0 | 98 | 43 | 1.4 | 2% |
| 10 | 19 | Guard | 15 | 11 | 15.0 | 8.0 | 210 | 93 | 6.2 | 4% |
| 10 | 19 | Stone | 1 | 4 | 20.0 | 5.0 | 0 | 0 | 0.0 | 0% |
| 10 | 19 | Stone | 15 | 11 | 10.0 | 5.0 | 0 | 0 | 0.0 | 0% |
| 10 | 19 | Inferno | 1 | 2 | 15.0 | 2.0 | 24 | 11 | 0.7 | 0% |
| 10 | 19 | Inferno | 15 | 9 | 7.5 | 2.0 | 24 | 11 | 1.4 | 0% |
| 10 | 19 | Fireball | 3 | 4 | 18.6 | 3.0 | 96 | 43 | 2.3 | 2% |
| 10 | 19 | Fireball | 15 | 10 | 10.0 | 3.0 | 195 | 87 | 8.7 | 4% |
| 10 | 19 | Chain | 1 | 3 | 30.0 | 5.0 | 86 | 153 | 5.1 | 7% |
| 10 | 19 | Chain | 15 | 10 | 15.0 | 5.0 | 126 | 225 | 15.0 | 10% |
| 10 | 19 | Shock Wall | 1 | 0 | 40.0 | 5.0 | 45 | 60 | 1.5 | 3% |
| 10 | 19 | Shock Wall | 15 | 7 | 20.0 | 5.0 | 180 | 240 | 12.0 | 10% |
| 10 | 19 | Wave | 1 | 3 | 25.0 | 4.0 | 61 | 109 | 4.4 | 5% |
| 10 | 19 | Wave | 15 | 10 | 12.5 | 4.0 | 61 | 109 | 8.7 | 5% |
| 20 | 39 | Nova | 4 | 7 | 31.2 | 6.0 | 307 | 281 | 9.0 | 5% |
| 20 | 39 | Nova | 15 | 13 | 17.5 | 6.0 | 588 | 537 | 30.7 | 10% |
| 20 | 39 | Wall | 1 | 4 | 25.0 | 4.0 | 62 | 57 | 2.3 | 1% |
| 20 | 39 | Wall | 15 | 11 | 12.5 | 4.0 | 62 | 57 | 4.6 | 1% |
| 20 | 39 | Guard | 1 | 4 | 30.0 | 8.0 | 172 | 79 | 2.6 | 1% |
| 20 | 39 | Guard | 15 | 11 | 15.0 | 8.0 | 315 | 144 | 9.6 | 3% |
| 20 | 39 | Stone | 1 | 4 | 20.0 | 5.0 | 0 | 0 | 0.0 | 0% |
| 20 | 39 | Stone | 15 | 11 | 10.0 | 5.0 | 0 | 0 | 0.0 | 0% |
| 20 | 39 | Inferno | 1 | 2 | 15.0 | 2.0 | 44 | 20 | 1.4 | 0% |
| 20 | 39 | Inferno | 15 | 9 | 7.5 | 2.0 | 44 | 20 | 2.7 | 0% |
| 20 | 39 | Fireball | 7 | 6 | 15.7 | 3.0 | 203 | 93 | 5.9 | 2% |
| 20 | 39 | Fireball | 15 | 10 | 10.0 | 3.0 | 325 | 148 | 14.8 | 3% |
| 20 | 39 | Chain | 1 | 3 | 30.0 | 5.0 | 161 | 295 | 9.8 | 5% |
| 20 | 39 | Chain | 15 | 10 | 15.0 | 5.0 | 236 | 432 | 28.8 | 8% |
| 20 | 39 | Shock Wall | 1 | 0 | 40.0 | 5.0 | 75 | 103 | 2.6 | 2% |
| 20 | 39 | Shock Wall | 15 | 7 | 20.0 | 5.0 | 300 | 411 | 20.6 | 7% |
| 20 | 39 | Wave | 1 | 3 | 25.0 | 4.0 | 111 | 203 | 8.1 | 4% |
| 20 | 39 | Wave | 15 | 10 | 12.5 | 4.0 | 111 | 203 | 16.3 | 4% |
| 20 | 39 | Ring | 1 | 3 | 40.0 | 8.0 | 62 | 86 | 2.1 | 2% |
| 20 | 39 | Ring | 15 | 10 | 20.0 | 8.0 | 62 | 86 | 4.3 | 2% |
| 20 | 39 | Apocalypse | 1 | 1 | 80.0 | 25.0 | 136 | 499 | 6.2 | 9% |
| 20 | 39 | Apocalypse | 15 | 8 | 40.0 | 25.0 | 289 | 1058 | 26.5 | 19% |

Before, at level 20:

| Lvl | Master clvl | Spell | Raised to | Engine lvl | Mana | Cooldown s | Dmg/victim | Expected total | Per mana | % of party life |
|---|---|---|---|---|---|---|---|---|---|---|
| 20 | 39 | Nova | 3 | 8 | 31 | 6.0 | 1087 | 994 | 32.1 | 3% |
| 20 | 39 | Nova | 15 | 20 | 17 | 6.0 | 4468 | 4085 | 240.3 | 13% |
| 20 | 39 | Wall | 1 | 4 | 25 | 4.0 | 62 | 57 | 2.3 | 0% |
| 20 | 39 | Wall | 15 | 18 | 12 | 4.0 | 62 | 57 | 4.8 | 0% |
| 20 | 39 | Guard | 1 | 4 | 30 | 8.0 | 75 | 34 | 1.1 | 0% |
| 20 | 39 | Guard | 15 | 18 | 15 | 8.0 | 98 | 45 | 3.0 | 0% |
| 20 | 39 | Stone | 1 | 4 | 20 | 5.0 | 0 | 0 | 0.0 | 0% |
| 20 | 39 | Stone | 15 | 18 | 10 | 5.0 | 0 | 0 | 0.0 | 0% |
| 20 | 39 | Inferno | 1 | 2 | 15 | 2.0 | 44 | 20 | 1.4 | 0% |
| 20 | 39 | Inferno | 15 | 16 | 7 | 2.0 | 44 | 20 | 2.9 | 0% |
| 20 | 39 | Fireball | 5 | 7 | 16 | 3.0 | 228 | 104 | 6.5 | 0% |
| 20 | 39 | Fireball | 15 | 17 | 10 | 3.0 | 741 | 339 | 33.9 | 1% |
| 20 | 39 | Chain | 1 | 3 | 30 | 5.0 | 161 | 295 | 9.8 | 1% |
| 20 | 39 | Chain | 15 | 17 | 15 | 5.0 | 312 | 570 | 38.0 | 2% |
| 20 | 39 | Shock Wall | 1 | 3 | 30 | 5.0 | 300 | 411 | 13.7 | 1% |
| 20 | 39 | Shock Wall | 15 | 17 | 15 | 5.0 | 300 | 411 | 27.4 | 1% |
| 20 | 39 | Wave | 1 | 3 | 35 | 6.0 | 111 | 203 | 5.8 | 1% |
| 20 | 39 | Wave | 15 | 17 | 17 | 6.0 | 111 | 203 | 12.0 | 1% |
| 20 | 39 | Ring | 1 | 3 | 40 | 8.0 | 62 | 86 | 2.1 | 0% |
| 20 | 39 | Ring | 15 | 17 | 20 | 8.0 | 62 | 86 | 4.3 | 0% |
| 20 | 39 | Apocalypse | 3 | 3 | 62 | 20.0 | 191 | 699 | 11.3 | 2% |
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
| 1 | Fire Trap | 1 | 100 | 40 | 8.0 | 70% / 75% | 4 |
| 1 | Arrow Trap | 1 | 75 | 14 | 3.0 | 70% / 75% | 4 |
| 1 | Shock Trap | 1 | 140 | 12 | 6.0 | 70% / 75% | 2 |
| 5 | Fire Trap | 2 | 100 | 69 | 7.6 | 64% / 71% | 6 |
| 5 | Arrow Trap | 2 | 75 | 24 | 2.85 | 64% / 71% | 6 |
| 5 | Shock Trap | 1 | 140 | 19 | 6.0 | 64% / 71% | 2 |
| 10 | Fire Trap | 3 | 100 | 97 | 7.2 | 59% / 66% | 9 |
| 10 | Arrow Trap | 3 | 75 | 35 | 2.7 | 59% / 66% | 9 |
| 10 | Shock Trap | 1 | 140 | 25 | 6.0 | 59% / 66% | 3 |
| 15 | Fire Trap | 3 | 100 | 122 | 7.2 | 54% / 61% | 10 |
| 15 | Arrow Trap | 4 | 75 | 49 | 2.55 | 54% / 61% | 12 |
| 15 | Shock Trap | 1 | 140 | 31 | 6.0 | 54% / 61% | 3 |
| 20 | Fire Trap | 4 | 100 | 207 | 6.8 | 42% / 51% | 16 |
| 20 | Arrow Trap | 4 | 75 | 76 | 2.55 | 42% / 51% | 15 |
| 20 | Shock Trap | 1 | 140 | 49 | 6.0 | 42% / 51% | 4 |

The last column is the damage rate while a Rogue stands beside the trap. The Shock figure leaves out its
repeat hits, about 8 in the model. The Fire Trap's burning wall isn't counted, though heroes avoid it.

## Upgrades

Now:

| Lvl | Gold before last raid | All upgrades cost | % of gold earned in the level | Tiers bought by last raid | Tiers on offer |
|---|---|---|---|---|---|
| 1 | 3028 | 8737 | 255% | 5 | 29 |
| 5 | 3901 | 11652 | 252% | 5 | 29 |
| 10 | 7039 | 12127 | 145% | 7 | 29 |
| 15 | 10975 | 14069 | 108% | 9 | 29 |
| 20 | 21412 | 19895 | 78% | 12 | 29 |

Before:

| Lvl | Gold before last raid | All upgrades cost | % of gold earned in the level | Tiers bought by last raid | Tiers on offer |
|---|---|---|---|---|---|
| 1 | 2249 | 9710 | 389% | 3 | 29 |
| 5 | 3271 | 9710 | 264% | 5 | 29 |
| 10 | 7112 | 9710 | 119% | 9 | 29 |
| 15 | 12810 | 9710 | 66% | 14 | 29 |
| 20 | 26052 | 9710 | 32% | 22 | 29 |

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
