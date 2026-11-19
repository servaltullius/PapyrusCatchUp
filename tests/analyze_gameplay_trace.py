"""Summarize APBGameplayTrace and PresentMon output from a single launch."""

import argparse
import csv
import math
import re
from collections import Counter, defaultdict, deque
from pathlib import Path
from statistics import median


def read_csv(path):
    with path.open(newline="", encoding="utf-8-sig") as stream:
        return list(csv.DictReader(stream))


def percentile(samples, p):
    return sorted(samples)[math.ceil(len(samples) * p) - 1] if samples else None


def show(value):
    return f"{value:.3f}" if value is not None else "n/a"


def parse_log(path, *, allow_incomplete=False):
    log = path.read_text(encoding="utf-8-sig")
    if not allow_incomplete and "gameplay trace complete" not in log:
        raise ValueError("Incomplete trace")
    frequency = int(re.search(r"qpc_hz=(\d+)", log)[1])
    phases = {}
    for line in log.splitlines():
        if "gameplay window" in line:
            fields = dict(re.findall(r"(\w+)=([\w.]+)", line))
            phases[int(fields["trial"])] = {
                "start": int(fields["qpc"]),
                "equip": fields["equip"] == "true",
                "enabled": fields.get("enabled") == "true",
            }
        elif "queue headroom samples=" in line:
            phases[next(reversed(phases))]["headroom"] = {
                name: int(value) for name, value in re.findall(r"(\w+)=(\d+)", line)
            }
        elif "gameplay end" in line:
            trial, end = map(int, re.search(r"trial=(\d+) qpc=(\d+)", line).groups())
            phases[trial]["end"] = end
        elif "gameplay result" in line:
            trial = int(re.search(r"trial=(\d+)", line)[1])
            phases[trial]["probes"] = list(map(float, line.split("probes=")[1].split(",")))
            phases[trial]["promoted"] = int(re.search(r"promoted=(\d+)", line)[1]) if "promoted=" in line else 0
    return phases, 1000 / frequency


def stack_metrics(events, ms):
    events.sort(key=lambda row: int(row["qpc"]))
    signal_events = [row for row in events if row["kind"] in ("E", "U")]
    signals = [int(row["qpc"]) for row in signal_events]
    requests = [row for row in events if row["kind"] in ("e", "u")]
    matched = sum(
        action["kind"].upper() == signal["kind"] and 0 <= (seen - int(action["qpc"])) * ms < 250
        for action, signal, seen in zip(requests, signal_events, signals)
    )
    roots = {}
    ends = []
    equipment_stacks = set()
    totals, waits, equipment_waits = Counter(), defaultdict(list), defaultdict(list)
    waiting = defaultdict(deque)
    for row in events:
        kind, stack, depth = row["kind"], row["stack"], row["depth"]
        qpc = int(row["qpc"])
        method = row["name3"].lower()
        name = f'{row["name1"].lower()}.{method}'
        key = stack, depth
        root = method in ("onobjectequipped", "onobjectunequipped")
        if kind == "Q":
            waiting[key].append(qpc)
        elif kind == "P":
            if root and signals and qpc >= signals[0]:
                roots[key] = name
                equipment_stacks.add(stack)
            totals[name] += 1
            if waiting[key]:
                wait = (qpc - waiting[key].popleft()) * ms
                waits[name].append(wait)
                if stack in equipment_stacks:
                    equipment_waits[name].append(wait)
        elif kind == "O" and root and roots.get(key) == name:
            ends.append(qpc)
            del roots[key]
    drain = max(0, max(ends) - signals[-1]) * ms if ends and not roots else None
    return len(signals), matched, len(ends), len(roots), drain, totals, waits, equipment_waits


def frame_metrics(presents, engine, phase, ms):
    lo, hi = phase["start"], phase["end"]
    swaps = defaultdict(list)
    for row in presents:
        qpc = int(row["QPCTime"])
        if lo <= qpc < hi:
            swaps[row["SwapChainAddress"]].append(qpc)
    engine_frames = [frame for qpc, frame in engine if lo <= qpc < hi]
    count = engine_frames[-1] - engine_frames[0] + 1
    times = sorted(max(swaps.values(), key=len))
    if abs(len(times) - count) <= 2:
        stride = 1
    elif abs(len(times) - 2 * count) <= 4:
        # Verify that the extra submissions occur in close pairs.
        gaps = [(b - a) * ms for a, b in zip(times, times[1:])]
        if not any(sum(gap < 5 for gap in gaps[offset::2]) >= len(gaps[offset::2]) * .95 for offset in (0, 1)):
            raise ValueError("PresentMon rows do not form pairs")
        stride = 2
    else:
        raise ValueError(f"PresentMon and engine frame counts disagree: {len(times)} vs {count}")
    periods = [[(times[i] - times[i - stride]) * ms for i in range(offset + stride, len(times), stride)]
               for offset in range(stride)]
    return count, len(times), periods


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    path = args.capture
    phases, ms = parse_log(path / "vm.log")
    events = defaultdict(list)
    for row in read_csv(path / "vm-events.csv"):
        events[int(row["trial"])].append(row)
    presents = read_csv(path / "present.csv")
    engine = [(int(row["qpc"]), int(row["frame"])) for row in read_csv(path / "engine-render.csv")]
    by_mode = defaultdict(lambda: defaultdict(list))
    compare_modes = all(phase["equip"] for phase in phases.values())
    headroom_by_mode = defaultdict(Counter)
    print("trial mode signals matched completed open drain_ms probes p95_ms engine presents frame_p99_ms alt_p99_ms promoted")
    for trial, phase in phases.items():
        signals, matched, completed, opened, drain, totals, waits, equip_waits = stack_metrics(events[trial], ms)
        count, presents_count, periods = frame_metrics(presents, engine, phase, ms)
        probes = phase["probes"]
        mode = ("on" if phase["enabled"] else "off") if compare_modes else (
            "equip" if phase["equip"] else "idle")
        group = by_mode[mode]
        group["probes"].extend(probes)
        if phase["equip"] and max(probes) < 200:
            # Probes are 250 ms apart. With this margin, the first 12
            # callbacks are the probes paired with the 12 equip actions.
            group["action_probes"].extend(probes[:12])
        group["frames"].extend(periods[0])
        if len(periods) == 2:
            group["alt_frames"].extend(periods[1])
        if drain is not None:
            group["drain"].append(drain)
        print(trial, mode, signals, matched, completed, opened, show(drain), len(probes), show(percentile(probes, .95)),
              count, presents_count, show(percentile(periods[0], .99)),
              show(percentile(periods[-1], .99)), phase["promoted"])
        if "headroom" in phase:
            headroom_by_mode[mode].update({
                name: count for name, count in phase["headroom"].items() if not name.endswith("_thread")
            })
        for method in ("form.gettype", "form.getformid", "game.getplayer", "form.haskeyword"):
            group[f"{method}:executions"].append(totals[method])
            group[f"{method}:all_queues"].extend(waits[method])
            group[f"{method}:equip_queues"].extend(equip_waits[method])
    for mode, data in by_mode.items():
        print(f"{mode}: probes={len(data['probes'])} probe_p95_ms={show(percentile(data['probes'], .95))} "
              f"action_probes={len(data['action_probes'])} action_probe_p95_ms={show(percentile(data['action_probes'], .95))} "
              f"drain_median_ms={show(median(data['drain']) if data['drain'] else None)} "
              f"frame_median_ms={show(median(data['frames']))} frame_p99_ms={show(percentile(data['frames'], .99))} "
              f"alt_frame_p99_ms={show(percentile(data['alt_frames'], .99))}")
        for method in ("form.gettype", "form.getformid", "game.getplayer", "form.haskeyword"):
            all_queues = data[f"{method}:all_queues"]
            equip_queues = data[f"{method}:equip_queues"]
            print(f"  {method}: executions={sum(data[f'{method}:executions'])} queued={len(all_queues)} "
                  f"equip_queued={len(equip_queues)} equip_queue_p95_ms={show(percentile(equip_queues, .95))} "
                  f"equip_queue_total_ms={sum(equip_queues):.3f}")
        if mode in headroom_by_mode:
            print("  queue headroom", " ".join(f"{name}={value}" for name, value in headroom_by_mode[mode].items()))


if __name__ == "__main__":
    main()
