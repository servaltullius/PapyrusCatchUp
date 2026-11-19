"""Verify same-thread DynDOLOD continuation and compare exterior return windows."""

import argparse
import csv
from bisect import bisect_right
from collections import Counter, defaultdict
from itertools import groupby
from pathlib import Path
from statistics import median

from analyze_gameplay_trace import frame_metrics, parse_log, percentile, read_csv, show
from analyze_lock_transitions import analyze_trial as analyze_locks
from analyze_resume_trace import analyze
from analyze_vm_chains import CELL


EXTERIOR = str(0x1A27F)
INTERIOR = str(0x1605E)
READS = {"objectreference.getworldspace", "formlist.getat"}
REPLICATION_ORDER = (False, True, True, False, True, False, False, True)


def trial_events(path):
    with path.open(newline="", encoding="utf-8-sig") as stream:
        for trial, rows in groupby(csv.DictReader(stream), key=lambda row: int(row["trial"])):
            yield trial, list(rows)


def worldspace_returns(rows, ms):
    """Match the VM result to its native call without resolving the object again."""
    arrival = next(int(row["qpc"]) for row in rows if row["kind"] == "c")
    previous_at, promoted, dispatch, sources = {}, {}, {}, {}
    lock_owners, return_locks = {}, {}
    stock, accelerated = [], []
    for row in rows:
        key = row["stack"], row["thread"]
        kind, name, time = row["kind"], row["name3"].lower(), int(row["qpc"])
        if kind == "G":
            lock_owners[row["detail"]] = key[0]
        elif kind in {"X", "Z"} and lock_owners.get(row["detail"]) == key[0]:
            del lock_owners[row["detail"]]
        elif kind == "A":
            return_locks[key] = row["detail"]
        elif kind == "N" and row["depth"] == "1" and name == "getat" and row["name1"].lower() == "formlist":
            previous_at[key] = time
            return_locks.pop(key, None)
        elif kind == "i" and name == "getworldspace":
            if key not in previous_at:
                raise ValueError("Promoted GetWorldSpace has no preceding GetAt return on the same thread")
            promoted[key] = time
        elif kind == "M" and row["depth"] == "1" and name == "getworldspace" and row["name1"].lower() == "objectreference":
            dispatch[key] = time
        elif kind == "y":
            sources[key] = int(row["detail"])
        elif kind == "Y":
            if key not in dispatch or key not in sources:
                raise ValueError("GetWorldSpace result is missing its source or dispatch")
            source, value = sources.pop(key), int(row["detail"])
            dispatched = dispatch.pop(key)
            early = promoted.pop(key, None)
            if early is not None:
                holds_return_lock = lock_owners.get(return_locks.get(key)) == key[0]
                accelerated.append((key[0], early, source, value, (dispatched - previous_at[key]) * ms, holds_return_lock))
            elif dispatched >= arrival:
                stock.append((source, value))
    if promoted or sources:
        raise ValueError("Incomplete GetWorldSpace return capture")
    return stock, accelerated


def list_lock_holds(rows, ms):
    """Pair a newly acquired list lock with its first release after GetAt returns."""
    owners = {}
    hold = None
    promoted = False
    values = defaultdict(list)
    for row in rows:
        kind, at, token = row["kind"], int(row["qpc"]), row["detail"]
        get_at = row["depth"] == "1" and row["name1"].lower() == "formlist" and row["name3"].lower() == "getat"
        if kind == "G":
            owners[token] = at
        elif kind == "i" and get_at:
            promoted = True
        elif kind == "M":
            hold = (at, None, promoted) if get_at else None
            promoted = False
        elif kind == "N" and get_at and hold:
            hold = (hold[0], at, hold[2])
        elif kind in {"X", "Z"}:
            acquired = owners.pop(token, None)
            if hold and hold[1] is not None:
                entered, returned, early = hold
                if kind == "X" and acquired is not None and acquired <= entered <= returned <= at and (at - returned) * ms < 1:
                    values["promoted" if early else "stock"].append((at - acquired) * ms)
                hold = None
    return values


def inspect(rows, ms):
    rows.sort(key=lambda row: int(row["qpc"]))
    request = next(int(row["qpc"]) for row in rows if row["kind"] == "b")
    arrival = next(int(row["qpc"]) for row in rows if row["kind"] == "c")
    probes = next(int(row["qpc"]) for row in rows if row["kind"] == "v")
    first_queue = {}
    running, completed = {}, []
    by_stack = defaultdict(list)
    counts, slices, frame_ids = Counter(), {}, set()
    guard_rejections = Counter()
    latest_dispatch, ready_reads = {}, {}
    durations, promoted = [], []
    for row in rows:
        at = int(row["qpc"])
        stack, kind, depth, thread, detail = (row[key] for key in ("stack", "kind", "depth", "thread", "detail"))
        by_stack[stack].append(row)
        counts[kind] += 1
        if kind == "w":
            guard_rejections[detail] += 1
        if kind == "Q" and depth == "0":
            first_queue.setdefault(stack, at)
        if depth == "0" and row["name3"].lower() in CELL:
            queued = first_queue.get(stack, at)
            if kind == "P" and request <= queued < probes:
                running[stack] = queued, at
            elif kind == "O" and stack in running:
                completed.append((row, *running.pop(stack)))

        key = thread, stack
        if kind == "M":
            latest_dispatch[key] = f'{row["name1"].lower()}.{row["name3"].lower()}'
        elif kind == "f":
            ready_reads[key] = latest_dispatch.get(key)

        if kind == "h":
            if thread in slices or detail in frame_ids:
                raise ValueError("Duplicate continuation slice in a thread or rendered frame")
            prior = ready_reads.pop(key, None)
            if prior not in READS:
                raise ValueError("Missing stock getter on the continuation thread")
            slices[thread] = (stack, detail, prior, [])
            frame_ids.add(detail)
        elif thread in slices:
            owner, frame, prior, events = slices[thread]
            if kind == "k":
                if owner != stack:
                    raise ValueError("Continuation returned from a different stack")
                durations.append(int(detail) * ms)
                del slices[thread]
                chosen = [(time, name) for action, time, name in events if action == "i"]
                if len(chosen) > 1:
                    raise ValueError("Continuation exceeded its one-read cap")
                for time, name in chosen:
                    if name not in READS - {prior} or not any(action == "T" and entered <= time for action, entered, _ in events) or not all(
                            any(action == marker and entered >= time and actual == name for action, entered, actual in events)
                            for marker in ("M", "N")):
                        raise ValueError("Promoted getter did not dispatch on the same stack/thread")
                    promoted.append((owner, frame, name, time))
            elif owner == stack and kind in {"i", "M", "N", "T"}:
                if kind == "i" and detail != frame:
                    raise ValueError("Promotion crossed a rendered frame boundary")
                events.append((kind, at, f'{row["name1"].lower()}.{row["name3"].lower()}'))
            elif kind == "i":
                raise ValueError("Promoted getter belongs to a different stack")
        elif kind in {"i", "k"}:
            raise ValueError("Continuation marker has no slice on this thread")

    if slices or not completed:
        raise ValueError("Incomplete cell or continuation capture")
    end, queued, entered = completed[-1]
    tail = end["stack"]
    calls, _ = analyze(by_stack[tail], ms)
    dyn = [(row, qpc, push) for row, qpc, push in completed if row["name1"].lower() == "sheson_dyndolod_objectenabler"]
    dyn_tail = dyn[-1][0]["stack"] if dyn else None
    dyn_calls, _ = analyze(by_stack[dyn_tail], ms) if dyn_tail else ([], [])
    list_holds = defaultdict(list)
    for root, _, _ in dyn:
        for kind, times in list_lock_holds(by_stack[root["stack"]], ms).items():
            list_holds[kind].extend(times)
    return {
        "tail": f'{end["name1"]}.{end["name3"]}',
        "tail_stack": tail,
        "root_wait": (entered - queued) * ms,
        "root_wait_after_arrival": max(0, entered - max(queued, arrival)) * ms,
        "native_wait": sum(call[1] for call in calls),
        "tail_wait": (entered - queued) * ms + sum(call[1] for call in calls),
        "target_wait": sum(call[1] for call in dyn_calls if call[0] in READS),
        "at_return_wait": sum(call[4] for call in dyn_calls if call[0] == "formlist.getat"),
        "at_lock_failures": sum(len(call[6]) for call in dyn_calls if call[0] == "formlist.getat"),
        "arrival": (arrival - request) * ms,
        "drain": max(0, int(end["qpc"]) - arrival) * ms if not running else None,
        "total": (int(end["qpc"]) - request) * ms if not running else None,
        "roots": len(completed),
        "open": len(running),
        "probes_start": probes,
        "counts": counts,
        "guard_rejections": guard_rejections,
        "durations": durations,
        "promoted": promoted,
        "dyn_tail": dyn_tail,
        "list_holds": list_holds,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--replication", action="store_true")
    parser.add_argument("--reverse", action="store_true")
    parser.add_argument("--allow-incomplete", action="store_true")
    args = parser.parse_args()
    path = args.capture
    phases, ms = parse_log(path / "vm.log", allow_incomplete=args.allow_incomplete)
    if "Installed ordered DynDOLOD read continuation" not in (path / "vm.log").read_text(encoding="utf-8-sig"):
        raise ValueError("Missing continuation hook install evidence")
    if args.allow_incomplete:
        skipped = [trial for trial, phase in phases.items() if "end" not in phase or "probes" not in phase]
        for trial in skipped:
            del phases[trial]
        print(f"Incomplete launch; analyzing only completed trials. Incomplete trials={skipped}")
    order = REPLICATION_ORDER[4:] + REPLICATION_ORDER[:4] if args.reverse else REPLICATION_ORDER
    if args.replication and tuple(phase["enabled"] for phase in phases.values()) != order:
        raise ValueError("Missing or misordered replication trials")
    presents = read_csv(path / "present.csv")
    engine = [(int(row["qpc"]), int(row["frame"])) for row in read_csv(path / "engine-render.csv")]
    engine_times = [time for time, _ in engine]
    groups = defaultdict(lambda: defaultdict(list))
    observed_stock_returns = defaultdict(set)
    accelerated_returns = []
    trial_metrics = []
    metrics = ("arrival", "drain", "total", "root_wait", "root_wait_after_arrival", "native_wait", "tail_wait",
               "target_wait", "at_return_wait")
    for (trial, phase), (event_trial, rows) in zip(phases.items(), trial_events(path / "vm-events.csv"), strict=True):
        if trial != event_trial:
            raise ValueError("VM event trial and log trial disagree")
        destination = next(row["stack"] for row in rows if row["kind"] == "c")
        if args.replication and (destination != EXTERIOR or len(phase["probes"]) != 32 or
                next(row["stack"] for row in rows if row["kind"] == "b") != INTERIOR):
            raise ValueError("Replication did not measure the expected exterior return")
        if destination != EXTERIOR:
            if any(row["kind"] in "hi" for row in rows):
                raise ValueError("Continuation ran outside the exterior benchmark")
            continue
        result = inspect(rows, ms)
        mode = "on" if phase["enabled"] else "off"
        if mode == "off" and result["counts"]["h"]:
            raise ValueError("Continuation ran in a disabled trial")
        group = groups[mode]
        group["probes"].extend(phase["probes"])
        group["late_probes"].extend(phase["probes"][4:])
        steady = {**phase, "start": phase["end"] - round(2000 / ms)}
        frames, _, periods = frame_metrics(presents, engine, steady, ms)
        group["frames"].extend(periods[0])
        if len(periods) == 2:
            group["alt_frames"].extend(periods[1])
        post = {**phase, "start": result["probes_start"]}
        try:
            post_count, _, post_periods = frame_metrics(presents, engine, post, ms)
        except ValueError as error:
            print(f"{trial} post-load PresentMon validation failed ({error})")
            post_count, post_periods = "n/a", [[]]
        else:
            group["post_frames"].extend(post_periods[0])
        for metric in metrics:
            if result[metric] is not None:
                group[metric].append(result[metric])
        for kind, values in result["list_holds"].items():
            group[f"{kind}_list_hold"].extend(values)
        if args.replication:
            stock, accelerated = worldspace_returns(rows, ms)
            if {(stack, qpc) for stack, qpc, *_ in accelerated} != {
                    (stack, qpc) for stack, _, _, qpc in result["promoted"]}:
                raise ValueError("Not every promotion has a recorded GetWorldSpace return")
            if mode == "off":
                for source, value in stock:
                    if source:
                        observed_stock_returns[source].add(value)
            accelerated_returns.extend((trial, stack, source, value, gap, held) for stack, _, source, value, gap, held in accelerated)
            trial_metrics.append((phase["enabled"], {**result,
                "response_p95": percentile(phase["probes"], .95), "steady_p99": percentile(periods[0], .99)}))
            _, retries, interrupted, _, _ = analyze_locks(rows)
            group["tail_unlock_gaps"].extend(gap * ms for stack, gap, _, _ in retries if stack == result["dyn_tail"])
            group["tail_retaken"].append(sum(result["dyn_tail"] in waiters for _, waiters in interrupted))
        print(f"{trial} {mode} {result['tail']} stack={result['tail_stack']} roots={result['roots']} open={result['open']} "
              + " ".join(f"{key}_ms={show(result[key])}" for key in metrics))
        print(f"  probes={len(phase['probes'])} p95_ms={show(percentile(phase['probes'], .95))} "
              f"post_load_frames={post_count} post_load_p99_ms={show(percentile(post_periods[0], .99))} "
              f"last_2s_frames={frames} steady_p99_ms={show(percentile(periods[0], .99))} "
              + " ".join(f"{key}={result['counts'][key]}" for key in "fmHhik")
              + f" rejected_state={result['guard_rejections']['1']} rejected_budget={result['guard_rejections']['2']} "
              f"target_tail_promotions={sum(stack == result['dyn_tail'] for stack, _, _, _ in result['promoted'])} "
              f"overall_tail_promotions={sum(stack == result['tail_stack'] for stack, _, _, _ in result['promoted'])} "
              f"target_tail_GetAt_lock_failures={result['at_lock_failures']} max_slice_ms={show(max(result['durations'], default=None))}")
        for stack, frame, name, qpc in result["promoted"]:
            index = bisect_right(engine_times, qpc)
            period = None
            if 0 < index < len(engine) and engine[index][1] == engine[index - 1][1] + 1:
                period = (engine[index][0] - engine[index - 1][0]) * ms
            first = bisect_right(engine_times, qpc - round(250 / ms))
            last = bisect_right(engine_times, qpc + round(250 / ms)) - 1
            local_fps = None
            if 0 <= first < last < len(engine):
                local_fps = (engine[last][1] - engine[first][1]) * 1000 / ((engine[last][0] - engine[first][0]) * ms)
            print(f"  promoted stack={stack} frame={frame} getter={name} sampled_render_period_ms={show(period)} "
                  f"sampled_local_fps={show(local_fps)}")
    for mode, group in sorted(groups.items()):
        print(f"{mode} probes={len(group['probes'])} response_p95_ms={show(percentile(group['probes'], .95))} "
              f"response_without_first_four_ms={show(percentile(group['late_probes'], .95))} "
              f"frame_median_ms={show(median(group['frames']))} frame_p99_ms={show(percentile(group['frames'], .99))} "
              f"alt_p99_ms={show(percentile(group['alt_frames'], .99))} post_load_p99_ms={show(percentile(group['post_frames'], .99))} "
              + " ".join(f"{key}_median_ms={show(median(group[key]) if group[key] else None)}(n={len(group[key])})"
                         for key in metrics))
        for kind in ("stock", "promoted"):
            samples = group[f"{kind}_list_hold"]
            if samples:
                print(f"  {kind}_GetAt_lock_hold_n={len(samples)} p50_ms={show(median(samples))} "
                      f"p95_ms={show(percentile(samples, .95))}")
        if args.replication:
            gaps = group["tail_unlock_gaps"]
            print(f"  target_tail_final_unlock_to_success_n={len(gaps)} p50_ms={show(percentile(gaps, .5))} "
                  f"p95_ms={show(percentile(gaps, .95))} retaken_by_other_stack={sum(group['tail_retaken'])}")
    if args.replication:
        comparisons = Counter()
        for trial, stack, source, value, gap, held in accelerated_returns:
            off_values = observed_stock_returns.get(source)
            if not off_values:
                outcome = "unseen_source"
            elif off_values == {value}:
                outcome = "matched"
            elif value in off_values:
                outcome = "variable_stock_result"
            else:
                outcome = "mismatch"
            comparisons[outcome] += 1
            print(f"  return trial={trial} stack={stack} source={source:016X} result={value:016X} "
                  f"off_results={','.join(f'{item:016X}' for item in sorted(off_values or [])) or 'n/a'} "
                  f"match={outcome} root_lock_observed={held} getat_to_worldspace_ms={gap:.4f}")
        print(f"GetWorldSpace return comparisons {dict(comparisons)} "
              f"distinct_promoted_sources={len({source for _, _, source, *_ in accelerated_returns})}")
        for metric in ("tail_wait", "target_wait", "total", "drain", "response_p95", "steady_p99"):
            deltas = []
            by_activation = defaultdict(list)
            for left, right in zip(trial_metrics[::2], trial_metrics[1::2], strict=True):
                off, on = (right[1], left[1]) if left[0] else (left[1], right[1])
                if off[metric] is not None and on[metric] is not None:
                    delta = on[metric] - off[metric]
                    deltas.append(delta)
                    by_activation[bool(on["promoted"])].append(delta)
            print(f"paired {metric} on_minus_off_median_ms={show(median(deltas) if deltas else None)} "
                  f"improved={sum(delta < 0 for delta in deltas)}/{len(deltas)} "
                  + " ".join(f"{label}_median_ms={show(median(by_activation[key]) if by_activation[key] else None)}"
                             f"(n={len(by_activation[key])})" for label, key in (("activated", True), ("inactive", False))))


if __name__ == "__main__":
    main()
