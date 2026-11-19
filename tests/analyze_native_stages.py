"""Attribute delayed DynDOLOD publications to stock VM stages without changing scheduling."""

import argparse
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
from pathlib import Path
from statistics import median

from analyze_continuation_ab import EXTERIOR, INTERIOR, trial_events
from analyze_gameplay_trace import parse_log, percentile
from analyze_getat_return_wait import holder_native_spans, inspect_trial
from analyze_native_drain import drains_and_enqueues, locate


STAGE_MARKERS = ("S", "s", "g859", "l859", "g860", "l860", "F")
STAGE_NAMES = ("drain_epilogue", "before_retry", "retry_104858", "after_retry", "stage_104859",
               "between_104859_and_104860", "stage_104860", "after_104860")
LATE = {"after_empty_drain", "after_budget_stop"}


def ticks(row):
    return int(row["qpc"])


def cycles(rows, drains):
    markers = defaultdict(list)
    times = defaultdict(list)
    for row in rows:
        if row["kind"] in {"S", "s", "g", "l", "F"}:
            markers[row["thread"]].append(row)
            times[row["thread"]].append(ticks(row))
    windows = []
    for index, (drain, following) in enumerate(zip(drains, drains[1:])):
        boundaries = times[drain.thread]
        first = bisect_left(boundaries, drain.end)
        last = bisect_left(boundaries, following.start)
        stages = markers[drain.thread][first:last]
        order = tuple(row["kind"] + row["depth"] if row["kind"] in {"g", "l"} else row["kind"] for row in stages)
        if order != STAGE_MARKERS:
            raise ValueError(f"Incomplete or misordered stock VM phases at drain {index + 1}: {order}")
        windows.append(stages)
    return windows


def stage_at(drain, stages, following, start, end):
    points = [drain.cutoff, drain.end, *(ticks(row) for row in stages[:-1]), following.start]
    if end < drain.start:
        return "before_previous_drain"
    if drain.start <= start <= end < drain.cutoff:
        return "inside_previous_drain"
    if start < points[0] or end > points[-1]:
        return "outside_cycle"
    for name, lo, hi in zip(STAGE_NAMES, points[:-1], points[1:], strict=True):
        if lo <= start <= end < hi:
            return name
    return "overlaps_stage_boundary"


def return_before_enqueue(rows):
    """Join the immediately preceding depth-1 native return to its next delayed enqueue."""
    pending, completed, matches = {}, {}, {}
    for row in rows:
        if row["depth"] != "1":
            continue
        stack, kind = row["stack"], row["kind"]
        if kind == "N":
            pending[stack] = {"N": row}
            completed.pop(stack, None)
        elif kind in {"A", "R", "T"} and stack in pending:
            pending[stack][kind] = row
        elif kind == "O":
            native = pending.pop(stack, {})
            entered = native.get("N")
            if entered and (entered["name1"], entered["name3"]) == (row["name1"], row["name3"]):
                completed[stack] = {**native, "O": row}
            else:
                completed.pop(stack, None)
        elif kind == "[":
            native = completed.pop(stack, None)
            if native:
                matches[row["thread"], stack, ticks(row)] = native
    return matches


def tasklet_context(marker, native):
    began = int(marker["detail"])
    if not began:
        return "no_matching_VMProcess_scope", None
    if began > ticks(marker):
        raise ValueError("Tasklet start follows its native enqueue")
    returned = native.get("O")
    if not returned:
        return "no_prior_return", began
    if ticks(returned) < began:
        return "started_after_return", began
    if marker["thread"] == returned["thread"]:
        return "return_inside_tasklet", began
    return "return_on_other_thread", began


def head_at_boundary(drain, stages, next_drain, queued, start, end):
    stage_end, head = stages[-2:]
    if end > ticks(stage_end):
        return "published_after_stage_end" if start >= ticks(stage_end) else "publication_overlapped_stage_end"
    if int(head["depth"]) == 0xFFFFFFFF:
        return "overflow"
    if head["stack"] != queued["stack"] or head["depth"] != "1":
        return "not_fifo_head"
    if any(value != drain.frame for value in (stages[2]["stack"], stages[3]["stack"], stages[4]["stack"],
                                               stage_end["stack"], head["detail"])):
        return "head_after_render_frame_advanced"
    if next_drain.frame == drain.frame:
        return "next_stock_drain_same_render_frame"
    if next_drain.pops and next_drain.pops[0][:2] == (queued["stack"], "1"):
        return "same_frame_head_first_next_drain"
    return "head_did_not_stay_first"


def inspect_stages(rows, ms):
    data, _, _, _, _, _, holders = inspect_trial(rows, ms)
    drains, publishes = drains_and_enqueues(rows)
    windows = cycles(rows, drains)
    starts = [phase.start for phase in drains]
    enqueue_markers = {(r["thread"], r["stack"], r["depth"], ticks(r)): r for r in rows if r["kind"] == "["}
    returns = return_before_enqueue(rows)
    counts = Counter()
    sources, return_stages, return_routes, contexts, heads, overlap = (Counter() for _ in range(6))
    durations = defaultdict(list)
    for queued, entered, held in holder_native_spans(rows, holders):
        if (entered["name1"].lower(), entered["name3"].lower()) != ("objectreference", "getworldspace"):
            continue
        classification, _, advanced = locate(queued, entered, drains, publishes)
        if advanced:
            raise ValueError("Unexpected budget extension in stock capture")
        counts[classification] += 1
        if classification not in LATE:
            continue
        key = queued["thread"], queued["stack"], queued["depth"]
        start, end = next((a, b) for a, b in publishes[key] if a <= ticks(queued) <= b)
        index = bisect_right(starts, start) - 1
        if not 0 <= index < len(windows):
            raise ValueError("Late native message has no complete publication cycle")
        drain, next_drain = drains[index:index + 2]
        stages = windows[index]
        name = stage_at(drain, stages, next_drain, start, end)
        origin = "same_thread" if queued["thread"] == drain.thread else "other_thread"
        sources[classification, name, origin] += 1
        overlap[classification, name] += held
        durations["publication_to_next_drain"].append(next_drain.start - end)
        if name == "after_104860":
            durations["stage_104860_end_to_publication"].append(start - ticks(stages[-2]))
        native = returns.get((queued["thread"], queued["stack"], start), {})
        returned = native.get("O")
        method = returned["name3"] if returned else "unmatched"
        accepted = "".join(kind for kind in ("A", "R", "T") if kind in native) or "unobserved"
        return_routes[classification, method, accepted] += 1
        if returned:
            return_stage = stage_at(drain, stages, next_drain, ticks(returned), ticks(returned))
            return_origin = "same_thread" if returned["thread"] == drain.thread else "other_thread"
            return_stages[classification, method, return_stage, return_origin] += 1
            durations["return_to_publication"].append(start - ticks(returned))
        context, began = tasklet_context(enqueue_markers[(*key, start)], native)
        tasklet_stage = stage_at(drain, stages, next_drain, began, began) if began is not None else "unobserved"
        contexts[classification, context, tasklet_stage] += 1
        if began is not None:
            durations["tasklet_start_to_publication"].append(start - began)
            if context == "started_after_return":
                durations["return_to_tasklet_start"].append(began - ticks(returned))
        head = head_at_boundary(drain, stages, next_drain, queued, start, end)
        heads[classification, head] += 1
        if head == "same_frame_head_first_next_drain":
            durations["head_snapshot_to_next_drain"].append(next_drain.start - ticks(stages[-1]))
    return data, counts, sources, return_stages, return_routes, contexts, heads, overlap, durations, len(windows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    path = args.capture
    phases, ms = parse_log(path / "vm.log")
    if "Installed stock post-drain stage and FIFO head observation" not in (path / "vm.log").read_text(encoding="utf-8-sig"):
        raise ValueError("Missing stage observation hook")
    if len(phases) != 4:
        raise ValueError("Expected four stock observation trials")
    totals = [Counter() for _ in range(7)]
    all_durations = defaultdict(list)
    for (trial, phase), (event_trial, rows) in zip(phases.items(), trial_events(path / "vm-events.csv"), strict=True):
        if (trial != event_trial or phase["enabled"] or len(phase["probes"]) != 32 or
                {r["kind"]: r["stack"] for r in rows if r["kind"] in {"b", "c"}} != {"b": INTERIOR, "c": EXTERIOR}):
            raise ValueError("Unexpected trial condition or missing probes")
        data, *counters, durations, windows = inspect_stages(rows, ms)
        if data["open"] or data["promoted"] or not counters[0]:
            raise ValueError("Incomplete or accelerated trial")
        for total, trial_values in zip(totals, counters, strict=True):
            total.update(trial_values)
        for key, values in durations.items():
            all_durations[key].extend(values)
        print(f"trial={trial} dyn_tail={data['dyn_tail']} complete_cycles={windows} "
              f"holder_calls={dict(counters[0])} fifo_head={dict(counters[5])}")
    counts, sources, returns, routes, contexts, heads, overlap = totals
    print(f"queue_classification {dict(counts)}")
    for name, counter in (("publication", sources), ("preceding_return", returns), ("return_route", routes),
                          ("tasklet_context", contexts), ("fifo_head", heads)):
        for labels, value in sorted(counter.items()):
            print(name, *labels, f"calls={value}")
    for labels, value in sorted(overlap.items()):
        print("lock_overlap", *labels, f"ms={value * ms:.3f}")
    for name, values in sorted(all_durations.items()):
        samples = [value * ms for value in values]
        print(f"{name} calls={len(samples)} p50_ms={median(samples):.3f} p95_ms={percentile(samples, .95):.3f}")


if __name__ == "__main__":
    main()
