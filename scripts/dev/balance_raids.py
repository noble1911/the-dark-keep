#!/usr/bin/env python3
"""Measures real raids for docs/BALANCE.md: fields the model's army at a level's last raid and records how it ends.

    python scripts/dev/balance_model.py --plan plan.json
    python scripts/dev/balance_raids.py plan.json --levels 1,5,10 --modes army,full --runs 8 --out raids.csv

Needs a build with -DKEEPER_DEVCTL=ON, and scripts/dev/testrun.ps1 run once to link the game data. Games are
invisible and silent (SDL's dummy drivers, no console), keep to build/test-profiles, and only the games
started here are ever stopped.

Modes:
    army   the army and the Lord only
    traps  adds the traps
    spells adds the spells, at the mana share in the plan
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
GAME_SECONDS_LIMIT = 900
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
"""


class Game:
    def __init__(self, profile, plan, extra=()):
        self.dir = ROOT / "build" / "test-profiles" / profile
        self.dir.mkdir(parents=True, exist_ok=True)
        (self.dir / "diablo.ini").write_text(INI)
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
                "--keeper-autostart", "--keeper-level", str(plan["level"]), "--keeper-start-wave", str(plan["wave"]), "--keeper-rank", "20", *extra]
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
    deadline = time.time() + 30
    while time.time() < deadline:
        lines = game.read()
        rows += [line for line in lines if len(line) > 50 and set(line) <= set("#.")]
        if any(token in line for line in lines):
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


def place(game, kind, count, candidates, used, spacing_gap=0.03):
    """Builds `count` of a kind on the candidates, in order, moving on past refused tiles. @return how many stood."""
    built = 0
    while built < count and candidates:
        tile = candidates.pop(0)
        if any(abs(tile[0] - u[0]) <= 1 and abs(tile[1] - u[1]) <= 1 for u in used):
            continue
        token = f"b{time.time()}"
        lines = game.ask(f"build {kind} {tile[0]} {tile[1]}\nwait 25\necho {token}", token)
        if any("refused" in line for line in lines):
            continue
        used.append(tile)
        built += 1
    return built


def one_raid(plan, mode, profile, extra_label=""):
    game = Game(profile, plan)
    try:
        deadline = time.time() + 120
        while True:
            s = state(game)
            if s.get("inKeep") == "true" or s.get("inKeep") == "1":
                break
            if time.time() > deadline:
                raise TimeoutError("never reached the keep")
            time.sleep(0.2)
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
        if mode in ("full", "traps"):
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
        if mode in ("full", "spells") and plan["casts"]:
            cast = f"autocast {','.join(str(c) for c in plan['casts'])} {plan['cast_every_ticks']}"
        game.ask(f"gold {plan['gold_left']}\nwait 100\nfillmana\nspeed {SPEED}\n{cast}\nraid\necho raid-sent", "raid-sent")
        started = time.time()
        reach = ""
        casts = 0
        while time.time() - started < GAME_SECONDS_LIMIT:
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
        return dict(level=plan["level"], mode=mode, result="timeout", built=f"{built}/{want}", traps=traps_built, reach=reach, casts=casts)
    finally:
        game.stop()


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
    args = parser.parse_args()
    plans = {p["level"]: p for p in json.loads(Path(args.plan).read_text())}
    jobs = [(plans[int(level)], mode, run) for level in args.levels.split(",") for mode in args.modes.split(",") for run in range(args.runs)]
    profiles = collections.deque(f"bal{i}" for i in range(args.workers))
    lock = threading.Lock()
    new_file = not Path(args.out).exists()
    out = open(args.out, "a", newline="")
    writer = csv.DictWriter(out, FIELDS, extrasaction="ignore")
    if new_file:
        writer.writeheader()

    def work(job):
        plan, mode, _ = job
        with lock:
            profile = profiles.popleft()
        try:
            try:
                row = one_raid(plan, mode, profile)
            except (RuntimeError, TimeoutError) as error:
                row = dict(level=plan["level"], mode=mode, result=f"error:{error}".replace(",", ";"))
            row["label"] = args.label
            with lock:
                writer.writerow(row)
                out.flush()
                print(row["level"], row["mode"], row.get("result"), row.get("ticks"), row.get("peakPartyLife"), row.get("partyLifeLeft"), row.get("lordLow"), row.get("built"), flush=True)
        finally:
            with lock:
                profiles.append(profile)

    with concurrent.futures.ThreadPoolExecutor(args.workers) as pool:
        list(pool.map(work, jobs))
    out.close()


if __name__ == "__main__":
    main()
