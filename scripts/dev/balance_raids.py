#!/usr/bin/env python3
"""Measures real raids for docs/BALANCE.md: fields the model's army at a level's last raid and records how it ends.

    python scripts/dev/balance_model.py --plan plan.json
    python scripts/dev/balance_raids.py plan.json --levels 1,5,10 --modes army,full --runs 8 --out raids.csv

Needs a build with -DKEEPER_DEVCTL=ON, and scripts/dev/testrun.ps1 run once to link the game data. Games are
invisible and silent (SDL's dummy drivers, no console), keep to build/test-profiles, and only the games
started here are ever stopped.

With --campaign, a level is played from raid 1 on real gold, buying between raids, so interest, early call, raid
types and thieves all count. Its policies, also given by --modes:
    newbie   the army's main kind and a filler, a few traps, spells at half the usual casting rate, no upgrades
    typical  a quarter of the gold on upgrades, a quarter on traps, spells at half the mana, the rest on the army
    army     only the army
    lord     every coin on Lord upgrades, no monsters, no spells

Modes:
    army   the army and the Lord only
    traps  adds the traps
    spells adds the spells, at the mana share in the plan
    sloppy traps and spells at half that casting rate, no upgrades: a rough stand-in for a weaker player
    full   adds upgrades, traps and spells

The game's random numbers run the same from the same start, so identical runs would come out nearly identical
and look more certain than they are. Each run lays out its army and traps a little differently, as two players would.
"""
import argparse
import collections
import concurrent.futures
import csv
import json
import os
import random
import re
import subprocess
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE_DIR = ROOT / "build" / "Release"
EXE = EXE_DIR / "devilutionx.exe"
CREATE_NO_WINDOW = 0x08000000
SPEED = 200  # ticks a second, as asked of `speed`: raids are timed in ticks, so this only saves waiting
GAME_SECONDS_LIMIT = 300  # real seconds; even the longest raids run in about a minute at this speed
COOP_PORT = 6128  # co-op tests in other checkouts share 6112 and could join each other. Each extra worker takes the next port.
MODES = ("army", "traps", "spells", "sloppy", "full")
# What CanBuildAt says of a tile. Its other refusals hold for every tile.
TILE_REFUSALS = ("Needs open floor", "Too close to", "Heroes are watching")
INI = """[Graphics]
Fullscreen=0
Fit to Screen=0
Width=640
Height=480
[Audio]
Sound Volume=-1600
Music Volume=-1600
[StartUp]
Diablo Intro=0
Hellfire Intro=0
Splash=0
[Network]
Bind Address=127.0.0.1
Port={port}
"""


class Game:
    def __init__(self, profile, plan, role="--keeper-autostart", port=6112, start_wave=None, difficulty=None):
        self.dir = ROOT / "build" / "test-profiles" / profile
        self.dir.mkdir(parents=True, exist_ok=True)
        (self.dir / "diablo.ini").write_text(INI.format(port=port))
        self.ctl = self.dir / "ctl.txt"
        self.out = self.dir / "ctl.txt.out"
        self.out.unlink(missing_ok=True)
        self.ctl.write_text("")
        self.read_at = 0
        lines = ["DARKKEEP-PROFILE 1", "xp 0 0"]
        lines += [f"spell {i} {level}" for i, level in plan["spell_levels"].items()]
        lines += [f"trap {i} {level}" for i, level in plan["trap_levels"].items()]
        (self.dir / "keeper_profile.txt").write_text("\n".join(lines) + "\n")
        args = [str(EXE), "--config-dir", str(self.dir), "--save-dir", str(self.dir), "--keeper-devctl", str(self.ctl),
                *role.split(), "--keeper-level", str(plan["level"]), "--keeper-start-wave", str(start_wave or plan["wave"]), "--keeper-rank", "20"]
        if difficulty is not None:
            args += ["--keeper-difficulty", str(difficulty)]
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
        self.process = subprocess.Popen(args, cwd=EXE_DIR, env=env, creationflags=CREATE_NO_WINDOW, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def send(self, *lines):
        for _ in range(50):
            try:
                with open(self.ctl, "a") as f:
                    f.write("\n".join(lines) + "\n")
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("cannot write to the game")

    def read(self):
        if not self.out.exists():
            return []
        with open(self.out, "rb") as f:
            f.seek(self.read_at)
            data = f.read()
        end = data.rfind(b"\n") + 1
        self.read_at += end
        return data[:end].decode(errors="replace").splitlines()

    def ask(self, command, until, timeout=30):
        """Sends a command and returns all output up to and including the line matching `until`."""
        self.send(command)
        lines = []
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError(f"game exited with {self.process.returncode}")
            for line in self.read():
                lines.append(line)
                if until in line:
                    return lines
            time.sleep(0.02)
        raise TimeoutError(command)

    def stop(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()


def state(game):
    line = game.ask("state", "state t=")[-1]
    return dict(re.findall(r"(\w+)=(\S+)", line))


def floor_plan(game):
    """Floor grid, entrance and vault."""
    token = f"map-end-{time.time()}"
    game.send("map", f"echo {token}")
    rows = []
    doors = []
    deadline = time.time() + 30
    while time.time() < deadline:
        lines = game.read()
        rows += [list(line) for line in lines if len(line) > 50 and set(line) <= set("#.")]
        doors += re.findall(r"^door at=(\d+),(\d+)", "\n".join(lines), re.MULTILINE)
        if any(token in line for line in lines):
            # Closed doors count as solid in the map, but heroes come through them.
            for x, y in doors:
                rows[int(y)][int(x)] = "."
            return rows
        time.sleep(0.02)
    raise TimeoutError("map")


def distances(rows, start):
    seen = {start: 0}
    queue = collections.deque([start])
    parent = {}
    while queue:
        x, y = queue.popleft()
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (1, -1), (-1, 1), (-1, -1)):
            nx, ny = x + dx, y + dy
            if (nx, ny) not in seen and 0 <= ny < len(rows) and 0 <= nx < len(rows[ny]) and rows[ny][nx] == ".":
                seen[(nx, ny)] = seen[(x, y)] + 1
                parent[(nx, ny)] = (x, y)
                queue.append((nx, ny))
    return seen, parent


def place(game, kind, count, candidates, used):
    """Builds `count` of a kind on the candidates, in order. Moves on past a bad tile; any other refusal, such as
    a full keep, would refuse every tile, so it ends the job. @return how many stood."""
    built = 0
    while built < count and candidates:
        tile = candidates.pop(0)
        if any(abs(tile[0] - u[0]) <= 1 and abs(tile[1] - u[1]) <= 1 for u in used):
            continue
        token = f"b{time.time()}"
        lines = game.ask(f"build {kind} {tile[0]} {tile[1]}\nwait 25\necho {token}", token)
        refusals = [line for line in lines if "refused" in line]
        if refusals:
            if any(reason in refusals[0] for reason in TILE_REFUSALS):
                continue
            return built
        used.append(tile)
        built += 1
    return built


def wait_for_keep(game):
    deadline = time.time() + 120
    while True:
        s = state(game)
        if s.get("inKeep") == "true":
            return s
        if time.time() > deadline:
            raise TimeoutError("never reached the keep")
        time.sleep(0.2)


CAMPAIGN_POLICIES = ("newbie", "typical", "army", "lord")


def campaign_run(plan, policy, profile, difficulty=None):
    """One level from raid 1 to its end, on the gold the game really pays. @return a row for the CSV."""
    game = Game(profile, plan, start_wave=1, difficulty=difficulty)
    rng = random.Random()
    trail = []
    try:
        s = wait_for_keep(game)
        entrance = tuple(int(v) for v in s["entrance"].split(","))
        vault = tuple(int(v) for v in s["vault"].split(","))
        rows = floor_plan(game)
        from_vault, parent = distances(rows, vault)
        ring = sorted((t for t in from_vault if from_vault[t] >= 3), key=lambda t: from_vault[t] + rng.uniform(0, 4))
        route = []
        at = min(from_vault, key=lambda t: max(abs(t[0] - entrance[0]), abs(t[1] - entrance[1])))
        while at in parent:
            route.append(at)
            at = parent[at]
        spots = route[len(route) // 5 + rng.randrange(4)::rng.choice((3, 4))]
        used, tiers, upgrade_spent, trap_spent, traps_built = [], [0] * len(plan["upgrade_costs"]), 0, 0, 0
        main = plan["army"][0]
        kinds = [main, plan["filler"]]
        upgrade_share, trap_share = dict(newbie=(0, 0.15), typical=(0.25, 0.25), army=(0, 0), lord=(1, 0))[policy]
        order = (5, 4) if policy == "lord" else (1, 0, 3, 5, 4, 2)
        if policy in ("newbie", "typical") and plan["casts"]:
            every = plan["cast_every_ticks_plain"] * 2 if policy == "newbie" else plan["cast_every_ticks"]
            game.send(f"autocast {','.join(str(c) for c in plan['casts'])} {every}")
        game.send(f"speed {SPEED}")
        spent = 0
        started = time.time()
        while True:
            s = state(game)
            gold = int(s["gold"])
            earned = gold + spent
            # Upgrades first, up to their share of all gold so far.
            while True:
                options = [i for i in order if tiers[i] < len(plan["upgrade_costs"][i])]
                if not options:
                    break
                choice = min(options, key=lambda i: tiers[i]) if policy != "lord" else options[0]
                cost = plan["upgrade_costs"][choice][tiers[choice]]
                if cost > gold or upgrade_spent + cost > earned * upgrade_share:
                    break
                game.ask(f"upgrade {choice}\nwait 30\necho upgrade-done", "upgrade-done")
                tiers[choice] += 1
                upgrade_spent += cost
                gold -= cost
                spent += cost
            while trap_share and plan["trap_cost"] and traps_built < 16 and gold >= plan["trap_cost"] and trap_spent + plan["trap_cost"] <= earned * trap_share:
                if place(game, plan["trap_kind"], 1, spots, used) == 0:
                    break
                traps_built += 1
                trap_spent += plan["trap_cost"]
                gold -= plan["trap_cost"]
                spent += plan["trap_cost"]
            if policy != "lord":
                used_pop, cap = (int(v) for v in state(game)["pop"].split("/"))
                for kind in kinds:
                    while gold >= kind["cost"] and used_pop + kind["pop"] <= cap:
                        if place(game, kind["index"], 1, ring, used) == 0:
                            break
                        gold -= kind["cost"]
                        spent += kind["cost"]
                        used_pop += kind["pop"]
            game.ask("raid\necho raid-sent", "raid-sent")
            raid_started = time.time()
            ended = None
            while time.time() - raid_started < GAME_SECONDS_LIMIT:
                if game.process.poll() is not None:
                    raise RuntimeError(f"game exited with {game.process.returncode}")
                for line in game.read():
                    if line.startswith("raidend"):
                        ended = dict(re.findall(r"(\w+)=(\S+)", line))
                if ended:
                    break
                time.sleep(0.05)
            if not ended:
                return dict(level=plan["level"], policy=policy, result="timeout", raids=len(trail), trail=" ".join(trail), seconds=int(time.time() - started))
            trail.append(f"{ended['wave']}:{gold}g,{int(ended['ticks']) // 20}s,lord{ended['lordLow']}/{ended['lordStart']},lost{ended['goldLost']}")
            if ended["result"] in ("Victory", "Defeat"):
                return dict(level=plan["level"], policy=policy, result=ended["result"], raids=ended["wave"], trail=" ".join(trail), seconds=int(time.time() - started))
            deadline = time.time() + 20
            while s.get("phase") == "Raid!" and time.time() < deadline:
                s = state(game)
                time.sleep(0.05)
    finally:
        game.stop()


def one_raid(plan, mode, profile, coop_port=0, difficulty=None, masters=1):
    """A co-op raid has more masters casting too, and runs at the normal speed."""
    game = Game(profile, plan, "--keeper-host", coop_port, difficulty=difficulty) if coop_port else Game(profile, plan, difficulty=difficulty)
    joiners = []
    joiner = None
    try:
        s = wait_for_keep(game)
        if coop_port:
            for i in range(masters - 1):
                joiners.append(Game(f"{profile}j{i}", plan, f"--keeper-join 127.0.0.1:{coop_port}", coop_port))
                wait_for_keep(joiners[-1])
            joiner = joiners[0] if joiners else None
            s = state(game)
        entrance = tuple(int(v) for v in s["entrance"].split(","))
        vault = tuple(int(v) for v in s["vault"].split(","))
        game.ask("gold 9999999\nwait 50\necho gold-ready", "gold-ready")
        rows = floor_plan(game)
        from_vault, parent = distances(rows, vault)
        rng = random.Random()
        ring = sorted(from_vault, key=lambda t: from_vault[t] + rng.uniform(0, 4))
        ring = [t for t in ring if from_vault[t] >= 3]
        used = []
        want = built = 0
        for unit in plan["army"]:
            want += unit["count"]
            built += place(game, unit["index"], unit["count"], ring, used)
        traps_built = 0
        if mode in ("full", "traps", "sloppy"):
            # Along the way in, out of the keep-out zones, spaced so a party passes each one.
            route = []
            # The entrance is on the stairs, which aren't floor: start from the floor nearest them.
            at = min(from_vault, key=lambda t: max(abs(t[0] - entrance[0]), abs(t[1] - entrance[1])))
            while at in parent:
                route.append(at)
                at = parent[at]
            spots = route[len(route) // 5 + rng.randrange(4)::rng.choice((3, 4))]
            traps_built = place(game, plan["trap_kind"], plan["trap_count"], spots, used)
        if mode == "full":
            for upgrade, tiers in enumerate(plan["tiers"]):
                for _ in range(tiers):
                    game.ask(f"upgrade {upgrade}\nwait 30\necho upgrade-done", "upgrade-done")
        cast = ""
        if mode in ("full", "spells", "sloppy") and plan["casts"]:
            # Only full buys the mana upgrades, so only full casts at the upgraded rate.
            every = {"full": plan["cast_every_ticks"], "spells": plan["cast_every_ticks_plain"], "sloppy": plan["cast_every_ticks_plain"] * 2}[mode]
            cast = f"autocast {','.join(str(c) for c in plan['casts'])} {every}"
        for other in joiners:
            other.ask(f"fillmana\n{cast}\necho joiner-ready", "joiner-ready")
        if plan.get("extra"):
            game.send(plan["extra"])  # for diagnosis, such as herolog
        speed = "" if coop_port else f"speed {SPEED}"
        game.ask(f"gold {plan['gold_left']}\nwait 100\nfillmana\n{speed}\n{cast}\nraid\necho raid-sent", "raid-sent")
        started = time.time()
        reach = ""
        casts = 0
        while time.time() - started < (GAME_SECONDS_LIMIT * 3 if coop_port else GAME_SECONDS_LIMIT):
            if game.process.poll() is not None:
                raise RuntimeError(f"game exited with {game.process.returncode}")
            for line in game.read():
                if line.startswith("reach"):
                    reach = re.search(r"ticks=(\d+)", line).group(1)
                if line.startswith("autocast"):
                    casts += 1
                if line.startswith("raidend"):
                    row = dict(re.findall(r"(\w+)=(\S+)", line))
                    row.update(level=plan["level"], mode=mode, built=f"{built}/{want}", traps=traps_built, reach=reach, casts=casts)
                    return row
            time.sleep(0.1)
        # A raid that never ends is worth a look: leave what it looked like beside the results.
        game.send("speed 20", "state", "mons", "echo stuck-end")
        time.sleep(5)
        with open(ROOT / "build" / "test-profiles" / "stuck-raids.log", "a") as log:
            log.write(f"== level {plan['level']} {mode}\n" + "\n".join(game.read()) + "\n")
        return dict(level=plan["level"], mode=mode, result="timeout", built=f"{built}/{want}", traps=traps_built, reach=reach, casts=casts)
    finally:
        game.stop()
        for other in joiners:
            other.stop()


FIELDS = ["label", "level", "mode", "result", "ticks", "peakHeroes", "peakPartyLife", "partyLifeLeft", "heroesLeft", "keepStart", "keepEnd", "lordStart", "lordLow", "goldLost", "built", "traps", "reach", "casts", "trapFires", "trapsLeft"]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("plan")
    parser.add_argument("--levels", default="1,5,10,15,20")
    parser.add_argument("--modes", default="full")
    parser.add_argument("--runs", type=int, default=8)
    parser.add_argument("--workers", type=int, default=3)
    parser.add_argument("--out", required=True)
    parser.add_argument("--label", default="")
    parser.add_argument("--coop", type=int, default=0, metavar="MASTERS", help="a host and joiners, on ports of their own")
    parser.add_argument("--campaign", action="store_true", help="play from raid 1 on real gold")
    parser.add_argument("--difficulty", type=int, help="Easy 0, Normal 1, Hard 2")
    args = parser.parse_args()
    for mode in args.modes.split(","):
        if mode not in (CAMPAIGN_POLICIES if args.campaign else MODES):
            parser.error(f"unknown mode {mode}")
    plans = {p["level"]: p for p in json.loads(Path(args.plan).read_text())}
    jobs = [(plans[int(level)], mode, run) for level in args.levels.split(",") for mode in args.modes.split(",") for run in range(args.runs)]
    profiles = collections.deque(f"bal{i}" for i in range(args.workers))
    lock = threading.Lock()
    new_file = not Path(args.out).exists()
    out = open(args.out, "a", newline="")
    writer = csv.DictWriter(out, ["label", "level", "policy", "result", "raids", "seconds", "trail"] if args.campaign else FIELDS, extrasaction="ignore")
    if new_file:
        writer.writeheader()

    def work(job):
        plan, mode, _ = job
        with lock:
            profile = profiles.popleft()
        try:
            try:
                if args.campaign:
                    row = campaign_run(plan, mode, profile, args.difficulty)
                else:
                    row = one_raid(plan, mode, profile, COOP_PORT + int(profile[3:]) if args.coop else 0, args.difficulty, args.coop or 1)
            except (RuntimeError, TimeoutError) as error:
                row = dict(level=plan["level"], mode=mode, policy=mode, result=f"error:{error}".replace(",", ";"))
            row["label"] = args.label
            with lock:
                writer.writerow(row)
                out.flush()
                print(row["level"], mode, row.get("result"), row.get("raids"), row.get("ticks"), row.get("trail"), flush=True)
        finally:
            with lock:
                profiles.append(profile)

    with concurrent.futures.ThreadPoolExecutor(args.workers) as pool:
        list(pool.map(work, jobs))
    out.close()


if __name__ == "__main__":
    main()
