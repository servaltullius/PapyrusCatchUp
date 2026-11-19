"""Check DynDOLOD queue-head and native dispatch context at stock tasklet completion."""

import argparse
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
from pathlib import Path
from statistics import median

from analyze_continuation_ab import EXTERIOR, INTERIOR, trial_events
from analyze_gameplay_trace import parse_log, percentile
from analyze_getat_return_wait import holder_native_spans, inspect_trial
from analyze_native_drain import drains_and_enqueues, locate
from analyze_native_stages import LATE, ticks


EXACT_HEAD, ROOT_LOCK, VM_FREEZE_GUARD, NATIVE_PHASE, OVERFLOW = 1, 2, 4, 8, 16
UPDATE_TASKLETS_RETURN_RVA = 0x14449BF
JOB_CALLBACK_RETURN_RVA = 0xCF7888


def finish_records(rows):
    enqueues, tagged, finished, matches = {}, {}, {}, {}
    for row in rows:
        kind, thread, stack = (row[key] for key in ("kind", "thread", "stack"))
        if kind == "[" and row["depth"] == "1":
            enqueues[thread, stack] = row
        elif kind == "t":
            enqueue = enqueues.pop((thread, stack), None)
            if not enqueue or not 0 < int(enqueue["detail"]) <= ticks(enqueue) <= ticks(row) or thread in tagged:
                raise ValueError("Unmatched tasklet enqueue tag")
            tagged[thread] = enqueue, row
        elif kind == "!":
            enqueue, tag = tagged.pop(thread)
            if stack != tag["stack"] or thread in finished:
                raise ValueError("Tasklet completion does not match tagged stack")
            finished[thread] = enqueue, tag, row
        elif kind == "@":
            enqueue, tag, end = finished.pop(thread)
            flags = int(row["depth"])
            if ((flags & EXACT_HEAD and row["stack"] != end["stack"]) or
                    (flags & ROOT_LOCK and not flags & EXACT_HEAD) or
                    (flags & OVERFLOW and flags & EXACT_HEAD)):
                raise ValueError("Contradictory stock FIFO or lock snapshot")
            matches[thread, end["stack"], ticks(enqueue)] = tag, end, row
    return matches


def update_records(rows):
    active, updates = {}, defaultdict(list)
    tasklet_batches = []
    for row in rows:
        kind, thread, phase = (row[key] for key in ("kind", "thread", "depth"))
        key = thread, phase
        if kind == "g" and phase in {"767", "768"}:
            if key in active:
                raise ValueError("Overlapping stock update on one thread")
            active[key] = row
        elif kind == "l" and phase in {"767", "768"}:
            before = active.pop(key, None)
            if not before:
                raise ValueError("Unmatched stock update end")
            updates[key].append((before, row))
        elif kind == "J":
            before, after = updates[thread, "768"][-1]
            if ticks(row) < ticks(after) or (tasklet_batches and tasklet_batches[-1][1] is after):
                raise ValueError("Unmatched tasklet batch snapshot")
            tasklet_batches.append((before, after, row))
    return updates, tasklet_batches


def finish_gate(tag, end, head, previous, following):
    flags = int(head["depth"])
    if flags & OVERFLOW:
        return "overflow"
    if not flags & EXACT_HEAD:
        return "another_head" if head["stack"] != "0" else "no_head"
    if not flags & ROOT_LOCK:
        return "root_lock_not_retained"
    if not tag["detail"] == end["depth"] == head["detail"] == previous.frame:
        return "different_render_frame"
    if following.frame == previous.frame:
        return "stock_drains_again_this_frame"
    if not following.pops or following.pops[0][:2] != (end["stack"], "1"):
        return "not_first_next_stock_pop"
    if end["thread"] != previous.thread:
        return "outside_native_drain_thread"
    if not flags & VM_FREEZE_GUARD:
        return "missing_vm_freeze_guard"
    if not flags & NATIVE_PHASE:
        return "outside_native_phase"
    return "verified_fifo_and_native_context"


def inspect_finishes(rows, ms):
    data, *_, holders = inspect_trial(rows, ms)
    drains, publishes = drains_and_enqueues(rows)
    starts = [d.start for d in drains]
    matches = finish_records(rows)
    updates, batches = update_records(rows)
    batch_times = [ticks(head) for _, _, head in batches]
    update_times = {key: [ticks(begin) for begin, _ in pairs] for key, pairs in updates.items()}
    counts, gate, frames, threads, locks, caller, outer, batch, all_finishes, fifo = (Counter() for _ in range(10))
    durations = defaultdict(list)
    ranks = []
    for _, _, head in matches.values():
        flags = int(head["depth"])
        all_finishes["exact_fifo_head"] += bool(flags & EXACT_HEAD)
        all_finishes["exact_head_with_root_lock"] += bool(flags & ROOT_LOCK)
        all_finishes["owns_vm_freeze"] += bool(flags & VM_FREEZE_GUARD)
        all_finishes["inside_native_phase"] += bool(flags & NATIVE_PHASE)
    locks["stock_drain_starts"] = sum(r["kind"] == "D" for r in rows)
    locks["stock_drain_owns_freeze"] = sum(r["kind"] == "D" and int(r["depth"]) & VM_FREEZE_GUARD != 0 for r in rows)
    for queued, entered, _ in holder_native_spans(rows, holders):
        if (entered["name1"].lower(), entered["name3"].lower()) != ("objectreference", "getworldspace"):
            continue
        classification, _, advanced = locate(queued, entered, drains, publishes)
        if advanced:
            raise ValueError("Unexpected acceleration in stock trial")
        counts[classification] += 1
        if classification not in LATE:
            continue
        key = queued["thread"], queued["stack"], queued["depth"]
        start, published = next((a, b) for a, b in publishes[key] if a <= ticks(queued) <= b)
        matched = matches.get((queued["thread"], queued["stack"], start))
        if matched is None:
            gate["no_verified_tasklet_end"] += 1
            continue
        tag, end, head = matched
        index = bisect_right(starts, start) - 1
        if index < 0 or index + 1 >= len(drains) or not published <= ticks(tag) <= ticks(end) <= ticks(head):
            raise ValueError("Tasklet finish is outside its queue publication trace")
        previous, following = drains[index:index + 2]
        rank, = (i for i, (owner, kind, pop, checked) in enumerate(following.pops)
                 if owner == end["stack"] and kind == "1" and pop <= ticks(entered) <= checked)
        ranks.append(rank)
        fifo["first_in_next_drain"] += rank == 0
        fifo["snapshot_head_is_first_in_next_drain"] += bool(following.pops and head["stack"] == following.pops[0][0])
        gate[finish_gate(tag, end, head, previous, following)] += 1
        flags = int(head["depth"])
        thread = "same_thread" if end["thread"] == previous.thread else "other_thread"
        threads[thread] += 1
        locks["tasklet_owns_vm_freeze"] += bool(flags & VM_FREEZE_GUARD)
        locks["tasklet_inside_native_phase"] += bool(flags & NATIVE_PHASE)
        locks["tasklet_exact_fifo_head"] += bool(flags & EXACT_HEAD)
        locks["tasklet_exact_head_with_root_lock"] += bool(flags & ROOT_LOCK)
        same_frame = tag["detail"] == end["depth"] == head["detail"] == previous.frame
        frames["matches_earlier_drain" if same_frame else "different_or_crossed_frame"] += 1
        if same_frame and flags & ROOT_LOCK:
            threads["same_frame_locked_head", thread] += 1
        rva = int(end["detail"])
        route = {UPDATE_TASKLETS_RETURN_RVA: "direct_UpdateTasklets", JOB_CALLBACK_RETURN_RVA: "BSJobs_callback"}
        caller[rva, route.get(rva, "other_VMProcess_caller")] += 1
        update_key = previous.thread, "767"
        update_index = bisect_right(update_times.get(update_key, ()), previous.start) - 1
        if update_index >= 0:
            _, update_end = updates[update_key][update_index]
            if previous.end <= ticks(update_end):
                outer["after_Update" if ticks(end) >= ticks(update_end) else "before_Update_return", thread] += 1
        if following.start > ticks(head):
            durations["tasklet_finish_to_next_stock_drain"].append(following.start - ticks(end))
        durations["publication_to_tasklet_finish"].append(ticks(end) - published)
        first = bisect_left(batch_times, ticks(end))
        last = bisect_left(batch_times, following.start)
        for _, _, batch_head in batches[first:last]:
            snapshot = int(batch_head["detail"])
            if str(snapshot & 0xFFFFFFFF) == previous.frame and batch_head["stack"] == end["stack"] and batch_head["depth"] == "1":
                context = snapshot >> 32
                batch["same_frame_fifo_head"] += 1
                batch["same_frame_fifo_head_has_native_context"] += (context & (VM_FREEZE_GUARD | NATIVE_PHASE)) == (VM_FREEZE_GUARD | NATIVE_PHASE)
    counters = (counts, gate, frames, threads, locks, caller, outer, batch, all_finishes, fifo)
    return data, counters, durations, ranks, len(drains), len(matches), len(batches)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    path = args.capture
    phases, ms = parse_log(path / "vm.log")
    if (len(phases) != 4 or "Installed stock tasklet completion, FIFO, and VM update context observation" not in
            (path / "vm.log").read_text(encoding="utf-8-sig")):
        raise ValueError("Missing complete four-trial stock tasklet observation")
    labels = ("queue_classification", "finish_gate", "frame", "thread", "lock", "caller_rva", "outer_update", "local_batch",
              "all_tagged_finishes", "fifo")
    totals = [Counter() for _ in labels]
    intervals = defaultdict(list)
    ranks = []
    for (trial, phase), (event_trial, rows) in zip(phases.items(), trial_events(path / "vm-events.csv"), strict=True):
        if (trial != event_trial or phase["enabled"] or len(phase["probes"]) != 32 or
                {r["kind"]: r["stack"] for r in rows if r["kind"] in {"b", "c"}} != {"b": INTERIOR, "c": EXTERIOR}):
            raise ValueError("Unexpected stock trial condition")
        data, counters, durations, trial_ranks, drains, matches, batches = inspect_finishes(rows, ms)
        if data["open"] or data["promoted"] or not counters[0]:
            raise ValueError("Incomplete or accelerated trial")
        for total, values in zip(totals, counters, strict=True):
            total.update(values)
        for name, values in durations.items():
            intervals[name].extend(values)
        ranks.extend(trial_ranks)
        print(f"trial={trial} dyn_tail={data['dyn_tail']} stock_drains={drains} observed_tasklet_ends={matches} "
              f"local_tasklet_batches={batches} holder_calls={dict(counters[0])} finish_gate={dict(counters[1])}")
    for label, counter in zip(labels, totals, strict=True):
        for key, count in sorted(counter.items(), key=lambda item: str(item[0])):
            if label == "caller_rva":
                key = f"0x{key[0]:X} {key[1]}"
            print(f"{label} {key} count={count}")
    for name, values in sorted(intervals.items()):
        samples = [value * ms for value in values]
        print(f"{name} calls={len(samples)} p50_ms={median(samples):.3f} p95_ms={percentile(samples, .95):.3f}")
    if ranks:
        print(f"messages_popped_ahead_in_next_drain calls={len(ranks)} min={min(ranks)} p50={median(ranks):.1f} "
              f"p95={percentile(ranks, .95):.1f} max={max(ranks)}")


if __name__ == "__main__":
    main()
