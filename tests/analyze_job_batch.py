"""Match late native calls to their stock Papyrus BSJobs list and its joined return."""

import argparse
from bisect import bisect_right
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from statistics import median

from analyze_continuation_ab import EXTERIOR, INTERIOR, trial_events
from analyze_gameplay_trace import parse_log, percentile
from analyze_getat_return_wait import holder_native_spans, inspect_trial, returns
from analyze_native_drain import drains_and_enqueues, locate
from analyze_native_stages import LATE, ticks
from analyze_tasklet_finish import NATIVE_PHASE, ROOT_LOCK, VM_FREEZE_GUARD, finish_records


@dataclass
class Batch:
    submit: dict
    members: Counter = field(default_factory=Counter)
    wait: dict | None = None
    end: dict | None = None
    head: dict | None = None
    query: dict | None = None


def batch_records(rows):
    active, joining, finished = {}, {}, {}
    batches = []
    for row in rows:
        kind, thread = row["kind"], row["thread"]
        if kind == "{":
            if thread in active or thread in joining or thread in finished:
                raise ValueError("Overlapping Papyrus batches on one submission thread")
            active[thread] = Batch(row)
        elif kind == "q":
            active[thread].members[row["detail"]] += 1
        elif kind == "|":
            batch = active.pop(thread)
            if (batch.submit["detail"] != row["detail"] or batch.members.total() != int(batch.submit["depth"]) or
                    not batch.members or int(row["depth"]) < 1):
                raise ValueError("Invalid stock submit/wait pair or missing signal count")
            batch.wait = row
            joining[thread] = batch
        elif kind == "}":
            batch = joining.pop(thread)
            if batch.submit["detail"] != row["detail"]:
                raise ValueError("Wait returned for a different job list")
            batch.end = row
            if row["depth"] == "1":
                finished[thread] = batch
            else:
                batches.append(batch)
        elif kind == "%":
            finished[thread].head = row
        elif kind == "^":
            batch = finished.pop(thread)
            if not batch.head or batch.head["stack"] != row["stack"]:
                raise ValueError("Invalid FIFO snapshot after joined wait")
            batch.query = row
            batches.append(batch)
    return batches


def identities(rows):
    end, tasklet, matched = {}, {}, {}
    for row in rows:
        kind, thread, stack = row["kind"], row["thread"], row["stack"]
        if kind == "!":
            end[thread] = row
        elif kind == "Y":
            tasklet[thread] = row
        elif kind == "?":
            done, identity = end.pop(thread), tasklet.pop(thread)
            if not done["stack"] == stack == identity["stack"]:
                raise ValueError("Tasklet and query identities disagree")
            matched[thread, stack, ticks(done)] = identity["detail"], row["detail"]
    return matched


def batch_gate(batch, previous, following, tag, end, tasklet_head, query):
    if batch.query["depth"] == str(0xFFFFFFFF):
        return "overflow"
    if (batch.head["stack"], batch.query["depth"], batch.query["detail"]) != (end["stack"], "1", query):
        return "different_fifo_head" if batch.head["stack"] != "0" else "empty_fifo"
    flags = int(batch.head["depth"])
    if not flags & ROOT_LOCK:
        return "root_lock_not_held"
    if not previous.frame == batch.submit["stack"] == tag["detail"] == end["depth"] == tasklet_head["detail"] == batch.end["stack"] == batch.head["detail"]:
        return "different_render_frame"
    if ticks(batch.head) >= following.start or following.frame == previous.frame:
        return "stock_drain_already_available"
    if not following.pops or following.pops[0][:2] != (end["stack"], "1"):
        return "not_first_stock_pop"
    if batch.head["thread"] != previous.thread:
        return "waiter_is_not_native_drain_thread"
    if not flags & VM_FREEZE_GUARD:
        return "missing_stock_vm_freeze_guard"
    if not flags & NATIVE_PHASE:
        return "outside_stock_native_phase"
    return "verified_stock_context"


def inspect_batches(rows, ms):
    data, *_, holders = inspect_trial(rows, ms)
    drains, publishes = drains_and_enqueues(rows)
    starts = [d.start for d in drains]
    finishes = finish_records(rows)
    pointers = identities(rows)
    batches = batch_records(rows)
    completed = [b for b in batches if b.head]
    completed.sort(key=lambda b: ticks(b.submit))
    batch_starts = [ticks(b.submit) for b in completed]
    by_member = defaultdict(list)
    for batch in completed:
        for tasklet, count in batch.members.items():
            if count == 1:
                by_member[tasklet].append(batch)
    coverage, gates, stats = Counter(), Counter(), Counter()
    intervals = defaultdict(list)
    tokens = {token for _, _, failures, _ in returns(rows, data["dyn_tail"]) for _, token in failures}
    ownership = defaultdict(list)
    for row in rows:
        if row["kind"] in {"G", "X", "Z"} and row["detail"] in tokens:
            ownership[row["stack"]].append(row)
    stats["recorded_wait_returns"] = len(batches)
    stats["joined_batches"] = len(completed)
    stats["member_tasklets"] = sum(b.members.total() for b in completed)
    stats["joined_waits_owning_freeze"] = sum(bool(int(b.head["depth"]) & VM_FREEZE_GUARD) for b in completed)
    stats["joined_waits_in_native_phase"] = sum(bool(int(b.head["depth"]) & NATIVE_PHASE) for b in completed)
    stats["stock_drain_owns_freeze"] = sum(r["kind"] == "D" and bool(int(r["depth"]) & VM_FREEZE_GUARD) for r in rows)
    stats["tagged_tasklet_finishes"] = len(finishes)
    joined = {}
    for (thread, stack, start), (tag, end, _) in finishes.items():
        pointer = pointers.get((thread, stack, ticks(end)))
        if not pointer:
            continue
        member, query = pointer
        matches = [b for b in by_member[member] if ticks(b.submit) <= start <= ticks(tag) <= ticks(end) <= ticks(b.end)]
        if len(matches) == 1:
            batch = matches[0]
            joined[thread, stack, start] = batch, query
            stats["tagged_finishes_matched_to_join"] += 1
            if (batch.head["stack"], batch.query["depth"], batch.query["detail"]) == (stack, "1", query):
                stats["all_tagged_exact_head_at_join"] += 1
                stats["all_tagged_exact_head_retains_root_lock"] += bool(int(batch.head["depth"]) & ROOT_LOCK)
    for queued, entered, _ in holder_native_spans(rows, holders):
        if (entered["name1"].lower(), entered["name3"].lower()) != ("objectreference", "getworldspace"):
            continue
        classification, _, advanced = locate(queued, entered, drains, publishes)
        if advanced:
            raise ValueError("Unexpected acceleration in observation mode")
        coverage[classification] += 1
        if classification not in LATE:
            continue
        thread, stack, depth = (queued[key] for key in ("thread", "stack", "depth"))
        start, published = next((a, b) for a, b in publishes[thread, stack, depth] if a <= ticks(queued) <= b)
        finish = finishes.get((thread, stack, start))
        if not finish:
            gates["unmatched_tasklet_finish"] += 1
            continue
        tag, end, tasklet_head = finish
        joined_batch = joined.get((thread, stack, start))
        if not joined_batch:
            gates["unmatched_or_ambiguous_joined_batch"] += 1
            continue
        batch, query = joined_batch
        if not published <= ticks(tag):
            raise ValueError("Tasklet tag preceded its successful publication")
        index = bisect_right(starts, start) - 1
        if not 0 <= index < len(drains) - 1:
            raise ValueError("Matched job finish outside observed drain cycle")
        previous, following = drains[index:index + 2]
        gate = batch_gate(batch, previous, following, tag, end, tasklet_head, query)
        gates[gate] += 1
        rank, = (i for i, (owner, kind, pop, checked) in enumerate(following.pops)
                 if owner == stack and kind == "1" and pop <= ticks(entered) <= checked)
        stats["head_stack_first_next_drain"] += bool(following.pops and batch.head["stack"] == following.pops[0][0])
        stats["exact_query_head_at_join"] += (batch.head["stack"], batch.query["depth"], batch.query["detail"]) == (stack, "1", query)
        stats["join_on_same_frame_as_drain"] += previous.frame == batch.head["detail"]
        stats["join_on_native_drain_thread"] += previous.thread == batch.head["thread"]
        intervals["fifo_messages_ahead"].append(rank)
        intervals["tasklet_end_to_join_ms"].append((ticks(batch.end) - ticks(end)) * ms)
        intervals["join_to_next_stock_drain_ms"].append((following.start - ticks(batch.end)) * ms)
        if gate == "missing_stock_vm_freeze_guard":
            intervals["exact_head_join_to_stock_dispatch_ms"].append((ticks(entered) - ticks(batch.end)) * ms)
            next_batch = bisect_right(batch_starts, ticks(batch.end))
            if next_batch < len(batch_starts) and batch_starts[next_batch] >= following.end:
                stats["exact_heads_without_intervening_tasklet_batch"] += 1
                acquisition = next((r for r in reversed(ownership[stack]) if r["kind"] == "G" and ticks(r) <= ticks(queued)), None)
                release = next((r for r in ownership[stack] if acquisition and ticks(acquisition) < ticks(r) and
                                r["kind"] in {"X", "Z"} and r["detail"] == acquisition["detail"]), None)
                if release and ticks(release) >= batch_starts[next_batch]:
                    stats["exact_heads_released_after_next_batch_submit"] += 1
                    intervals["next_batch_submit_to_root_release_ms"].append((ticks(release) - batch_starts[next_batch]) * ms)
    return data, coverage, gates, stats, intervals


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    path = args.capture
    phases, ms = parse_log(path / "vm.log")
    if (len(phases) != 4 or "Installed stock Papyrus BSJobs submit, membership, and joined completion observation" not in
            (path / "vm.log").read_text(encoding="utf-8-sig")):
        raise ValueError("Missing complete Papyrus BSJobs observation")
    all_calls, all_gates, all_stats = Counter(), Counter(), Counter()
    intervals = defaultdict(list)
    for (trial, phase), (event_trial, rows) in zip(phases.items(), trial_events(path / "vm-events.csv"), strict=True):
        cells = {r["kind"]: r["stack"] for r in rows if r["kind"] in {"b", "c"}}
        if trial != event_trial or phase["enabled"] or len(phase["probes"]) != 32 or cells != {"b": INTERIOR, "c": EXTERIOR}:
            raise ValueError("Unexpected BSJobs trial")
        data, calls, gates, stats, samples = inspect_batches(rows, ms)
        if data["open"] or data["promoted"] or not calls:
            raise ValueError("Incomplete or accelerated trial")
        all_calls.update(calls)
        all_gates.update(gates)
        all_stats.update(stats)
        for name, values in samples.items():
            intervals[name].extend(values)
        print(f"trial={trial} dyn_tail={data['dyn_tail']} holder_calls={dict(calls)} gates={dict(gates)} stats={dict(stats)}")
    for label, counter in (("call", all_calls), ("gate", all_gates), ("stat", all_stats)):
        for key, count in sorted(counter.items()):
            print(f"{label} {key} count={count}")
    for name, samples in sorted(intervals.items()):
        print(f"{name} calls={len(samples)} min={min(samples):.3f} p50={median(samples):.3f} "
              f"p95={percentile(samples, .95):.3f} max={max(samples):.3f}")


if __name__ == "__main__":
    main()
