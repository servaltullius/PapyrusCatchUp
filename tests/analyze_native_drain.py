"""Measure where DynDOLOD lock holders' GetWorldSpace calls miss a native drain."""

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from statistics import median
from struct import pack, unpack

from analyze_continuation_ab import EXTERIOR, INTERIOR, REPLICATION_ORDER, trial_events
from analyze_gameplay_trace import frame_metrics, parse_log, percentile, read_csv, show
from analyze_getat_return_wait import holder_native_spans, inspect_trial


@dataclass
class Drain:
    start: int
    frame: str
    thread: str
    budget: float
    cutoff: int = 0
    end: int = 0
    budget_stop: bool = False
    pops: list = field(default_factory=list)
    extension_target: str | None = None
    extra_pops: set = field(default_factory=set)


def drains_and_enqueues(rows):
    phases, active, pending = [], {}, {}
    publishes = defaultdict(list)
    for row in rows:
        kind, stack, depth, thread = (row[key] for key in ("kind", "stack", "depth", "thread"))
        time, detail = int(row["qpc"]), int(row["detail"])
        if kind == "D":
            budget, = unpack("<f", pack("<I", detail))
            if thread in active:
                raise ValueError("Overlapping native drains on one thread")
            active[thread] = Drain(time, stack, thread, budget)
        elif kind == "K" and thread in active:
            active[thread].pops.append((stack, depth, detail, time))
        elif kind == "W" and thread in active:
            phase = active[thread]
            if phase.extension_target or row["detail"] != phase.frame:
                raise ValueError("Invalid native budget extension marker")
            phase.extension_target = stack
        elif kind == "=" and thread in active:
            phase = active[thread]
            if not phase.extension_target or not phase.pops or phase.pops[-1][:3] != (stack, depth, detail):
                raise ValueError("Extra pop did not match a stock queue pop")
            phase.extra_pops.add(detail)
        elif kind == "d" and thread in active:
            phase = active.pop(thread)
            if phase.frame != stack or not phase.start <= detail <= time or (depth == "1" and not phase.pops):
                raise ValueError("Invalid native drain boundary")
            phase.cutoff, phase.end, phase.budget_stop = detail, time, depth == "1"
            phases.append(phase)
        elif kind == "[":
            pending[thread, stack, depth] = time
        elif kind == "]":
            before = pending.pop((thread, stack, depth), None)
            if before is not None and detail == 1:
                publishes[thread, stack, depth].append((before, time))
    phases.sort(key=lambda phase: phase.start)
    if not phases or any(a.end > b.start for a, b in zip(phases, phases[1:])):
        raise ValueError("Missing or overlapping stock native drain windows")
    return phases, publishes


def locate(queued, entered, phases, publishes):
    stack, depth = entered["stack"], entered["depth"]
    queued_at, entered_at = int(queued["qpc"]), int(entered["qpc"])
    publications = [(start, end) for start, end in publishes[queued["thread"], stack, depth] if start <= queued_at <= end]
    if len(publications) != 1:
        return "unmatched_enqueue", None, False
    matches = [(index, rank, pop_at) for index, phase in enumerate(phases)
               if phase.thread == entered["thread"] and phase.start <= entered_at <= phase.cutoff
               for rank, (owner, kind, pop_at, checked_at) in enumerate(phase.pops)
               if owner == stack and kind == "1" and pop_at <= entered_at <= checked_at]
    if len(matches) != 1:
        return "unmatched_pop", None, False
    index, rank, pop_at = matches[0]
    extended = pop_at in phases[index].extra_pops
    start, end = publications[0]
    previous = phases[index - 1] if index else None
    earlier = [phase for phase in phases[:index] if end <= phase.cutoff]
    if any(not phase.budget_stop for phase in earlier):
        return "empty_despite_publication", None, extended
    if earlier:
        return "budget_deferred", (len(earlier), rank, earlier[0].budget), extended
    if not previous:
        return "no_previous_phase", None, extended
    if start <= previous.cutoff:
        return "publication_overlapped_cutoff", None, extended
    if end <= phases[index].start:
        label = "after_budget_stop" if previous.budget_stop else "after_empty_drain"
        return label, (start - previous.cutoff, phases[index].start - end, pop_at - phases[index].start), extended
    if start >= phases[index].start:
        return "inside_servicing_drain", None, extended
    return "publication_overlapped_start", None, extended


def analyze_trial(rows, ms):
    data, _, _, _, _, _, holders = inspect_trial(rows, ms)
    phases, publishes = drains_and_enqueues(rows)
    counts, waits, overlap_ms, details = Counter(), defaultdict(list), Counter(), defaultdict(list)
    advanced = 0
    for queued, entered, overlap in holder_native_spans(rows, holders):
        if (entered["name1"].lower(), entered["name3"].lower()) != ("objectreference", "getworldspace"):
            continue
        label, detail, extended = locate(queued, entered, phases, publishes)
        advanced += extended
        counts[label] += 1
        overlap_ms[label] += overlap * ms
        waits[label].append((int(entered["qpc"]) - int(queued["qpc"])) * ms)
        if detail is not None:
            details[label].append(detail)
    return data, phases, counts, waits, overlap_ms, details, advanced


def targeted_extra_calls(rows, drains):
    extras = defaultdict(list)
    for phase in drains:
        for stack, kind, popped, checked in phase.pops:
            if kind == "1" and popped in phase.extra_pops:
                extras[phase.thread, stack].append((popped, checked))
    return sum(any(start <= int(row["qpc"]) <= end for start, end in extras.get((row["thread"], row["stack"]), ()))
               for row in rows if row["kind"] == "P" and row["depth"] == "1" and
               row["name1"].lower() == "objectreference" and row["name3"].lower() == "getworldspace")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--ab", action="store_true")
    args = parser.parse_args()
    path = args.capture
    phases, ms = parse_log(path / "vm.log")
    if "Installed stock native drain observation" not in (path / "vm.log").read_text(encoding="utf-8-sig"):
        raise ValueError("Missing stock drain observation hook")
    if args.ab and tuple(phase["enabled"] for phase in phases.values()) not in (REPLICATION_ORDER[:4], REPLICATION_ORDER):
        raise ValueError("Incomplete or misordered A/B capture")
    presents = read_csv(path / "present.csv")
    engine = [(int(row["qpc"]), int(row["frame"])) for row in read_csv(path / "engine-render.csv")]
    all_counts, all_overlap = Counter(), Counter()
    all_waits, all_details = defaultdict(list), defaultdict(list)
    drain_reasons, budgets = Counter(), []
    groups = defaultdict(lambda: defaultdict(list))
    pairs = []
    for (trial, phase), (event_trial, rows) in zip(phases.items(), trial_events(path / "vm-events.csv"), strict=True):
        if trial != event_trial or (phase["enabled"] and not args.ab) or len(phase["probes"]) != 32:
            raise ValueError("Unexpected trial condition or missing responses")
        cells = {row["kind"]: row["stack"] for row in rows if row["kind"] in {"b", "c"}}
        if cells != {"b": INTERIOR, "c": EXTERIOR}:
            raise ValueError("Unexpected cell transition")
        data, drains, counts, waits, overlaps, details, advanced = analyze_trial(rows, ms)
        if not counts or data["promoted"] or data["open"] or data["drain"] is None:
            raise ValueError("Incomplete stock-only DynDOLOD trial")
        all_counts.update(counts)
        all_overlap.update(overlaps)
        for key, samples in waits.items():
            all_waits[key].extend(samples)
        for key, samples in details.items():
            all_details[key].extend(samples)
        drain_reasons.update("budget" if entry.budget_stop else "empty" for entry in drains)
        budgets.extend(entry.budget for entry in drains)
        two_seconds = round(2000 / ms)
        steady = {**phase, "start": phase["end"] - two_seconds}
        frame_count, _, observed = frame_metrics(presents, engine, steady, ms)
        mode = "on" if phase["enabled"] else "off"
        extended = [entry for entry in drains if entry.extension_target]
        arrived = next(int(row["qpc"]) for row in rows if row["kind"] == "c")
        early_count, _, early = frame_metrics(presents, engine, {"start": arrived, "end": arrived + two_seconds}, ms)
        if len(early) != 1 or len(observed) != 1:
            raise ValueError("Expected one PresentMon submission per engine frame")
        if (extended and mode == "off" or len({entry.frame for entry in extended}) != len(extended) or
                any(entry.start <= arrived for entry in extended) or len(extended) != data["counts"]["W"]):
            raise ValueError("Native budget changed outside the permitted frame or trial")
        group = groups[mode]
        group["probes"].extend(phase["probes"])
        group["frames"].extend(observed[0])
        group["early_frames"].extend(early[0])
        trial_metrics = {"arrival_to_drain": data["drain"], "total": data["total"], "tail_wait": data["tail_wait"],
                         "at_return_wait": data["at_return_wait"], "response_p95": percentile(phase["probes"], .95),
                         "frame_p99": percentile(observed[0], .99), "early_frame_p99": percentile(early[0], .99),
                         "holder_queue": sum(overlaps.values())}
        pairs.append((mode, trial_metrics))
        for name, value in trial_metrics.items():
            group[name].append(value)
        if args.ab:
            group["extensions"].append(len(extended))
            group["extended_empty"].append(sum(not entry.budget_stop for entry in extended))
            group["extra_messages"].append(sum(len(entry.extra_pops) for entry in extended))
            group["getworldspace_extra"].append(targeted_extra_calls(rows, drains))
            group["holder_extra"].append(advanced)
            group["tagged"].append(data["counts"]["t"])
            group["queue_matches"].append(data["counts"]["V"])
            group["with_reserve"].append(sum(row["kind"] == "V" and row["detail"] == "1" for row in rows))
            print(f"  {mode} tagged={group['tagged'][-1]} fifo_matches={group['queue_matches'][-1]} "
                  f"with_reserve={group['with_reserve'][-1]} extended={len(extended)} extended_empty={group['extended_empty'][-1]} "
                  f"extra_messages={group['extra_messages'][-1]} "
                  f"getworldspace_extra={group['getworldspace_extra'][-1]} holder_extra={advanced}")
        print(f"trial={trial} mode={mode} tail={data['dyn_tail']} drains={len(drains)} "
              f"budget_stops={sum(entry.budget_stop for entry in drains)} frames_first_2s_after_arrival={early_count} "
              f"frames_last_2s={frame_count} "
              f"unpaired_drain_starts={sum(row['kind'] == 'D' for row in rows) - len(drains)} "
              + " ".join(f"{key}={value}" for key, value in counts.items()))
    print(f"drain_end_reasons {dict(drain_reasons)} original_budget_ms_median={show(median(budgets))} "
          f"min={show(min(budgets))} max={show(max(budgets))}")
    for label, count in sorted(all_counts.items()):
        waits = all_waits[label]
        print(f"{label} calls={count} lock_hold_overlap_ms={all_overlap[label]:.3f} "
              f"Q_to_P_p50_ms={median(waits):.3f} Q_to_P_p95_ms={percentile(waits, .95):.3f}")
    for reason in ("after_empty_drain", "after_budget_stop"):
        if not all_details[reason]:
            continue
        for field, ticks in zip(("previous_cutoff_to_enqueue", "enqueue_done_to_drain", "drain_start_to_pop"),
                                zip(*all_details[reason], strict=True), strict=True):
            values = [value * ms for value in ticks]
            print(f"{reason} {field}_p50_ms={median(values):.3f} p95_ms={percentile(values, .95):.3f}")
    for skipped, earlier, budget in all_details["budget_deferred"]:
        print(f"budget_case skipped_drains={skipped} messages_popped_ahead_in_servicing_drain={earlier} original_budget_ms={budget:.6f}")
    for mode, group in sorted(groups.items()):
        print(f"{mode}_metrics probes={len(group['probes'])} response_p95_ms={show(percentile(group['probes'], .95))} "
              f"steady_frame_p50_ms={show(median(group['frames']))} steady_frame_p99_ms={show(percentile(group['frames'], .99))} "
              f"early_frame_p99_ms={show(percentile(group['early_frames'], .99))} "
              + " ".join(f"{name}_median_ms={show(median(group[name]))}" for name in (
                  "arrival_to_drain", "total", "tail_wait", "at_return_wait", "holder_queue")))
        if args.ab:
            print(f"{mode}_activity " + " ".join(f"{name}={sum(group[name])}" for name in (
                "tagged", "queue_matches", "with_reserve", "extensions", "extended_empty", "extra_messages", "getworldspace_extra", "holder_extra")))
    if args.ab:
        changes = defaultdict(list)
        for left, right in zip(pairs[::2], pairs[1::2], strict=True):
            off, on = (left[1], right[1]) if left[0] == "off" else (right[1], left[1])
            for name in off:
                changes[name].append(on[name] - off[name])
        for name, samples in changes.items():
            print(f"paired_{name} median_on_minus_off_ms={show(median(samples))} improved={sum(delta < 0 for delta in samples)}/{len(samples)}")


if __name__ == "__main__":
    main()
