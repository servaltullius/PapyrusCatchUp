"""Trace DynDOLOD root-lock release and audit stock tasklet admission and allocation."""

import argparse
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict, deque
from pathlib import Path
from statistics import median

from analyze_continuation_ab import EXTERIOR, INTERIOR, trial_events
from analyze_gameplay_trace import parse_log, percentile
from analyze_getat_return_wait import holder_native_spans, inspect_trial, returns
from analyze_job_batch import batch_records
from analyze_native_stages import ticks


LOCK_SWITCH_RETURN_RVA = 0x144E614


def collections(rows):
    active, pairs = {}, []
    for row in rows:
        kind, thread = row["kind"], row["thread"]
        if row["depth"] != "855":
            continue
        if kind == "g":
            if thread in active:
                raise ValueError("Overlapping stock tasklet collection")
            active[thread] = row
        elif kind == "l":
            before = active.pop(thread)
            old_size, size, old_cap, cap = (int(value) for value in
                (before["stack"], row["stack"], before["detail"], row["detail"]))
            if not (0 <= old_size <= size <= cap and old_size <= old_cap <= cap):
                raise ValueError("Invalid job-array sizes across stock collection")
            pairs.append((before, row))
    return pairs


def collect_for_batches(pairs, batches):
    by_thread = defaultdict(list)
    for pair in pairs:
        by_thread[pair[0]["thread"]].append(pair)
    for thread_pairs in by_thread.values():
        thread_pairs.sort(key=lambda pair: ticks(pair[1]))
    ends = {thread: [ticks(after) for _, after in thread_pairs] for thread, thread_pairs in by_thread.items()}
    matches = {}
    for batch in batches:
        submit = batch.submit
        thread = submit["thread"]
        index = bisect_right(ends.get(thread, ()), ticks(submit)) - 1
        if index < 0:
            raise ValueError("Submitted tasklets have no preceding collection on their owner thread")
        before, after = by_thread[thread][index]
        if int(after["stack"]) - int(before["stack"]) != batch.members.total() or ticks(after) in matches:
            raise ValueError("Tasklet collection does not match exact submitted job count")
        matches[ticks(after)] = (before, batch)
    return matches.values()


def native_pairs(rows):
    queued = defaultdict(deque)
    resolved = {}
    for row in rows:
        if row["depth"] != "1":
            continue
        stack, kind = row["stack"], row["kind"]
        if kind == "Q":
            queued[stack].append(row)
        elif kind == "P" and queued[stack]:
            resolved[stack, ticks(queued[stack].popleft())] = row["name3"]
    return resolved


def inspect_release(rows, ms):
    data, *_, holders = inspect_trial(rows, ms)
    spans = list(holder_native_spans(rows, holders))
    by_stack = defaultdict(list)
    for row in rows:
        if row["stack"] in holders:
            by_stack[row["stack"]].append(row)
    calls = native_pairs(rows)
    pairs = collections(rows)
    batches = [batch for batch in batch_records(rows) if batch.head]
    collect_starts = sorted(ticks(before) for before, _ in pairs)
    submitted = defaultdict(list)
    for before, batch in collect_for_batches(pairs, batches):
        for member, count in batch.members.items():
            if count == 1:
                submitted[member].append((before, batch))
    counts, sources, next_calls = Counter(), Counter(), Counter()
    durations = defaultdict(list)
    counts["collections"] = len(pairs)
    counts["joined_batches"] = len(batches)
    for before, after in pairs:
        elapsed = (ticks(after) - ticks(before)) * ms
        durations["collect_ms"].append(elapsed)
        if int(after["detail"]) > int(before["detail"]):
            counts["job_array_growth"] += 1
            durations["collection_with_growth_ms"].append(elapsed)
    tokens = {token for _, _, misses, _ in returns(rows, data["dyn_tail"]) for _, token in misses}
    for queued, entered, _ in spans:
        if (entered["name1"].lower(), entered["name3"].lower()) != ("objectreference", "getworldspace"):
            continue
        counts["holder_worldspace"] += 1
        stack = entered["stack"]
        relevant = by_stack[stack]
        body = [row for row in relevant if ticks(entered) <= ticks(row) and row["kind"] in {"N", "O"} and
                row["depth"] == "1" and row["name3"] == entered["name3"] and row["thread"] == entered["thread"]][:2]
        if not len(body) == 2 or [row["kind"] for row in body] != ["N", "O"]:
            counts["unmatched_native_return"] += 1
            continue
        native, returned = body
        durations["native_marshalling_ms"].append(int(native["detail"]) * ms)
        segment = [row for row in relevant if ticks(returned) <= ticks(row)]
        following_queue = next((row for row in segment if row["kind"] == "Q" and row["depth"] == "1"), None)
        if following_queue:
            segment = [row for row in segment if ticks(row) < ticks(following_queue)]
        marked = [row for row in segment if row["kind"] in {"R", "r", "T"} and row["depth"] == "0"]
        if len(marked) < 3 or [row["kind"] for row in marked[:3]] != ["R", "r", "T"] or len({r["detail"] for r in marked[:3]}) != 1:
            counts["unmatched_return_tasklet"] += 1
            continue
        enqueue, published, accepted = marked[:3]
        if enqueue["thread"] != returned["thread"] or published["thread"] != enqueue["thread"]:
            counts["unmatched_return_tasklet"] += 1
            continue
        counts["matched_return_tasklet"] += 1
        durations["publication_to_accept_ms"].append((ticks(accepted) - ticks(published)) * ms)
        matching = [(start, b) for start, b in submitted[published["detail"]]
                    if ticks(enqueue) <= ticks(b.submit) <= ticks(accepted) <= ticks(b.end)]
        if len(matching) == 1:
            collect, _ = matching[0]
            if ticks(published) <= ticks(collect):
                skipped = bisect_left(collect_starts, ticks(collect)) - bisect_left(collect_starts, ticks(published))
                counts["collected_first_opportunity" if skipped == 0 else "earlier_collection_after_publication"] += 1
                durations["publication_to_collection_ms"].append((ticks(collect) - ticks(published)) * ms)
            else:
                counts["publication_overlapped_collection"] += 1
        else:
            counts["unmatched_exact_job_membership"] += 1
        owner = next((row for row in reversed(relevant) if row["kind"] == "G" and row["detail"] in tokens and
                      ticks(row) <= ticks(queued)), None)
        release = next((row for row in relevant if owner and row["kind"] in {"X", "Z"} and
                        row["detail"] == owner["detail"] and ticks(owner) < ticks(row)), None)
        if not release or release["thread"] != accepted["thread"] or ticks(release) < ticks(accepted) or release not in segment:
            counts["unmatched_root_release"] += 1
            continue
        marker = next((row for row in segment if row["kind"] == "~" and row["thread"] == release["thread"] and
                       ticks(row) >= ticks(release)), None)
        if not marker:
            counts["unmatched_release_site"] += 1
            continue
        caller = int(marker["detail"])
        sources[caller] += 1
        durations["tasklet_accept_to_root_release_ms"].append((ticks(release) - ticks(accepted)) * ms)
        if caller == LOCK_SWITCH_RETURN_RVA and following_queue:
            next_calls[calls.get((stack, ticks(following_queue)), "unmatched")] += 1
            if any(row["kind"] == "G" and row["thread"] == release["thread"] and row["detail"] != release["detail"] and
                   ticks(row) >= ticks(release) for row in segment):
                counts["acquired_a_different_object_before_next_native"] += 1
    return data, counts, sources, next_calls, durations


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    path = args.capture
    phases, ms = parse_log(path / "vm.log")
    if (len(phases) != 4 or "Installed stock Papyrus release source and job-array growth observation" not in
            (path / "vm.log").read_text(encoding="utf-8-sig")):
        raise ValueError("Missing completed release and tasklet collection observation")
    counts, sources, next_calls = Counter(), Counter(), Counter()
    durations = defaultdict(list)
    for (trial, phase), (event_trial, rows) in zip(phases.items(), trial_events(path / "vm-events.csv"), strict=True):
        cells = {r["kind"]: r["stack"] for r in rows if r["kind"] in {"b", "c"}}
        if trial != event_trial or phase["enabled"] or len(phase["probes"]) != 32 or cells != {"b": INTERIOR, "c": EXTERIOR}:
            raise ValueError("Unexpected stock trial")
        data, activity, source, methods, samples = inspect_release(rows, ms)
        if data["open"] or data["promoted"] or not activity["holder_worldspace"]:
            raise ValueError("Incomplete or accelerated trial")
        counts.update(activity)
        sources.update(source)
        next_calls.update(methods)
        for name, values in samples.items():
            durations[name].extend(values)
        print(f"trial={trial} dyn_tail={data['dyn_tail']} activity={dict(activity)} sites={dict(source)} next={dict(methods)}")
    print(f"activity {dict(counts)}")
    print("release_sites " + " ".join(f"0x{caller:X}={count}" for caller, count in sorted(sources.items())))
    print(f"next_native_after_object_switch {dict(next_calls)}")
    for name, samples in sorted(durations.items()):
        print(f"{name} n={len(samples)} p50={median(samples):.4f} p95={percentile(samples, .95):.4f} "
              f"max={max(samples):.4f} sum={sum(samples):.4f}")


if __name__ == "__main__":
    main()
