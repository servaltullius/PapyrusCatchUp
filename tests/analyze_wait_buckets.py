"""Queue waits (registration to function entry) of the equip-handler stacks in a catch-up capture,
bucketed by frames: under half a frame (ran in the same frame), about one frame, over 1.5 frames.

  python tests/analyze_wait_buckets.py <capture> <frame_ms>
"""
import csv
import sys
from collections import defaultdict, deque
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_gameplay_trace import parse_log  # noqa: E402

cap = Path(sys.argv[1])
phases, ms = parse_log(cap / "vm.log")
rows = list(csv.DictReader(open(cap / "vm-events.csv", encoding="utf-8-sig")))
by = defaultdict(list)
for r in rows:
    by[int(r["trial"])].append(r)
print("trial mode handlers frame_ms waits same_frame one_frame two_plus same% two_plus%")
for t, phase in phases.items():
    ev = sorted(by[t], key=lambda r: int(r["qpc"]))
    first = min(int(r["qpc"]) for r in ev if r["kind"] in ("E", "U"))
    equip = set()
    for r in ev:
        if r["kind"] == "P" and r["depth"] == "0" and r["name3"].lower() in ("onobjectequipped", "onobjectunequipped") and int(r["qpc"]) >= first:
            equip.add(r["stack"])
    handlers = 0
    for r in ev:
        if r["kind"] == "O" and r["depth"] == "0" and r["name3"].lower() in ("onobjectequipped", "onobjectunequipped") and int(r["qpc"]) >= first:
            handlers += 1
    waiting = defaultdict(deque)
    waits = []
    for r in ev:
        if r["stack"] not in equip:
            continue
        key = (r["stack"], r["depth"])
        q = int(r["qpc"])
        if r["kind"] == "Q":
            waiting[key].append(q)
        elif r["kind"] == "P" and waiting[key]:
            waits.append((q - waiting[key].popleft()) * ms)
    waits.sort()
    fm = float(sys.argv[2]) if len(sys.argv) > 2 else (waits[len(waits) // 2] if waits else 0.0)
    same = sum(w < 0.5 * fm for w in waits)
    two = sum(w > 1.5 * fm for w in waits)
    print(t, "on" if phase["enabled"] else "off", handlers, f"{fm:.1f}", len(waits), same, len(waits) - same - two, two,
          f"{100 * same / max(1, len(waits)):.1f}", f"{100 * two / max(1, len(waits)):.1f}")
