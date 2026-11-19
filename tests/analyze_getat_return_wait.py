"""Partition DynDOLOD GetAt return latency by observed lock ownership."""

import argparse
from collections import Counter, defaultdict, deque
from math import isfinite
from pathlib import Path
from statistics import median
from struct import pack, unpack

from analyze_continuation_ab import inspect, trial_events
from analyze_gameplay_trace import frame_metrics, parse_log, percentile, read_csv, show


DYNDOLOD = "sheson_dyndolod_objectenabler"


def returns(rows, stack):
    start = None
    for row in rows:
        if row["stack"] != stack or row["depth"] != "1":
            continue
        time = int(row["qpc"])
        kind = row["kind"]
        get_at = row["name1"].lower() == "formlist" and row["name3"].lower() == "getat"
        if kind == "N" and get_at:
            if start is not None:
                raise ValueError("GetAt returned before its prior VM frame completed")
            start, misses, success = time, [], None
        elif start is not None:
            if kind == "L":
                misses.append((time, row["detail"]))
            elif kind == "A":
                success = time, row["detail"]
            elif kind == "O" and get_at:
                yield start, time, misses, success
                start = None


def partition(transitions, start, end, target, roots, misses):
    durations = Counter(unknown=end - start)
    gaps = defaultdict(list)
    holders = defaultdict(list)
    for (begin, event, owner), (finish, next_event, next_owner) in zip(transitions, transitions[1:]):
        lo, hi = max(start, begin), min(end, finish)
        if lo >= hi:
            continue
        if event == "G" and next_event in {"X", "Z"} and owner == next_owner:
            category = "own" if owner == target else "other_dyn" if roots.get(owner) == DYNDOLOD else "other"
            if category == "other_dyn":
                holders[owner].append((lo, hi))
        elif event == "X" and next_event == "G" and not any(begin < miss < finish for miss, _ in misses):
            category = "free_to_target" if next_owner == target else "free_to_other"
            gaps[category].append((lo, hi))
        else:
            continue
        durations[category] += hi - lo
        durations["unknown"] -= hi - lo
    return durations, gaps, holders


def holder_native_spans(rows, holders):
    queued = defaultdict(deque)
    for row in rows:
        stack, kind = row["stack"], row["kind"]
        if stack not in holders or row["depth"] != "1":
            continue
        time = int(row["qpc"])
        if kind == "Q":
            queued[stack].append(row)
        elif kind == "P" and queued[stack]:
            queued_row = queued[stack].popleft()
            start = int(queued_row["qpc"])
            overlap = sum(max(0, min(time, hi) - max(start, lo)) for lo, hi in holders[stack])
            if overlap:
                yield queued_row, row, overlap


def holder_native_queue(rows, holders):
    duration = Counter()
    for _, pushed, overlap in holder_native_spans(rows, holders):
        duration[f'{pushed["name1"]}.{pushed["name3"]}'] += overlap
    return duration


def inspect_trial(rows, milliseconds):
    result = inspect(rows, milliseconds)
    stack = result["dyn_tail"]
    if not stack or result["open"]:
        raise ValueError("Missing completed DynDOLOD tail")
    roots = {row["stack"]: row["name1"].lower() for row in rows if row["kind"] == "P" and row["depth"] == "0"}
    waits = list(returns(rows, stack))
    tokens = {token for _, _, misses, _ in waits for _, token in misses}
    transitions = defaultdict(list)
    all_misses = defaultdict(list)
    for row in rows:
        kind, token = row["kind"], row["detail"]
        if token in tokens and kind in {"G", "X", "Z", "B"}:
            transitions[token].append((int(row["qpc"]), kind, row["stack"]))
        elif token in tokens and kind == "L":
            all_misses[token].append((int(row["qpc"]), token))

    durations = Counter()
    gaps = defaultdict(list)
    holders = defaultdict(list)
    failures = 0
    for start, end, misses, success in waits:
        if not misses:
            durations["no_miss"] += end - start
            continue
        failures += len(misses)
        token = misses[0][1]
        if any(other != token for _, other in misses) or not success or success[1] != token or success[0] < misses[-1][0]:
            durations["unknown"] += end - start
            continue
        values, intervals, owners = partition(transitions[token], start, end, stack, roots, all_misses[token])
        durations.update(values)
        for label, samples in intervals.items():
            gaps[label].extend(samples)
        for owner, spans in owners.items():
            holders[owner].extend(spans)
    if abs(sum(durations.values()) * milliseconds - result["at_return_wait"]) > .001:
        raise ValueError("Lock partition does not account for every GetAt return tick")
    return result, len(waits), failures, durations, gaps, holder_native_queue(rows, holders), holders


def retry_passes(rows):
    active, before, after = {}, {}, {}
    for row in rows:
        kind, thread = row["kind"], row["thread"]
        if kind == "J":
            before[thread] = row
        elif kind == "S" and thread in before:
            budget, = unpack("<f", pack("<I", int(row["detail"])))
            active[thread] = int(row["qpc"]), budget, before.pop(thread)
        elif kind == "s" and thread in active:
            start, budget, pre = active.pop(thread)
            after[thread] = start, int(row["qpc"]), budget, pre
        elif kind == "j" and thread in after:
            yield (*after.pop(thread), row)


def retry_opportunities(rows, target, intervals, ms, retaken=()):
    passes = list(retry_passes(rows))
    attempts = [int(row["qpc"]) for row in rows if row["stack"] == target and row["kind"] in {"A", "L"}]
    acquiring_threads = defaultdict(set)
    for row in rows:
        if row["stack"] == target and row["kind"] == "G":
            acquiring_threads[int(row["qpc"])].add(row["thread"])
    counts = Counter(passes=len(passes), unpaired_starts=sum(row["kind"] == "S" for row in rows) - len(passes))
    for release, acquire in intervals:
        if (acquire - release) * ms <= 5:
            continue
        counts["long_final_gaps"] += 1
        if any(start < acquire <= end and after["thread"] in acquiring_threads[acquire] for start, end, _, _, after in passes):
            counts["acquired_during_stock_retry"] += 1
        for start, end, budget, before, after in passes:
            if not release < end < acquire:
                continue
            counts["passes_ended_while_free"] += 1
            # The engine swaps its queues once. Entries already put into the next
            # queue are not revisited by increasing the current pass's budget.
            if (after["stack"] == target and 0 < int(after["depth"]) <= 128 and int(after["detail"]) == 0 and
                    int(before["depth"]) == 0 and (int(before["detail"]) & 0xFFFFFFFF) == int(target) and
                    isfinite(budget) and 0 < budget < (end - start) * ms - .02 and
                    not any(start <= time <= end for time in attempts)):
                counts["ordered_budget_candidate"] += 1
    for release, acquire in retaken:
        if (acquire - release) * ms <= 5:
            continue
        counts["long_retaken_gaps"] += 1
        if any(release < end < acquire for _, end, *_ in passes):
            counts["retaken_pass_ended_while_free"] += 1
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path, nargs="+")
    args = parser.parse_args()
    totals, intervals = defaultdict(list), defaultdict(list)
    holder_shares, holder_methods = [], Counter()
    for capture in args.capture:
        phases, ms = parse_log(capture / "vm.log", allow_incomplete=True)
        finished = {trial for trial, phase in phases.items() if "probes" in phase and "end" in phase}
        observation = Counter()
        probes, frame_periods, drains, totals_with_load = [], [], [], []
        print(capture.name)
        for trial, rows in trial_events(capture / "vm-events.csv"):
            if trial not in finished or phases[trial]["enabled"]:
                continue
            data, calls, failures, durations, gaps, holder_waits, _ = inspect_trial(rows, ms)
            stack = data["dyn_tail"]
            durations["total"] = sum(durations.values())
            durations["free_total"] = durations["free_to_other"] + durations["free_to_target"]
            print(f"  off trial={trial} stack={stack} getat_calls={calls} failed_acquires={failures} "
                  + " ".join(f"{name}_ms={durations[name] * ms:.3f}" for name in (
                      "total", "other_dyn", "other", "free_total", "free_to_other", "free_to_target", "own", "no_miss", "unknown")))
            for name in ("total", "other_dyn", "other", "free_total", "free_to_other", "free_to_target", "own", "no_miss", "unknown"):
                totals[name].append(durations[name] * ms)
            for name, values in gaps.items():
                intervals[name].extend((end - start) * ms for start, end in values)
            if durations["other_dyn"]:
                holder_shares.append(sum(holder_waits.values()) / durations["other_dyn"])
            holder_methods.update({method: value * ms for method, value in holder_waits.items()})
            if any(row["kind"] == "J" for row in rows):
                if data["promoted"] or data["counts"]["h"]:
                    raise ValueError("Stock observation executed an experimental continuation")
                if not probes:
                    presents = read_csv(capture / "present.csv")
                    engine = [(int(row["qpc"]), int(row["frame"])) for row in read_csv(capture / "engine-render.csv")]
                phase = phases[trial]
                steady = {**phase, "start": phase["end"] - round(2000 / ms)}
                _, _, periods = frame_metrics(presents, engine, steady, ms)
                frame_periods.extend(periods[0])
                probes.extend(phase["probes"])
                drains.append(data["drain"])
                totals_with_load.append(data["total"])
                observation.update(retry_opportunities(rows, stack, gaps["free_to_target"], ms, gaps["free_to_other"]))
        if observation:
            print("  stock_retry", " ".join(f"{key}={observation[key]}" for key in (
                "passes", "unpaired_starts", "long_final_gaps", "acquired_during_stock_retry", "passes_ended_while_free",
                "ordered_budget_candidate", "long_retaken_gaps", "retaken_pass_ended_while_free")))
            print(f"  stock_metrics response_p95_ms={show(percentile(probes, .95))} probes={len(probes)} "
                  f"drain_median_ms={show(median(drains))} total_median_ms={show(median(totals_with_load))} "
                  f"frame_p50_ms={show(median(frame_periods))} frame_p99_ms={show(percentile(frame_periods, .99))}")
    print("off tails", len(totals["total"]), "median ms")
    for name, values in totals.items():
        print(f"  {name} median={show(median(values))} min={show(min(values))} max={show(max(values))}")
    for name, values in intervals.items():
        print(f"  {name} gaps={len(values)} p50_ms={show(percentile(values, .5))} p95_ms={show(percentile(values, .95))} "
              f"max_ms={show(max(values))} over_5ms={sum(value > 5 for value in values)}")
    if holder_shares:
        print(f"  other_dyn_hold_inside_native_queue_median_percent={median(holder_shares) * 100:.3f} "
              + " ".join(f"{method}_total_ms={ticks:.3f}" for method, ticks in holder_methods.most_common()))


if __name__ == "__main__":
    main()
