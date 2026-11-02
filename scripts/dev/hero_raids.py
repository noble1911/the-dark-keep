#!/usr/bin/env python3
"""Watches heroes in real raids: balance_raids.py's fields and armies, plus each hero's track and the event log.

    python scripts/dev/hero_raids.py plan.json --levels 1,10,20 --modes army --runs 8 --out dir [--exe-dir DIR]
    python scripts/dev/hero_raids.py plan.json --levels 1 --modes none --lord-hp 60000 --seconds 120 --out dir
    python scripts/dev/hero_raids.py --report dir [dir2 ...]

Mode `none` fields no army and no traps. Each run's raw log lands in <out>/<level>-<mode>-<run>.log, and
--report turns them into what a watcher would complain about: heroes who stand idle, stand in a hazard,
pace about, loot, escape or run. Runs differ in where the army stands (see balance_raids.py).
"""
import argparse
import collections
import concurrent.futures
import csv
import json
import re
import statistics
import threading
from pathlib import Path

import balance_raids as br

LOG_EVERY_TICKS = 10
SECONDS_PER_SAMPLE = LOG_EVERY_TICKS / 20
IDLE_SECONDS = 5  # standing on one tile this long, not fighting, is what a watcher would call stuck
PACING_SECONDS = 15  # in a tile box this small for this long, walking, is pacing
PACING_BOX = 3


class LoggedGame(br.Game):
    log_path = {}
    extras = ()

    def __init__(self, profile, plan, extra=()):
        super().__init__(profile, plan, tuple(extra) + tuple(LoggedGame.extras))
        self.log = open(LoggedGame.log_path[profile], "w", encoding="utf-8")

    def send(self, *lines):
        super().send(*(line.replace("\nraid\n", f"\nherolog {LOG_EVERY_TICKS}\nraid\n") for line in lines))

    def read(self):
        lines = super().read()
        for line in lines:
            if line.startswith(("h ", "msg ", "raidend", "reach", "autocast")):
                self.log.write(line + "\n")
        self.log.flush()
        return lines

    def stop(self):
        super().stop()
        self.log.close()


def run_jobs(args):
    plans = {p["level"]: p for p in json.loads(Path(args.plan).read_text())}
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    br.Game = LoggedGame
    if args.exe_dir:
        br.EXE_DIR = Path(args.exe_dir)
        br.EXE = br.EXE_DIR / "devilutionx.exe"
    br.GAME_SECONDS_LIMIT = args.seconds
    LoggedGame.extras = ["--keeper-lord-hp", str(args.lord_hp)] if args.lord_hp else []
    jobs = [(int(level), mode, run) for level in args.levels.split(",") for mode in args.modes.split(",") for run in range(args.first_run, args.first_run + args.runs)]
    profiles = collections.deque(f"{args.profile}{i}" for i in range(args.workers))
    lock = threading.Lock()
    csv_path = out_dir / "raids.csv"
    new_file = not csv_path.exists()
    out = open(csv_path, "a", newline="")
    writer = csv.DictWriter(out, br.FIELDS, extrasaction="ignore")
    if new_file:
        writer.writeheader()

    def work(job):
        level, mode, run = job
        with lock:
            profile = profiles.popleft()
            LoggedGame.log_path[profile] = out_dir / f"{level}-{mode}-{run}.log"
        plan = dict(plans[level])
        if mode == "none":
            plan["army"] = []
        if args.wave:
            plan["wave"] = args.wave
        try:
            try:
                row = br.one_raid(plan, "army" if mode == "none" else mode, profile)
                row["mode"] = mode
            except (RuntimeError, TimeoutError) as error:
                row = dict(level=level, mode=mode, result=f"error:{error}".replace(",", ";"))
            row["label"] = args.label
            with lock:
                writer.writerow(row)
                out.flush()
                print(level, mode, run, row.get("result"), row.get("ticks"), row.get("peakPartyLife"), row.get("partyLifeLeft"), row.get("lordLow"), row.get("goldLost"), flush=True)
        finally:
            with lock:
                profiles.append(profile)

    with concurrent.futures.ThreadPoolExecutor(args.workers) as pool:
        list(pool.map(work, jobs))
    out.close()


HERO_LINE = re.compile(r"^h t=(\d+) who=(\S+) cls=(\S+) at=(\d+),(\d+) hp=(-?\d+)/(\d+) mode=(\d+) carried=(-?\d+) fleeing=(\S+) hazard=(\S+) act=(-?\d+)")
EVENTS = (("looted", "looted"), ("escaped_gold", "escaped with"), ("portal", "through the portal"), ("fled", "fled|flees"), ("slain", "been slain|Recovered"), ("drops", "drops"))


def analyse(path):
    tracks = collections.defaultdict(list)
    counts = collections.Counter()
    for line in open(path, encoding="utf-8"):
        match = HERO_LINE.match(line)
        if match:
            t, who, cls, x, y, hp, _, mode, carried, fleeing, hazard, act = match.groups()
            tracks[(who, cls)].append((int(t), (int(x), int(y)), int(hp), int(mode), int(carried), fleeing in ("true", "1"), hazard in ("true", "1"), int(act)))
        elif line.startswith("msg "):
            text = line.split(" ", 2)[2]
            for key, pattern in EVENTS:
                if re.search(pattern, text):
                    counts[key] += 1
    result = collections.defaultdict(collections.Counter)
    for (_, cls), samples in tracks.items():
        stat = result[cls]
        stat["heroes"] += 1
        stat["hazard"] += sum(s[6] for s in samples)
        i = 0
        worst = 0
        while i < len(samples):
            j = i
            # Same tile, standing, not told to fight: a bot's action is ACTION_NONE (-1), a monster hero has no enemy (0).
            while j + 1 < len(samples) and samples[j + 1][1] == samples[i][1] and samples[j + 1][7] <= 0 and samples[j + 1][3] == 0:
                j += 1
            idle = (j - i) * SECONDS_PER_SAMPLE
            if idle >= IDLE_SECONDS:
                stat["idle_spells"] += 1
                stat["idle_seconds"] += idle
            worst = max(worst, idle)
            i = j + 1
        stat["worst_idle"] = max(stat["worst_idle"], worst)
        window = int(PACING_SECONDS / SECONDS_PER_SAMPLE)
        k = 0
        while k + window < len(samples):
            box = samples[k:k + window]
            xs = [s[1][0] for s in box]
            ys = [s[1][1] for s in box]
            moved = sum(a[1] != b[1] for a, b in zip(box, box[1:]))
            if max(xs) - min(xs) <= PACING_BOX and max(ys) - min(ys) <= PACING_BOX and moved >= window // 3 and all(s[7] <= 0 for s in box):
                stat["pacing"] += 1
                k += window
            else:
                k += 1
        stat["lived_seconds"] += int((samples[-1][0] - samples[0][0]) / 20)
    return result, counts


def report(dirs):
    for directory in dirs:
        directory = Path(directory)
        print(f"\n## {directory}")
        rows = list(csv.DictReader(open(directory / "raids.csv"))) if (directory / "raids.csv").exists() else []
        groups = collections.defaultdict(list)
        for row in rows:
            groups[(int(row["level"]), row["mode"])].append(row)
        print("| Lvl | Mode | Runs | Held | Timeout | Life removed | Seconds | Lord left | Monsters left | Gold lost |")
        print("|---|---|---|---|---|---|---|---|---|---|")
        for key in sorted(groups):
            rs = groups[key]
            done = [r for r in rs if r.get("ticks")]
            held = sum(r["result"] != "Defeat" for r in done)
            timeouts = sum(r["result"] == "timeout" for r in rs)
            if not done:
                print(f"| {key[0]} | {key[1]} | {len(rs)} | 0 | {timeouts} | | | | | |")
                continue
            removed = statistics.median(1 - int(r["partyLifeLeft"]) / max(int(r["peakPartyLife"]), 1) for r in done)
            print(f"| {key[0]} | {key[1]} | {len(rs)} | {held} | {timeouts} | {removed * 100:.0f}% | {statistics.median(int(r['ticks']) / 20 for r in done):.0f} | {statistics.median(int(r['lordLow']) / max(int(r['lordStart']), 1) for r in done) * 100:.0f}% | {statistics.median(int(r['keepEnd']) for r in done):.0f}/{statistics.median(int(r['keepStart']) for r in done):.0f} | {statistics.median(int(r['goldLost']) for r in done):.0f} |")
        totals = collections.defaultdict(collections.Counter)
        events = collections.defaultdict(collections.Counter)
        for log in sorted(directory.glob("*.log")):
            level_mode = "-".join(log.stem.split("-")[:2])
            result, counts = analyse(log)
            events[level_mode].update(counts)
            events[level_mode]["runs"] += 1
            for cls, stat in result.items():
                for k, v in stat.items():
                    if k == "worst_idle":
                        totals[(level_mode, cls)][k] = max(totals[(level_mode, cls)][k], v)
                    else:
                        totals[(level_mode, cls)][k] += v
        print("\n| Level-mode | Class | Heroes | Lived s (avg) | Idle spells | Idle s | Worst idle s | Pacing | Hazard s |")
        print("|---|---|---|---|---|---|---|---|---|")
        for key in sorted(totals):
            stat = totals[key]
            print(f"| {key[0]} | {key[1]} | {stat['heroes']} | {stat['lived_seconds'] / max(stat['heroes'], 1):.0f} | {stat['idle_spells']} | {stat['idle_seconds']:.0f} | {stat['worst_idle']:.0f} | {stat['pacing']} | {stat['hazard'] * SECONDS_PER_SAMPLE:.1f} |")
        print("\n| Level-mode | Runs | Looted | Escaped with gold | Portal | Fled | Slain | Dropped loot |")
        print("|---|---|---|---|---|---|---|---|")
        for key in sorted(events):
            c = events[key]
            print(f"| {key} | {c['runs']} | {c['looted']} | {c['escaped_gold']} | {c['portal']} | {c['fled']} | {c['slain']} | {c['drops']} |")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("plan", nargs="?")
    parser.add_argument("--levels", default="1,10,20")
    parser.add_argument("--modes", default="army")
    parser.add_argument("--runs", type=int, default=8)
    parser.add_argument("--first-run", type=int, default=0)
    parser.add_argument("--workers", type=int, default=3)
    parser.add_argument("--out")
    parser.add_argument("--label", default="")
    parser.add_argument("--profile", default="hero")
    parser.add_argument("--exe-dir")
    parser.add_argument("--lord-hp", type=int, default=0)
    parser.add_argument("--wave", type=int, default=0)
    parser.add_argument("--seconds", type=int, default=600, help="real seconds before a raid counts as a timeout")
    parser.add_argument("--report", nargs="+")
    args = parser.parse_args()
    if args.report:
        report(args.report)
    else:
        run_jobs(args)


if __name__ == "__main__":
    main()
