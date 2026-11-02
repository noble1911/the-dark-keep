#!/usr/bin/env python3
"""A paper model of The Dark Keep's campaign. See docs/BALANCE.md.

Reads the balance tables and constants straight out of the C++ and the monster data, so a run always
describes the source tree it is pointed at:

    python scripts/dev/balance_model.py [--root PATH] [--coop] [--section NAME ...]

It models a "typical player" who clears every level first time, buys steadily and fights raids in a
Lanchester square-law sense. It says nothing about pathing, AI or how good a real player is: use the
numbers to compare levels with each other, not as a forecast of any one fight.
"""
import argparse
import copy
import csv
import math
import re
from pathlib import Path

# ---- assumptions that are not in the source ----------------------------------------------------
RAID_SECONDS = 90       # how long a raid lasts: unknown until played, used only for mana and traps
SPELL_DIST = 2          # tiles from a spell's centre to a typical victim
RANGED_DIST = 4         # tiles between a shooter and its target
TRAP_PASS_SECONDS = 12  # a party takes this long to pass a trap, in and out: heroes avoid traps they know
SHOCK_HITS = 8          # a Shock Trap's lightning rolls to hit on most ticks it lives (ProcessLightning)
TRAP_GOLD_SHARE = 0.25  # share of the gold the typical player puts into traps
SPELL_EFFICIENCY = 0.5  # a typical player lands this share of what mana and cooldowns allow
TRAP_EFFICIENCY = 0.5   # share of traps a party actually walks past
UPGRADE_GOLD_SHARE = 0.25  # share of a level's gold the typical player puts into upgrades
MAX_SUPPORT = 0.8       # however strong, spells and traps don't win a raid without an army
ATTACK_PAUSE = 0.3      # seconds a unit spends between swings on top of the attack animation
HERO_SWING = 16 / 20    # hero monsters: 16-frame attack at one frame per tick
BOT_THINK = 6 / 20      # ThinkInterval in keeper_bots.cpp
BLOCK_UPTIME = 0.8      # share of attacks that land while a bot stands or swings, so can block
SHARE_KINDS, SHARE_SPELLS, SHARE_TRAPS = 55, 35, 10  # how the typical player splits Renown
FOCUS_FAMILIES = 4      # the typical player climbs this many monster families
MASTER_MAGIC_TO_HIT = 10 + 50  # the master is a Warrior: 10 magic + 50 base (GetMagicToHit)
HERO_MONSTER_LEVEL = 5  # RegisterHeroMonsterData

# Spell hits and victims, from the engine's missile code (see BALANCE.md).
SPELL_TARGETS = {"Nova": 2, "Wall": 2, "Guard": 1, "Stone": 1, "Inferno": 2, "Fireball": 2, "Chain": 4,
                 "Shock Wall": 3, "Wave": 3, "Ring": 3, "Apocalypse": 8}


def read(root, rel):
    return (root / rel).read_text(encoding="utf-8")


def constants(text, seed=None):
    """Every `constexpr int NAME = expr;` in a file, evaluated in order."""
    values = dict(seed or {})
    for name, expr in re.findall(r"constexpr (?:int|size_t|uint16_t)\s+(\w+)\s*=\s*([^;]+);", text):
        try:
            values[name] = eval(expr.replace("static_cast<int>", ""), {"__builtins__": {}}, values)
        except Exception:
            pass
    return values


class Game:
    def __init__(self, root, coop=False):
        self.coop = coop
        keeper = "Source/keeper/"
        camp = read(root, keeper + "keeper_campaign.cpp")
        self.levels = [
            dict(name=m[0], raids=int(m[1]), gold=int(m[2]), strength=int(m[3]), party=int(m[4]), growth=int(m[5]))
            for m in re.findall(r'\{ "([^"]+)",\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(?:KeepLevelSeed|0x[0-9A-F]+) \}', camp)
            if m[0] != "Custom Map"
        ]
        self.campaign = constants(camp)
        game_text = read(root, keeper + "keeper_game.cpp")
        game_hpp = read(root, keeper + "keeper_game.hpp")
        self.g = constants(game_hpp, constants(game_text, {"ManaScale": 100, "TicksPerSecond": 20}))
        heroes_text = read(root, keeper + "keeper_heroes.cpp")
        self.heroes_c = constants(heroes_text)
        self.hero_stats = [[int(x) for x in m] for m in re.findall(r"\{ (\d+), (\d+), (\d+), (\d+), (\d+), (\d+) \},\s*// (?:Warrior|Rogue|Sorcerer)", heroes_text)]
        self.heroes_c.update(constants(read(root, keeper + "keeper_campaign.hpp")))
        self.bots_c = constants(read(root, keeper + "keeper_bots.cpp"))
        self.p = constants(read(root, keeper + "keeper_profile.cpp"))
        roster = read(root, keeper + "keeper_roster.cpp")
        self.r = constants(roster, {"NumCampaignLevels": 20})
        self.spells = [
            dict(name=m[0], mana=int(m[1]), level=int(m[2]), cooldown=eval(m[3]), rank=int(m[4]))
            for m in re.findall(r'\{ "([^"]+)", MissileID::\w+, (\d+), (\d+), ([\d *]+), (\d+), SpellID', game_text)
        ]
        self.traps = [
            dict(name=m[0], cost=int(m[1]), rank=int(m[2]), rearm=eval(m[3]), base=int(m[4]), per=int(m[5]))
            for m in re.findall(r'\{ "([^"]+)", (\d+), (\d+), ([\d *]+), (\d+), (\d+), SpellID', game_hpp)
        ]
        upgrades = read(root, keeper + "keeper_upgrades.cpp")
        self.upgrades = [
            dict(name=m[0], tiers=int(m[1]), base=int(m[2]), step=int(m[3]), effect=int(m[4]))
            for m in re.findall(r'\.cardName = "([^"]+)".*?\.maxTier = (\d+), \.baseCost = (\d+), \.costStep = (\d+), \.effectPerTier = (\d+)', upgrades)
        ]
        self.upgrades_scale = "ScaleByHeroStrength(info.baseCost" in upgrades
        monsters = {}
        for rel in ("assets/txtdata/monsters/monstdat.tsv", "mods/Hellfire/txtdata/monsters/monstdat.tsv"):
            for row in csv.DictReader(read(root, rel).splitlines(), delimiter="\t"):
                monsters[row["_monster_id"]] = row
        self.kinds = []
        table = roster[roster.index("constexpr RosterEntry Roster[]"):]
        previous = None
        pattern = r'\{ (MT_\w+), "([^"]+)", (\w+), "(?:[^"\\]|\\.)*"(?:, (\d+), (\d+), (\d+)|, (\d+))? \},'
        for m in re.findall(pattern, table):
            row = monsters.get(m[0])
            if row is None or row["availability"] == "Never":
                continue
            kind = self.make_kind(m, row, previous)
            self.kinds.append(kind)
            previous = kind

    # ---- roster ---------------------------------------------------------------------------------
    def make_kind(self, m, row, previous):
        r = self.r
        life = (int(row["hitPointsMinimum"]) + int(row["hitPointsMaximum"])) // 2
        ac = int(row["armorClass"])
        toughness = life * (100 + ac) // 100
        mind, maxd, to_hit = int(row["minDamage"]), int(row["maxDamage"]), int(row["toHit"])
        punch = (mind + maxd) // 2 * min(to_hit, 100) // 100
        ai = row["ai"]
        ranged_ai = {"SkeletonRanged", "GoatRanged", "Succubus", "Acid", "Magma", "Storm", "Mega", "Counselor", "FireBat", "Torchant", "Lich", "ArchLich", "Psychorb", "Necromorb", "BoneDemon"}
        percent = r["RangedPowerPercent"] if ai in ranged_ai else r["ChargerPowerPercent"] if ai in ("Rhino", "Snake") else r["AmbusherPowerPercent"] if ai == "Sneak" else 100
        power = math.isqrt(max(toughness * punch, 1)) * percent // 100
        step = r["KindCostRoundTo"]
        cost = (r["KindCostBase"] + power * r["KindCostPerPower"] + step // 2) // step * step
        pop = min(1 + power // r["KindPopPowerDivisor"], r["KindMaxPop"])
        level = int(row["level"])
        rank = max(1, min(1 + (level * r["UnlockRankPerLevelNumerator"] - 1) // r["UnlockRankPerLevelDenominator"], 20))
        same_family = previous is not None and previous["family"] == m[2]
        if m[3]:
            rank, cost, pop = int(m[3]), int(m[4]), int(m[5])
        else:
            if m[6]:
                rank = int(m[6])
            if same_family:
                cost = max(cost, previous["cost"] + r["TierMinCostStep"])
                pop = max(pop, previous["pop"])
        frames = [int(x) for x in row["frames[6]"].split(",")]
        rates = [int(x) for x in row["rate[6]"].split(",")]
        return dict(id=m[0], name=m[1], family=m[2], tier=(previous["tier"] + 1) if same_family else 1, rank=rank, cost=cost, pop=pop,
                    life=life, ac=ac, min=mind, max=maxd, to_hit=to_hit, level=level, power=power, ranged=ai in ranged_ai,
                    arrow=ai in ("SkeletonRanged", "GoatRanged"), period=frames[2] * rates[2] / 20 + ATTACK_PAUSE, ai=ai)

    def kind_life(self, kind, life_percent=100):
        life = kind["life"] if self.coop else max(kind["life"] // 2, 1)
        return life * life_percent / 100

    # ---- level rules ----------------------------------------------------------------------------
    def party_size(self, level, wave):
        rules = self.levels[level - 1]
        return max(1, min(rules["party"] + rules["growth"] * (wave - 1) // 100, self.campaign["MaxPartySize"]))

    def scale(self, level, value):
        return value * self.levels[level - 1]["strength"] // 100

    def scale_armor(self, level, value):
        c = self.campaign
        percent = self.levels[level - 1]["strength"]
        if "HeroArmorStrengthPercent" in c:  # armour takes only part of the level's strength
            percent = 100 + (percent - 100) * c["HeroArmorStrengthPercent"] // 100
        return value * percent // 100

    def supply_cap(self, level, wave):
        g = self.g
        return g["BaseSupply"] + g["SupplyPerLevel"] * (level - 1) + g["SupplyPerRaid"] * (wave - 1)

    def tribute(self, level, wave):
        return self.scale(level, self.g.get("TributeBase", 100) + self.g.get("TributePerWave", 25) * wave)

    def hero_bounty(self, level, wave):
        c = self.heroes_c
        return self.scale(level, c.get("HeroBountyBase", 20) + c.get("HeroBountyPerWave", 5) * wave)

    def bot_bounty(self, level, wave):
        c = self.bots_c
        return self.scale(level, c.get("BotBountyBase", 30) + c.get("BotBountyPerWave", 10) * wave)

    def master_level(self, level):
        return 1 + self.master_per_level * (level - 1)

    def spell_engine_level(self, spell, raised):
        per = self.g.get("RaisesPerEngineSpellLevel", 1)
        return spell["level"] + (raised - 1) // per

    def mana_cost(self, spell, raised):
        base = spell["mana"]
        return max(base - (max(raised, 1) - 1) * max(base // self.g["MaxKeepLevel"], 1), base // 2)

    # ---- heroes ---------------------------------------------------------------------------------
    def monster_hero(self, level, wave, cls, champion):
        if self.hero_stats:
            line = self.hero_stats["WRS".index(cls)]
            c = self.heroes_c
            hp = line[0] + line[1] * wave
            lo = line[2] + c["HeroMinDamagePerTenRaids"] * wave // 10
            hi = line[3] + c["HeroMaxDamagePerTenRaids"] * wave // 10
            ac = line[4] + line[5] * wave
        else:  # the first guesses, for comparing with old revisions
            hp, lo, hi, ac = {
                "W": (30 + 10 * wave, 2 + wave, 6 + 2 * wave, 10 + 3 * wave),
                "R": (20 + 7 * wave, 1 + wave, 4 + 2 * wave, 5 + 2 * wave),
                "S": (15 + 6 * wave, 2 + wave, 5 + 2 * wave, 3 + wave),
            }[cls]
        if champion:
            hp = hp * 3 // 2
        return dict(cls=cls, bot=False, hp=self.scale(level, hp), min=min(self.scale(level, lo), 255), max=min(self.scale(level, hi), 255),
                    ac=min(self.scale_armor(level, ac), 255), ranged=cls != "W", to_hit=60, period=HERO_SWING + ATTACK_PAUSE)

    def bot_hero(self, level, wave, cls, champion):
        base = {"W": (30, 10, 20, 25, 18, 2, 2, 30, 70, 60, 50), "R": (20, 15, 30, 20, 23, 2, 1, 20, 50, 70, 50), "S": (15, 35, 15, 20, 9, 1, 1, 10, 50, 50, 70)}[cls]
        st, mag, dex, vit, adj, lvl_life, chr_life, block_bonus, melee_hit, ranged_hit, magic_hit = base
        c = self.bots_c
        per_raid = c.get("BotStatsPerRaid", 3)
        bonus = self.scale(level, per_raid * (wave - 1))
        vit_bonus = per_raid * (wave - 1) if "BotStatsPerRaid" in c else bonus  # before that, vitality scaled too
        st, mag, dex, vit = st + bonus, mag + bonus, dex + bonus, vit + vit_bonus
        clvl = min(wave * 2 - 1, 30)
        hp = adj + lvl_life * clvl + chr_life * vit
        if champion:
            hp = hp * 3 // 2
        hp = self.scale(level, hp)
        clvl = min(clvl + c["LevelsPerCampaignLevel"] * (level - 1), c["MaxBotLevel"])
        potions = c.get("BotPotionsBase", 2) + wave // c.get("BotRaidsPerPotion", 2)
        if cls == "W":
            dam, to_hit, period = (2 + 6) / 2 + st * clvl // 100, clvl + dex // 2 + melee_hit, 16 / 20
            dam *= 1 + min(clvl, 100) / 100  # critical strikes
        elif cls == "R":
            dam, to_hit, period = (1 + 4) / 2 + (st + dex) * clvl // 200, clvl + dex + ranged_hit - RANGED_DIST * RANGED_DIST // 2, 16 / 20
        else:
            dam, to_hit, period = 4.5 + mag // 8 + min(1 + wave // 2, 15) + 1, mag + magic_hit - RANGED_DIST, 16 / 20
        return dict(cls=cls, bot=True, hp=hp, potions=potions, damage=dam, to_hit=to_hit, period=period + BOT_THINK, clvl=clvl, dex=dex,
                    armor=dex // 5 + (3 if cls == "W" else 0), shield=cls == "W", block_base=dex + block_bonus, magic_vs_ac=cls == "S")

    def party(self, level, wave):
        champion = wave >= self.levels[level - 1]["raids"]
        out = []
        for i in range(self.party_size(level, wave)):
            cls = "WRS"[i % 3]
            out.append(self.bot_hero(level, wave, cls, champion) if i < 4 else self.monster_hero(level, wave, cls, champion))
        return out

    def hero_dps_against(self, hero, kind, armor_bonus=0):
        ac = kind["ac"] + armor_bonus
        if hero["bot"]:
            if hero["magic_vs_ac"]:
                hit = hero["to_hit"] - kind["level"] * 2  # magic ignores armour
            else:
                hit = hero["to_hit"] - ac
            return hero["damage"] * min(max(hit, 5), 95) / 100 / hero["period"]
        hit = (90 - ac - RANGED_DIST) if hero["ranged"] else hero["to_hit"]
        hit = min(max(hit, 5), 95)
        return (hero["min"] + hero["max"]) / 2 * hit / 100 / hero["period"]

    def kind_dps_against(self, kind, hero, damage_percent=100):
        dam = (kind["min"] + kind["max"]) / 2 * damage_percent / 100
        if hero["bot"]:
            if kind["ranged"]:
                if kind["arrow"]:
                    hit = kind["to_hit"] + 2 * (kind["level"] - hero["clvl"]) + 30 - 2 * RANGED_DIST - hero["armor"]
                else:
                    hit = 40 + 2 * (kind["level"] - hero["clvl"]) - 2 * RANGED_DIST
                hit = max(hit, 10)
                block = hero["block_base"] - 2 * (kind["level"] - hero["clvl"]) if hero["shield"] else 0
            else:
                hit = max(kind["to_hit"] + 2 * (kind["level"] - hero["clvl"]) + 30 - hero["armor"], 15)
                block = hero["block_base"] + 2 * hero["clvl"] - 2 * kind["level"] if hero["shield"] else 0
            hit = min(hit, 100) / 100 * (1 - min(max(block, 0), 100) / 100 * BLOCK_UPTIME)
        else:
            hit = (90 - hero["ac"] - RANGED_DIST) if kind["ranged"] else kind["to_hit"]
            hit = min(max(hit, 5), 95 if kind["ranged"] else 100) / 100
        return dam * hit / kind["period"]

    # ---- spells ---------------------------------------------------------------------------------
    def spell_damage(self, spell, raised, clvl, guardian_table=True):
        """Damage to one victim of one cast, before the hit chance."""
        name, spl = spell["name"], self.spell_engine_level(spell, raised)
        scale = 1.125 ** spl
        if name == "Nova":
            return 2 * ((17.5 + clvl + 5) // 2) * scale  # two balls of the ring on a victim
        if name == "Fireball":
            return (2 * (clvl + 11) + 4) * scale
        if name == "Wall":
            return (13 + clvl) / 8 * 20 * 2.0  # 2 seconds in the fire: heroes step out of it
        if name == "Ring":
            return (13 + clvl) / 8 * 20 * 2.0
        if name == "Shock Wall":
            return (13 + clvl) / 4 * 20 * 0.6 * 2  # heroes walk through it unwarned: two tiles' worth
        if name == "Inferno":
            return (8 * (clvl / 2 + 0.5) + 16) * 1.5 / 64 * 20 * 2
        if name == "Chain":
            return (clvl / 2 + 2.5) * (6 + spl / 2)
        if name == "Wave":
            return (5.5 + clvl) * 2
        if name == "Apocalypse":
            per = self.g["ApocalypseDamagePerLevel"]
            return (3.5 * clvl + clvl) * (100 + per * (raised - 1)) / 100
        if name == "Guard":
            shots = max(min(spl + clvl // 2, 30) * 16, 30) / 16
            g = self.g
            if "GuardianDamageBase" in g:
                shot = (g["GuardianDamageBase"] + clvl * g["GuardianDamagePerMasterLevel"] // 10) * (100 + g["GuardianDamagePerRaise"] * (raised - 1)) // 100
            else:
                shot = 4.5 + 10 // 8 + 0 + 1  # AddFirebolt's own damage: the master learned no Guardian spell
            return shots * shot
        return 0

    def spell_hit(self, hero):
        if hero["bot"]:
            return self.g.get("MasterSpellBotHit", 40) / 100
        return min(max(MASTER_MAGIC_TO_HIT - HERO_MONSTER_LEVEL * 2 - SPELL_DIST, 5), 95) / 100

    def spell_output(self, level, spell_levels, mana_percent, party):
        """Best damage a raid's mana can do, and which spell does it. Returns (damage, per-spell list)."""
        regen = self.g["ManaRegenPerTick"] * mana_percent / 100 * 20 / 100
        pool = self.g["MaxMana"] + regen * RAID_SECONDS
        clvl = self.master_level(level)
        hit = sum(self.spell_hit(h) for h in party) / len(party)
        rows = []
        for spell in self.spells:
            raised = spell_levels.get(spell["name"], 0)
            if raised == 0 or spell["name"] == "Stone":
                continue
            per_hit = self.spell_damage(spell, raised, clvl) * hit
            targets = min(SPELL_TARGETS[spell["name"]], len(party))
            mana = self.mana_cost(spell, raised)
            rows.append((per_hit * targets / mana, per_hit * targets, mana, spell))
        rows.sort(key=lambda r: -r[0])
        damage, seconds = 0.0, float(RAID_SECONDS)
        for density, cast, mana, spell in rows:
            casts = min(pool / mana, seconds / (spell["cooldown"] / 20))
            damage += casts * cast
            pool -= casts * mana
            seconds -= casts * spell["cooldown"] / 20
        return damage

    def upgrade_cost(self, level, upgrade, tier):
        cost = upgrade["base"] + tier * upgrade["step"]
        return self.scale(level, cost) if self.upgrades_scale else cost

    def xp_for(self, level):
        return self.p["XpPerLevelStep"] * level * (level - 1) // 2

    def keeper_level(self, xp):
        level = 1
        while xp >= self.xp_for(level + 1):
            level += 1
        return level

    def kind_renown(self, kind):
        return self.p["KindCostBase"] + kind["rank"] // self.p["LevelsPerExtraRenown"]

    def item_renown(self, rank):
        return self.p["SpellCostBase"] + rank // self.p["LevelsPerExtraRenown"]

    def raise_renown(self, level):
        return self.p["RaiseCostBase"] + level // self.p["LevelsPerExtraRaiseCost"]


# ---- the typical player ---------------------------------------------------------------------------

SAVE_UP = -1

class Player:
    """Spends Renown between levels. Starts with the rank 1 items, as GrantStarters does."""

    def __init__(self, game):
        self.game = game
        self.kinds = {k["id"] for k in game.kinds if k["rank"] <= 1}
        self.spells = {s["name"]: 1 for s in game.spells if s["rank"] <= 1}
        self.traps = {t["name"]: 1 for t in game.traps if t["rank"] <= 1}
        self.spent = {"kinds": 0, "spells": 0, "traps": 0}
        self.families = []

    def family_next(self, keeper_level):
        """Next tier of each family, if the keeper level allows it."""
        out = {}
        for kind in self.game.kinds:
            if kind["id"] in self.kinds or kind["family"] in out:
                continue
            if kind["rank"] <= keeper_level:
                out[kind["family"]] = kind
            else:
                out.setdefault(kind["family"], None)
        return {f: k for f, k in out.items() if k is not None}

    def best_kind_to_unlock(self, keeper_level):
        options = self.family_next(keeper_level)
        if not options:
            return None
        def score(kind):
            bonus = 1.5 if kind["family"] in self.families else 1.0
            return bonus * kind["power"] / kind["pop"] * (1 + kind["power"] / 100)
        return max(options.values(), key=score)

    def spend(self, keeper_level):
        budget = self.game.p["RenownPerLevel"] * (keeper_level - 1) - sum(self.spent.values())
        while budget > 0:
            total = max(sum(self.spent.values()), 1)
            wants = [("kinds", SHARE_KINDS), ("spells", SHARE_SPELLS), ("traps", SHARE_TRAPS)]
            wants.sort(key=lambda w: self.spent[w[0]] / total - w[1] / 100)
            bought = False
            for category, _ in wants:
                cost = getattr(self, "buy_" + category)(keeper_level, budget)
                if cost == SAVE_UP:
                    return budget  # the next thing wanted is worth waiting for
                if cost:
                    budget -= cost
                    self.spent[category] += cost
                    bought = True
                    break
            if not bought:
                break
        return budget

    def buy_kinds(self, keeper_level, budget):
        kind = self.best_kind_to_unlock(keeper_level)
        if kind is None:
            return 0
        if self.game.kind_renown(kind) > budget:
            return SAVE_UP
        self.kinds.add(kind["id"])
        if kind["family"] not in self.families and len(self.families) < FOCUS_FAMILIES:
            self.families.append(kind["family"])
        return self.game.kind_renown(kind)

    def buy_spells(self, keeper_level, budget):
        # New strong spells first, then raise the best damage spell owned.
        for spell in self.game.spells:
            if spell["name"] not in self.spells and spell["rank"] <= keeper_level and spell["name"] in ("Fireball", "Chain", "Apocalypse"):
                cost = self.game.item_renown(spell["rank"])
                if cost <= budget:
                    self.spells[spell["name"]] = 1
                    return cost
                return SAVE_UP
        for name in ("Apocalypse", "Fireball", "Nova"):
            if name in self.spells and self.spells[name] < self.game.g["MaxKeepLevel"]:
                cost = self.game.raise_renown(self.spells[name])
                if cost <= budget:
                    self.spells[name] += 1
                    return cost
                return SAVE_UP
        return 0

    def buy_traps(self, keeper_level, budget):
        for name in ("Arrow Trap", "Fire Trap"):
            if name in self.traps and self.traps[name] < self.game.g["MaxKeepLevel"]:
                cost = self.game.raise_renown(self.traps[name])
                if cost <= budget and self.traps[name] <= (self.traps.get("Fire Trap", 1) if name == "Arrow Trap" else 99):
                    self.traps[name] += 1
                    return cost
        return 0


def lord_unit(game, level, tiers):
    """The Keep Lord, as a one-off kind: stats from OnMonsterInit, sprites and swing from the Black Knight."""
    life_up = 100 + tiers[4] * game.upgrades[4]["effect"]
    damage_up = 100 + tiers[5] * game.upgrades[5]["effect"]
    return dict(name="Lord", life=game.scale(level, 600) * life_up / 100, ac=game.scale_armor(level, 40),
                min=min(game.scale(level, 12), 255) * damage_up / 100, max=min(game.scale(level, 24), 255) * damage_up / 100,
                to_hit=110, level=24, ranged=False, arrow=False, period=16 / 20 + ATTACK_PAUSE, pop=0, cost=0)


def hero_hit_damage(hero):
    return hero["damage"] if hero["bot"] else (hero["min"] + hero["max"]) / 2


def lord_stand(game, lord, party):
    """Seconds the Lord alone lasts against a party, and seconds it would take him to kill that party."""
    average_blow = sum(hero_hit_damage(hero) for hero in party) / len(party)
    rate = sum(game.hero_dps_against(hero, lord) / hero_hit_damage(hero) for hero in party)
    lasts = max(math.ceil(lord["life"] / average_blow), 1) / max(rate, 0.01)
    kills = sum((h["hp"] * (1 + 0.5 * h["potions"]) if h["bot"] else h["hp"]) / max(game.kind_dps_against(lord, h), 0.01) for h in party)
    return lasts, kills


def army_score(game, army, party, life_percent, damage_percent, armor_bonus):
    """How long the army lasts over how long the party lasts; above 1 the army wins. The Lord is left
    out: he stands at the end of the keep and is reported on his own. Every monster takes at least
    one blow, so a swarm of tiny monsters still buys time."""
    units = list(army)
    # Time for the army to kill the party, one hero at a time, with the whole army's fire.
    kill_time = 0.0
    for hero in party:
        dps = sum(game.kind_dps_against(k, hero, damage_percent) * n for k, n in units)
        life = hero["hp"] * (1 + 0.5 * hero["potions"]) if hero["bot"] else hero["hp"]
        kill_time += life / max(dps, 0.01)
    # Time for the party to kill the army, counted in blows.
    death_time = 0.0
    average_blow = sum(hero_hit_damage(hero) for hero in party) / len(party)
    for kind, count in units:
        life = game.kind_life(kind, life_percent)
        rate = sum(game.hero_dps_against(hero, kind, armor_bonus) / hero_hit_damage(hero) for hero in party)
        death_time += count * max(math.ceil(life / average_blow), 1) / max(rate, 0.01)
    return death_time / max(kill_time, 0.001), kill_time, death_time


def trap_output(game, level, wave, gold, trap_levels, party):
    """Damage the trap slots can do in a raid, and the gold left."""
    best = None
    for trap in game.traps:
        if trap["name"] not in trap_levels or trap["base"] == 0:
            continue
        raised = trap_levels[trap["name"]]
        damage = game.scale(level, trap["base"] + trap["per"] * wave) * (100 + game.g["TrapDamagePerLevel"] * (raised - 1)) // 100
        rearm = trap["rearm"] * max(100 - game.g["TrapRearmPerLevel"] * (raised - 1), game.g["TrapRearmFloorPercent"]) // 100 / 20
        hits = []
        for hero in party:
            if hero["bot"]:
                hits.append(0.9 if trap["name"] == "Arrow Trap" else 0.4)
            else:
                hits.append(min(max(90 - hero["ac"] - 1, 5), 95) / 100)
        if trap["name"] == "Shock Trap":
            damage *= SHOCK_HITS
        per_raid = TRAP_PASS_SECONDS / rearm * damage * sum(hits) / len(hits)
        if best is None or per_raid / trap["cost"] > best[0] / best[1]:
            best = (per_raid, trap["cost"])
    if best is None:
        return 0.0, gold
    count = min(game.g["MaxTraps"], int(gold * TRAP_GOLD_SHARE // best[1]))
    return count * best[0], gold - count * best[1]


def best_army(game, level, gold, pop, owned, party, life_percent, damage_percent, armor_bonus):
    """Best one- or two-kind army that gold and Pop allow."""
    kinds = [k for k in game.kinds if k["id"] in owned]
    # The top units by square-law density: enough candidates that the search stays quick.
    def density(k):
        hero = party[len(party) // 2]
        return game.kind_dps_against(k, hero, damage_percent) * game.kind_life(k, life_percent) / (k["pop"] ** 2)
    kinds = sorted(kinds, key=density, reverse=True)[:10] + sorted(kinds, key=lambda k: k["cost"] / max(game.kind_life(k), 1))[:2]
    best = (0.0, [], 0)
    for a in kinds:
        for b in kinds:
            max_a = min(gold // a["cost"], pop // a["pop"])
            step = max(1, max_a // 12)
            for count_a in range(0, max_a + 1, step):
                left_gold, left_pop = gold - count_a * a["cost"], pop - count_a * a["pop"]
                count_b = min(left_gold // b["cost"], left_pop // b["pop"]) if b is not a else 0
                army = [(a, count_a)] + ([(b, count_b)] if count_b > 0 else [])
                ratio, _, _ = army_score(game, army, party, life_percent, damage_percent, armor_bonus)
                spent = count_a * a["cost"] + count_b * b["cost"]
                if ratio > best[0]:
                    best = (ratio, army, spent)
    return best


def play_level(game, player, level, xp):
    rules = game.levels[level - 1]
    keeper = game.keeper_level(xp)
    unspent = player.spend(keeper)
    gold = rules["gold"]
    tiers = [0] * len(game.upgrades)
    bought_spent = 0
    wave_rows = []
    for wave in range(1, rules["raids"] + 1):
        party = list(game.party(level, wave))
        cap = game.supply_cap(level, wave)
        life, damage, armor = 100 + tiers[0] * game.upgrades[0]["effect"], 100 + tiers[1] * game.upgrades[1]["effect"], tiers[2] * game.upgrades[2]["effect"]
        while True:  # buy upgrades up to their share of everything earned so far
            options = [i for i in (1, 0, 3, 2, 5, 4) if tiers[i] < game.upgrades[i]["tiers"]]
            if not options:
                break
            cost = game.upgrade_cost(level, game.upgrades[options[0]], tiers[options[0]])
            if bought_spent + cost > gold * UPGRADE_GOLD_SHARE:
                break
            bought_spent += cost
            tiers[options[0]] += 1
        life, damage, armor = 100 + tiers[0] * game.upgrades[0]["effect"], 100 + tiers[1] * game.upgrades[1]["effect"], tiers[2] * game.upgrades[2]["effect"]
        available = gold - bought_spent
        trap_damage, after_traps = trap_output(game, level, wave, available, player.traps, party)
        lord = lord_unit(game, level, tiers)
        ratio, army, spent = best_army(game, level, after_traps, cap, player.kinds, party, life, damage, armor)
        lord_lasts, lord_kills = lord_stand(game, lord, party)
        # The cheap swarm: as many as gold and Pop allow of the cheapest kind owned.
        cheapest = min((k for k in game.kinds if k["id"] in player.kinds), key=lambda k: k["cost"] / max(k["pop"], 1))
        swarm_count = min(after_traps // cheapest["cost"], cap // cheapest["pop"])
        swarm_ratio, _, _ = army_score(game, [(cheapest, swarm_count)], party, life, damage, armor)
        pop_used = sum(k["pop"] * n for k, n in army)
        left = after_traps - spent
        smallest = min((k["pop"] for k, _ in army), default=1)
        binding = "Pop" if cap - pop_used < smallest and left >= min(game.upgrade_cost(level, u, 0) for u in game.upgrades) else "gold"
        mana_percent = 100 + tiers[3] * game.upgrades[3]["effect"]
        spell_damage = game.spell_output(level, player.spells, mana_percent, party)
        party_life = sum(h["hp"] * (1 + 0.5 * h["potions"]) if h["bot"] else h["hp"] for h in party)
        spell_damage *= SPELL_EFFICIENCY
        trap_damage *= TRAP_EFFICIENCY
        f = min((spell_damage + trap_damage) / party_life, MAX_SUPPORT)
        heroes = len(party)
        bots = min(heroes, 4)
        xp += heroes * (game.p["HeroSlainXpBase"] + game.p["HeroSlainXpPerWave"] * wave) + game.p["RaidXpBase"] + game.p["RaidXpPerWave"] * wave
        bounty = bots * game.bot_bounty(level, wave) + (heroes - bots) * game.hero_bounty(level, wave)
        tribute = game.tribute(level, wave) if wave < rules["raids"] else 0
        hero_dps = sum(hero_hit_damage(h) * (0.8 if h["bot"] else 0.6) / h["period"] for h in party)
        wave_rows.append(dict(wave=wave, heroes=heroes, party_life=party_life, hero_dps=hero_dps, cap=cap, pop_used=pop_used, gold=gold, army_cost=spent, ratio=ratio, swarm=(swarm_count, cheapest["name"], swarm_ratio), ratio_spells=ratio / (1 - f), lord_lasts=lord_lasts, lord_kills=lord_kills, spell_frac=spell_damage / party_life, trap_frac=trap_damage / party_life, army=army, binding=binding, tiers=list(tiers), bounty=bounty, tribute=tribute))
        gold += bounty + tribute
    xp += game.p["ClearXpBase"] + game.p["ClearXpPerLevel"] * level
    return dict(level=level, name=rules["name"], keeper_start=keeper, keeper_end=game.keeper_level(xp), xp=xp, renown=game.p["RenownPerLevel"] * (game.keeper_level(xp) - 1), unspent=unspent, spells=dict(player.spells), traps=dict(player.traps), kinds=sorted(player.kinds), n_kinds=len(player.kinds), waves=wave_rows, gold_end=gold, master=game.master_level(level), spent=dict(player.spent))


def simulate(game):
    player = Player(game)
    xp = 0
    rows = []
    for level in range(1, len(game.levels) + 1):
        row = play_level(game, player, level, xp)
        xp = row["xp"]
        rows.append(row)
    return rows


def solve_strength(game, first_target, last_target):
    """Hero strength per level that puts the last raid's R (with spells and traps) on a straight line of targets."""
    player = Player(game)
    xp = 0
    print("| Lvl | Strength % | R last raid | R first raid | Target |")
    print("|---|---|---|---|---|")
    for level in range(1, len(game.levels) + 1):
        if level == 1:
            row = play_level(game, player, level, xp)
            xp = row["xp"]
            print(f"| 1 | {game.levels[0]['strength']} | {row['waves'][-1]['ratio_spells']:.2f} | {row['waves'][0]['ratio_spells']:.2f} | fixed |")
            continue
        target = first_target + (last_target - first_target) * (level - 1) / (len(game.levels) - 1)
        low, high = 50, 500
        for _ in range(7):
            mid = (low + high) // 2
            game.levels[level - 1]["strength"] = mid
            trial = Player(game)
            trial.__dict__.update(copy.deepcopy({k: v for k, v in player.__dict__.items() if k != "game"}))
            if play_level(game, trial, level, xp)["waves"][-1]["ratio_spells"] > target:
                low = mid
            else:
                high = mid
        game.levels[level - 1]["strength"] = low
        row = play_level(game, player, level, xp)
        xp = row["xp"]
        print(f"| {level} | {low} | {row['waves'][-1]['ratio_spells']:.2f} | {row['waves'][0]['ratio_spells']:.2f} | {target:.2f} |")


# ---- reports ----------------------------------------------------------------------------------------

def table(title, header, lines):
    print(f"\n### {title}\n")
    print("| " + " | ".join(header) + " |")
    print("|" + "---|" * len(header))
    for line in lines:
        print("| " + " | ".join(str(x) for x in line) + " |")


def report_progress(game, rows):
    lines = []
    previous = 0
    for row in rows:
        eligible = [k for k in game.kinds if k["rank"] <= row["keeper_end"]]
        offer = sum(game.kind_renown(k) for k in eligible if k["rank"] > 1)
        spell_names = ", ".join(f"{n[:4]} {v}" for n, v in row["spells"].items() if v > 1 or n in ("Fireball", "Chain", "Apocalypse"))
        lines.append((row["level"], row["keeper_start"], row["keeper_end"], row["xp"] - previous, row["xp"], row["renown"], row["n_kinds"], len(eligible), offer, row["unspent"], spell_names))
        previous = row["xp"]
    table("XP and Renown (first clears, every hero killed)", ["Lvl", "KL start", "KL end", "XP earned", "XP total", "Renown total", "Kinds owned", "Kinds eligible", "Renown for all eligible", "Unspent", "Key spells"], lines)
    total_kinds = sum(game.kind_renown(k) for k in game.kinds if k["rank"] > 1)
    total_spell = sum(game.item_renown(s["rank"]) for s in game.spells if s["rank"] > 1)
    raise_all = sum(game.raise_renown(i) for i in range(1, game.g["MaxKeepLevel"]))
    print(f"\nWhole catalogue: all kinds {total_kinds} Renown; spell unlocks {total_spell}; raising one item 1 to {game.g['MaxKeepLevel']} costs {raise_all}; 11 spells and 4 traps maxed {raise_all * 15}.")
    ranks = sorted(k["rank"] for k in game.kinds)
    print("Kinds by unlock rank: " + ", ".join(f"{r}:{ranks.count(r)}" for r in range(1, 21) if ranks.count(r)))


def report_gold(game, rows):
    lines = []
    for row in rows:
        w = row["waves"]
        income = sum(x["bounty"] + x["tribute"] for x in w)
        first, last = w[0], w[-1]
        unit = last["army"][0][0] if last["army"] else None
        lines.append((row["level"], first["gold"], sum(x["tribute"] for x in w), sum(x["bounty"] for x in w), row["gold_end"], first["cap"], last["cap"], first["army_cost"], last["army_cost"], first["binding"], last["binding"], sum(last["tiers"])))
    table("Gold and Pop", ["Lvl", "Start gold", "Tribute", "Bounties", "Gold if none spent", "Pop W1", "Pop last", "Army gold W1", "Army gold last", "Binds W1", "Binds last", "Upgrade tiers bought"], lines)


def army_name(army):
    return " + ".join(f"{n} {k['name']}" for k, n in army if n)


def report_combat(game, rows):
    lines = []
    for row in rows:
        for x in (row["waves"][0], row["waves"][-1]):
            lines.append((f"{row['level']}/{x['wave']}", x["heroes"], round(x["party_life"]), round(x["hero_dps"]), army_name(x["army"]), f"{x['ratio']:.2f}", f"{x['swarm'][0]} {x['swarm'][1]}: {x['swarm'][2]:.2f}", f"{x['spell_frac'] * 100:.0f}%", f"{x['trap_frac'] * 100:.0f}%", f"{x['ratio_spells']:.2f}", f"{x['lord_lasts']:.0f}s / {x['lord_kills']:.0f}s"))
    table("Combat (R above 1 means the army should win; 'with both' first removes what a raid's mana and traps can kill)", ["Lvl/raid", "Heroes", "Party life", "Party DPS", "Best army", "R", "R of the cheap swarm", "Spells % of party life", "Traps %", "R with both", "Lord lasts / would kill party in"], lines)


def report_consistency(game):
    lines = []
    for level, wave in ((1, 1), (1, 5), (5, 6), (10, 8), (15, 9), (20, 1), (20, 10)):
        champion = wave >= game.levels[level - 1]["raids"]
        for cls in "WRS":
            b, m = game.bot_hero(level, wave, cls, champion), game.monster_hero(level, wave, cls, champion)
            lines.append((f"{level}/{wave}", cls, b["hp"], b["potions"], b["clvl"], f"{b['damage']:.0f}", b["armor"], m["hp"], f"{(m['min'] + m['max']) / 2:.0f}", m["ac"]))
    table("Bot heroes against monster heroes", ["Lvl/raid", "Class", "Bot life", "Potions", "Bot clvl", "Bot dmg", "Bot AC", "Monster life", "Monster dmg", "Monster AC"], lines)
    lines = []
    for level, wave in ((1, 5), (5, 6), (10, 8), (15, 9), (20, 10)):
        champion = wave >= game.levels[level - 1]["raids"]
        cells = [f"{level}/{wave}"]
        for cls in "WRS":
            ac = game.monster_hero(level, wave, cls, champion)["ac"]
            cells.append(f"{ac} ({max(min(90 - ac - RANGED_DIST, 95), 5)}%)")
        bot = game.bot_hero(level, wave, "W", champion)
        for mlvl in (5, 20):
            block = min(max(bot["block_base"] + 2 * bot["clvl"] - 2 * mlvl, 0), 100)
            cells.append(f"{block}%")
        lines.append(cells)
    table("Monster heroes' armour (chance for ranged monsters and traps to hit), and Warrior bots' melee block chance", ["Lvl/raid", "Warrior AC", "Rogue AC", "Sorcerer AC", "Block vs mlvl 5", "Block vs mlvl 20"], lines)
    lines = []
    for level, wave in ((1, 1), (5, 1), (10, 1), (15, 1), (20, 1), (20, 10)):
        champion = wave >= game.levels[level - 1]["raids"]
        cells = [f"{level}/{wave}"]
        w, r = game.bot_hero(level, wave, "W", champion), game.bot_hero(level, wave, "R", champion)
        for ac in (10, 30, 50, 80):
            cells.append(f"{max(min(w['to_hit'] - ac, 95), 5)}% / {max(min(r['to_hit'] - ac, 95), 5)}%")
        lines.append(cells)
    table("Bots' chance to hit monster armour (Warrior melee / Rogue bow)", ["Lvl/raid", "AC 10", "AC 30", "AC 50", "AC 80"], lines)


def report_spells(game, rows):
    lines = []
    for level in (1, 5, 10, 15, 20):
        row = rows[level - 1]
        clvl = game.master_level(level)
        party = row["waves"][-1]
        party_life = party["party_life"]
        for spell in game.spells:
            if spell["rank"] > row["keeper_end"]:
                continue
            for raised in (row["spells"].get(spell["name"], 1), 15):
                hit = sum(game.spell_hit(h) for h in game.party(level, party["wave"])) / party["heroes"]
                dmg = game.spell_damage(spell, raised, clvl)
                targets = min(SPELL_TARGETS[spell["name"]], party["heroes"])
                mana = game.mana_cost(spell, raised)
                lines.append((level, clvl, spell["name"], raised, game.spell_engine_level(spell, raised), mana, spell["cooldown"] / 20, round(dmg), round(dmg * hit * targets), f"{dmg * hit * targets / mana:.1f}", f"{dmg * hit * targets / party_life * 100:.0f}%"))
    table("Spells (per cast; last column is one cast against the level's last raid)", ["Lvl", "Master clvl", "Spell", "Raised to", "Engine lvl", "Mana", "Cooldown s", "Dmg/victim", "Expected total", "Per mana", "% of party life"], lines)


def report_traps(game, rows):
    lines = []
    for level in (1, 5, 10, 15, 20):
        wave = game.levels[level - 1]["raids"]
        champion = True
        ac = game.monster_hero(level, wave, "W", champion)["ac"]
        ac_r = game.monster_hero(level, wave, "R", champion)["ac"]
        for trap in game.traps:
            if trap["base"] == 0:
                continue
            for raised in (rows[level - 1]["traps"].get(trap["name"], 1),):
                damage = game.scale(level, trap["base"] + trap["per"] * wave) * (100 + game.g["TrapDamagePerLevel"] * (raised - 1)) // 100
                rearm = trap["rearm"] * max(100 - game.g["TrapRearmPerLevel"] * (raised - 1), game.g["TrapRearmFloorPercent"]) // 100 / 20
                hit_r = max(90 - ac_r - 1, 5)
                lines.append((level, trap["name"], raised, trap["cost"], damage, rearm, f"{max(90 - ac - 1, 5)}% / {hit_r}%", f"{damage * hit_r / 100 / rearm:.0f}"))
    table("Traps at the level's last raid", ["Lvl", "Trap", "Level", "Gold", "Damage", "Rearm s", "Hit Warrior / Rogue", "DPS while a Rogue stands by it"], lines)


def report_roster(game):
    lines = []
    for k in game.kinds:
        hero = dict(bot=False, ac=20, ranged=False)
        dps = (k["min"] + k["max"]) / 2 * (min(max(90 - 20 - RANGED_DIST, 5), 95) if k["ranged"] else min(k["to_hit"], 100)) / 100 / k["period"]
        life = game.kind_life(k)
        lines.append((k["rank"], k["name"], k["family"], k["cost"], k["pop"], k["power"], life, k["ac"], f"{dps:.1f}", f"{dps * life / k['cost']:.1f}", f"{dps * life / k['pop'] ** 2:.1f}"))
    lines.sort(key=lambda r: (r[0], r[3]))
    table("Roster", ["Rank", "Kind", "Family", "Gold", "Pop", "Power", "Life", "AC", "DPS", "DPSxLife per gold", "DPSxLife per Pop squared"], lines)


def report_upgrades(game, rows):
    lines = []
    for level in (1, 5, 10, 15, 20):
        total = sum(game.upgrade_cost(level, u, t) for u in game.upgrades for t in range(u["tiers"]))
        row = rows[level - 1]
        lines.append((level, row["waves"][-1]["gold"], total, f"{total / max(row['gold_end'], 1) * 100:.0f}%", sum(row["waves"][-1]["tiers"]), sum(u["tiers"] for u in game.upgrades)))
    table("Upgrades against gold", ["Lvl", "Gold before last raid", "All upgrades cost", "% of gold earned in the level", "Tiers bought by last raid", "Tiers on offer"], lines)


REPORTS = dict(progress=report_progress, gold=report_gold, combat=report_combat, spells=report_spells, traps=report_traps, upgrades=report_upgrades)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=Path(__file__).resolve().parents[2], type=Path)
    parser.add_argument("--coop", action="store_true", help="monsters at full life, as in a multiplayer game")
    parser.add_argument("--section", action="append", help="progress, gold, combat, consistency, spells, traps, upgrades, roster")
    parser.add_argument("--solve-strength", nargs=2, type=float, metavar=("FIRST", "LAST"), help="print the hero strength table that hits these R targets")
    args = parser.parse_args()
    game = Game(args.root, args.coop)
    keeper_cpp = read(args.root, "Source/keeper/keeper.cpp")
    game.master_per_level = int(re.search(r"MasterLevelsPerCampaignLevel = (\d+)", keeper_cpp).group(1))
    if args.solve_strength:
        solve_strength(game, *args.solve_strength)
        return
    rows = simulate(game)
    sections = args.section or ["progress", "gold", "combat", "consistency", "spells", "traps", "upgrades", "roster"]
    for section in sections:
        if section == "consistency":
            report_consistency(game)
        elif section == "roster":
            report_roster(game)
        else:
            REPORTS[section](game, rows)


if __name__ == "__main__":
    main()
