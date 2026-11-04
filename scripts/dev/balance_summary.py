#!/usr/bin/env python3
"""Sums up balance_raids.py's CSV files: wins, life removed, raid length and how often the Lord was hurt."""
import collections
import csv
import statistics
import sys


def main():
    groups = collections.defaultdict(list)
    campaign = collections.defaultdict(list)
    for path in sys.argv[1:]:
        with open(path, newline="") as f:
            for row in csv.DictReader(f):
                if "policy" in row:  # --campaign: whole levels, not single raids
                    campaign[(row["label"], int(row["level"]), row["policy"])].append(row)
                else:
                    groups[(row["label"], int(row["level"]), row["mode"])].append(row)
    if campaign:
        print("| Label | Lvl | Policy | Wins | Raid reached |")
        print("|---|---|---|---|---|")
        for key in sorted(campaign):
            rows = campaign[key]
            wins = sum(r["result"] == "Victory" for r in rows)
            print(f"| {key[0]} | {key[1]} | {key[2]} | {wins}/{len(rows)} | {' '.join(r['raids'] for r in rows)} |")
    if not groups:
        return
    print("| Label | Lvl | Mode | Wins | Other | Life removed, median | Seconds, median (range) | Lord life left, median | Monsters left, median | Heroes left, median |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for key in sorted(groups):
        rows = groups[key]
        done = [r for r in rows if r["result"] in ("Victory", "Defeat")]
        wins = sum(r["result"] == "Victory" for r in rows)
        other = len(rows) - len(done)
        if not done:
            print(f"| {key[0]} | {key[1]} | {key[2]} | 0/{len(rows)} | {other} | | | | | |")
            continue
        removed = [1 - int(r["partyLifeLeft"]) / max(int(r["peakPartyLife"]), 1) for r in done]
        seconds = [int(r["ticks"]) / 20 for r in done]
        lord = [int(r["lordLow"]) / max(int(r["lordStart"]), 1) for r in done]
        print(f"| {key[0]} | {key[1]} | {key[2]} | {wins}/{len(rows)} | {other} | {statistics.median(removed) * 100:.0f}% | {statistics.median(seconds):.0f} ({min(seconds):.0f}-{max(seconds):.0f}) | {statistics.median(lord) * 100:.0f}% | {statistics.median(int(r['keepEnd']) for r in done):.0f} of {statistics.median(int(r['keepStart']) for r in done):.0f} | {statistics.median(int(r['heroesLeft']) for r in done):.0f} |")


if __name__ == "__main__":
    main()
