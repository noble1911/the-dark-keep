# The Dark Keep — Mod Plan & Roadmap

A **dungeon-master / tower-defence** total conversion of the classic Diablo engine,
built on DevilutionX. You don't play the hero — you *are* the dungeon. Raiding parties
of AI heroes come down the stairs to steal your treasure and escape; you and up to three
co-op allies build the defences, command the monsters, and smite the intruders.

Requires the player's own original game data; the project ships no Blizzard assets.
Binary/project names stay `devilutionx` in the build tree so upstream merges stay clean;
branding is presentation-layer only (same convention as The Red Portal).

Last updated: **2026-11-01** (project start).

## Decisions (2026-11-01)

| Question | Decision |
|---|---|
| Core mode | **Co-op masters vs AI heroes** (1–4 players share one dungeon). Human-hero "DM vs party" mode is a later extension. |
| Pacing | **Tower-defence**: a long, generous setup phase before the first raid and short breathers between waves; building *is* allowed during a raid (see Build rules). |
| Hero goal | **Steal and escape**: heroes reach the treasure vault, grab gold, and run for the exit. Kill them before they leave to get it back. |
| Scope | **One level** (Cathedral 1, fixed seed) until the mechanics are fun. |
| Platform | Windows first. Switch later, if the cursor-heavy controls can be mapped well. |

## Design pillars

1. **Prepare, watch it pay off, intervene at the climax.** The joy of tower defence and
   the dungeon-master genre: a plan you built crushing a party, plus a well-timed spell
   when it doesn't.
2. **Readable but surprising heroes.** Scouting shows the next party, so counter-planning
   matters, but hero fear/greed and class roles keep raids from becoming routine
   (the "Keep the Heroes Out!" trap: AI that's solved after three games).
3. **Light management.** No worker micromanagement, no hunger/pay chores. Every choice is
   *where* and *what* to place or cast.
4. **Co-op from day one.** Every master action is a network command; nothing mutates game
   state locally first.
5. **It's still Diablo.** Reuse monsters, missiles/spells, traps, doors, objects and the
   original level generator rather than inventing new content.

## The loop (one level)

```
 Setup (long)  ->  Scout  ->  RAID  ->  Breather  ->  Scout  ->  RAID ... -> Champion raid
```

1. **Setup phase** — before wave 1, a long timer (≈3–4 min, skippable with "Ready" from
   all masters). Starting Gold is spent on lairs, traps, doors and bait.
2. **Scout** — the next party's card: classes, levels, resistances, greed/courage.
3. **Raid** — heroes enter from the up-stairs. Monsters fight with stock Diablo AI.
   Masters cast spells with Mana and can keep building (at a cost, see Build rules).
4. **Breather** — ≈60–90 s between waves (skippable) to rebuild and re-plan.
5. **Win** after surviving all waves (target ≈7, the last a named champion party).
   **Lose** if the heroes kill the **Keep Lord** (boss at the vault) *and* one of them
   escapes through the Town Portal they then channel (8 s; killing the caster interrupts
   it). A wiped party means the Lord rises again. (Changed 2026-11-01: emptying the
   vault no longer loses -- it made spending everything a dominant strategy.)

### Steal and escape
- The **Treasure Vault** sits at the far end of the level (near the down-stairs area).
  It holds the keep's Gold — which is also the masters' building currency, so spending
  is a risk trade-off: gold spent on defences can't be stolen, gold in the vault is
  what you're defending.
- A hero who reaches the vault loots a chunk (carry cap by class) and switches to
  **escape mode**, heading back to the up-stairs.
- Killing a carrier drops the stolen gold where they fall — recover it (it returns to the
  vault after a short time, or when a monster/master collects it).
- Escaped gold is lost for good. Thieves escaping is the main pressure, not hero kills.

### Build rules (tower-defence pacing)
- **Setup/breather**: full price, instant placement.
- **During a raid**: allowed, but placements take a few seconds to "materialise"
  (a summoning circle the heroes can see and interrupt) and cannot be placed within
  heroes' line of sight / a radius of a hero. Keeps mid-raid building a tactic, not a
  replacement for preparation.
- Selling/removing refunds part of the cost (full refund during setup).

## Mechanics

### Resources
| Resource | Source | Spent on |
|---|---|---|
| **Gold** (shared) | Vault stock, dead heroes' loot, recovered thief gold | Lairs, traps, doors, bait, upgrades |
| **Mana** (per master) | Regenerates; faster when heroes are near your monsters (fear feeds you) | Spells |
| **Souls** (meta, later) | Hero kills | Unlocks between runs (new monsters/spells) |

### Placeables
- **Lairs** — bound to a room; spawn and respawn a monster group of a chosen type
  (Zombies, Fallen, Skeletons, Scavengers... later deeper-tier types). Monsters guard
  their room unless rallied.
- **Traps** — reuse Diablo trap objects: arrow/fire wall traps, trapped chests,
  pressure plates. Rogues can detect/disarm.
- **Doors** — lock/unlock or reinforce existing level doors; heroes must bash locked
  doors (time + noise).
- **Bait** — fake chests/item piles that pull greedy heroes off the direct path.

### Master powers (cursor spells)
Reuse the missile system: Fireball, Lightning/Chain, Firewall, Stone Curse, Golem,
Inferno, Teleport (monster), Darkness (snuff light, raises hero fear). Each has Mana
cost + cooldown.

### Monster control (indirect)
- Rally flag: monsters from a chosen lair move to a point.
- Stance per lair: Guard room / Aggressive (chase anywhere) / Ambush (hold still until a
  hero is close).
- No per-unit orders.

### Heroes (AI)
- Classes mirror the original three: **Warrior** (front line, door basher, high carry),
  **Rogue** (ranged, detects + disarms traps, fast thief), **Sorcerer** (spells,
  fragile, breaks Stone Curse/Golems).
- Goals in order: explore → loot chests/bait → reach vault → escape.
- **Fear/courage**: frightened heroes (low HP, dead allies, darkness, overwhelming
  numbers) retreat — dropping carried loot. Greedy heroes take detours to bait.
- Parties scale per wave in count, level, and gear.

### Possession (stretch)
A master can possess one of their monsters and control it directly with normal
Diablo click-to-move/attack controls (Mana drain per second).

### Co-op
- 2–4 masters, one dungeon, one vault, one shared Gold pool, separate Mana.
- Ping marker ("heroes here!", "build here").
- Any master can build/cast; no ownership of individual lairs (simplest, revisit later).

## Technical approach & risks

1. **Bodyless player.** The master is a `Player` with no in-world hero: free-scroll
   camera, cursor, custom HUD (Gold/Mana/wave/build palette). Touches camera
   (`ViewPosition` follows the hero today), input, HUD panels, and anything that
   assumes `MyPlayer` stands on a tile. Plan: keep the player entity but park it
   off-map / invulnerable / invisible, decouple the camera.
2. **Hero AI implementation — spike decision.**
   - (a) Heroes as **monsters** using player-class sprites + new AI routines. Rides on the
     existing monster sync (`Source/sync.cpp` deltas). **Preferred first.**
   - (b) Heroes as **bot players** — richer (potions, items), but player entities are
     owned by one client; much harder netcode. Revisit only if (a) feels too thin.
3. **Co-op sync.** DevilutionX syncs monsters by deltas, not host authority. All master
   actions (place, sell, cast, rally, phase changes) become new `CMD_*` messages from
   day one. Wave timer/spawns derived from a shared seed + game tick. A designated
   "keeper host" (lowest player id) owns the wave director so spawns happen once.
4. **Fixed level.** Cathedral 1 with a fixed seed so every client generates the same
   map; vault/entry positions derived from stairs locations.

## Milestones

| Milestone | Status |
|---|---|
| M0 — fork, repo, Windows build, run against game data | ✅ done (2026-11-01) |
| M1 — Master view: no hero, free camera, HUD, fixed-seed Cathedral 1 | ✅ done (2026-11-01) |
| M2 — Place lairs + cast spells at cursor, all via net commands | ✅ done (2026-11-01) |
| M3 — Hero AI spike: one party walks to the vault, fights, loots, escapes | ✅ done (2026-11-01) — heroes are monsters wearing player-class sprites (option a) |
| M4 — Raid loop: setup timer, scout card, waves, breathers, win/lose | ✅ done (2026-11-01) |
| M5 — Traps, doors, bait, hero fear; mid-raid build rules | ✅ done (2026-11-01) |
| M6 — 2-client co-op test | ✅ done (2026-11-01) — host + client over TCP on one machine: builds from either master, raids, theft, tribute and phases stay in step |
| M7 — Possession | ✅ done (2026-11-01) — E over a keep monster: click to move/attack heroes, 10 mana + 2/s, E/Esc releases (it then guards where it stands) |
| M8 — Keep Lord + Town Portal win condition | ✅ done (2026-11-01) — boss guards the vault; heroes channel a portal after killing it; escape = defeat |
| M9 — Real player bots as heroes (4 bot slots after the 4 network slots) | 🚧 in progress |

## Co-op notes (M6, 2026-11-01)

- Dev flags: `--keeper-autostart` (single player), `--keeper-host`, `--keeper-join <ip>`
  (TCP, skips all menus with a throwaway "Keeper" hero). Two local instances need
  separate `--config-dir`/`--save-dir`.
- Windows shows a firewall prompt the first time the exe opens a TCP port; localhost
  works without allowing it, LAN play needs it allowed.
- Known gaps: a master joining mid-game does not receive existing lairs/traps/locks/bait
  (they are event messages, not part of the snapshot); hero AI state (carried gold,
  fear) is simulated per client and only the vault/phase are authoritative, so rare
  cosmetic disagreements (e.g. message order) are possible.

## Research notes (2026-11-01)

- *Dungeon Keeper*: villain inversion and humour are the hook; monsters largely
  self-directed; designers prototyped for fun first.
- *Dungeon Keeper 2 / Dungeons*: too many management jobs became "not so much fun" —
  keep chores minimal.
- *Legend of Keepers*: scout the party's stats/resistances, then counter-plan — the
  core decision moment.
- *Keep the Heroes Out!*: co-op monsters vs AI heroes; reviewers found hero priorities
  easy to solve — hero AI needs variety.
- *Dungeons 3*: 2-player co-op sharing one dungeon, units and resources.
- *Descent / Greed*: overlord role must have constant meaningful decisions.
