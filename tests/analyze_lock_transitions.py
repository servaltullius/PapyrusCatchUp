"""Match genuine object unlocks to native-return retry attempts in one game run."""

import argparse
from collections import Counter, defaultdict
from pathlib import Path

from analyze_gameplay_trace import parse_log, percentile, read_csv, show


EQUIPMENT = {"onobjectequipped", "onobjectunequipped"}


def analyze_trial(rows):
    owners = {}
    released = {}
    pending = defaultdict(dict)
    first_unlock = {}
    previous_acquire = {}
    counts = Counter()
    retries = []
    attempts = []
    interrupted = []
    roots = {}

    for row in sorted(rows, key=lambda event: int(event["qpc"])):
        qpc = int(row["qpc"])
        kind, stack, depth, token, thread = (
            row[name] for name in ("kind", "stack", "depth", "detail", "thread")
        )
        match = previous_acquire.pop(thread, None)
        if kind == "O" and depth == "0" and row["name3"].lower() in EQUIPMENT:
            roots[stack] = (qpc, row["name1"])
        if kind not in {"G", "X", "Z", "B", "L", "A"}:
            continue
        counts[kind] += 1
        key = stack, depth
        if kind in {"A", "L"}:
            unlock = first_unlock.pop((token, key), None)
            if unlock is not None:
                attempts.append((stack, qpc - unlock, kind == "A"))
        if kind == "B":
            released.pop(token, None)
        elif kind in {"X", "Z"}:
            released.pop(token, None)
            owner = owners.pop(token, None)
            if owner is not None and owner != stack:
                counts["owner_mismatch"] += 1
            elif kind == "X":
                released[token] = qpc
                for waiter in pending.get(token, ()):
                    first_unlock.setdefault((token, waiter), qpc)
        elif kind == "G":
            unlock = released.pop(token, None)
            if token in owners:
                counts["acquire_without_release"] += 1
            owners[token] = stack
            if token not in pending:
                continue
            if any(waiter == stack for waiter, _ in pending[token]):
                previous_acquire[thread] = (token, stack, qpc, unlock, len(pending[token]))
            elif unlock is not None:
                interrupted.append((qpc - unlock, {waiter for waiter, _ in pending[token]}))
        elif kind == "L":
            # A failed acquire contradicts a free interval still open in the log.
            if released.pop(token, None) is not None:
                counts["failure_after_observed_unlock"] += 1
            pending[token][key] = qpc
        elif kind == "A" and token in pending and key in pending[token]:
            last_miss = pending[token].pop(key)
            if not pending[token]:
                del pending[token]
            counts["retried_success"] += 1
            # The new-owner transition and its result are consecutive on this thread.
            if match is None or match[:2] != (token, stack) or not last_miss <= match[2] <= qpc:
                continue
            counts["matched_acquire"] += 1
            _, _, acquired_at, unlock, waiters = match
            if unlock is not None and last_miss <= unlock <= acquired_at:
                retries.append((stack, acquired_at - unlock, acquired_at - last_miss, waiters))

    return counts, retries, interrupted, roots, attempts


def analyze_retry_passes(rows):
    candidate = {}
    active = {}
    counts = Counter()
    for row in sorted(rows, key=lambda event: int(event["qpc"])):
        kind, stack, thread = row["kind"], row["stack"], row["thread"]
        if kind in {"D", "d"}:
            counts["candidates"] += 1
            counts["advanced"] += kind == "d"
            candidate[thread] = stack
        elif kind == "S":
            counts["passes"] += 1
            if thread in candidate:
                active[thread] = candidate.pop(thread)
        elif kind in {"L", "A"} and active.get(thread) == stack:
            counts["head_succeeded" if kind == "A" else "head_failed"] += 1
            del active[thread]
        elif kind == "s" and thread in active:
            counts["head_not_attempted"] += 1
            del active[thread]
    if candidate or active:
        raise ValueError("Candidate did not pair with a completed retry pass")
    return counts


def describe(label, ticks, milliseconds):
    values = [value * milliseconds for value in ticks]
    print(f"{label} n={len(values)} p50_ms={show(percentile(values, .5))} "
          f"p95_ms={show(percentile(values, .95))} max_ms={show(max(values, default=None))} "
          f"over_1ms={sum(value > 1 for value in values)} over_5ms={sum(value > 5 for value in values)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    log = (args.capture / "vm.log").read_text(encoding="utf-8-sig")
    if "gameplay trace complete" not in log or "with ownership transitions" not in log:
        raise ValueError("Missing completed lock transition capture")
    phases, milliseconds = parse_log(args.capture / "vm.log")
    trials = defaultdict(list)
    for row in read_csv(args.capture / "vm-events.csv"):
        trials[int(row["trial"])].append(row)
    total = Counter()
    all_retries, equip_retries, tail_retries, retakes, tail_retakes = [], [], [], [], []
    tails_with_misses = 0
    mode_counts = defaultdict(Counter)
    mode_samples = defaultdict(list)
    mode_attempts = defaultdict(list)
    for trial, rows in sorted(trials.items()):
        counts, retries, interrupted, roots, attempts = analyze_trial(rows)
        if not roots:
            continue
        tail = max(roots, key=roots.get)
        tail_samples = [gap for stack, gap, _, _ in retries if stack == tail]
        tail_misses = sum(row["stack"] == tail and row["kind"] == "L" for row in rows)
        tails_with_misses += bool(tail_misses)
        tail_interruptions = [gap for gap, waiters in interrupted if tail in waiters]
        mode = "on" if phases[trial]["enabled"] else "off"
        passes = analyze_retry_passes(rows)
        mode_counts[mode].update(passes)
        mode_samples[mode].extend(gap for stack, gap, _, _ in retries if stack in roots)
        mode_attempts[mode].extend((gap, ok) for stack, gap, ok in attempts if stack in roots)
        print(f"trial={trial} tail={roots[tail][1]} matched_free={len(retries)} "
              f"tail_misses={tail_misses} tail_matched_free={len(tail_samples)} "
              f"tail_max_free_ms={show(max(tail_samples) * milliseconds if tail_samples else None)} "
              f"tail_retaken={len(tail_interruptions)} "
              f"tail_max_retaken_ms={show(max(tail_interruptions) * milliseconds if tail_interruptions else None)}")
        total.update(counts)
        all_retries.extend(retries)
        equip_retries.extend(sample for sample in retries if sample[0] in roots)
        tail_retries.extend(tail_samples)
        retakes.extend(gap for gap, _ in interrupted)
        tail_retakes.extend(tail_interruptions)

    print("raw", " ".join(f"{key}={value}" for key, value in sorted(total.items())))
    if not all_retries:
        raise ValueError("No return retries matched to an unlock")
    for label, samples in (("all", all_retries), ("equipment", equip_retries)):
        describe(f"{label} final_unlock_to_successful_retry", [gap for _, gap, _, _ in samples], milliseconds)
        describe(f"{label} last_failed_to_successful_retry", [gap for _, _, gap, _ in samples], milliseconds)
        print(f"{label} multiple_observed_return_waiters={sum(waiters > 1 for _, _, _, waiters in samples)}")
    describe("last_equipment_stacks final_unlock_to_successful_retry", tail_retries, milliseconds)
    describe("unlock_to_other_stack_acquire_while_return_pending", retakes, milliseconds)
    describe("last_equipment_stacks unlock_to_other_stack_acquire_while_return_pending", tail_retakes, milliseconds)
    print(f"last_equipment_stacks_with_lock_misses={tails_with_misses}")
    for mode, counts in sorted(mode_counts.items()):
        attempts = mode_attempts[mode]
        describe(f"{mode} equipment first_unlock_to_next_attempt", [gap for gap, _ in attempts], milliseconds)
        print(f"{mode} equipment next_attempt_succeeded={sum(ok for _, ok in attempts)} "
              f"next_attempt_failed={sum(not ok for _, ok in attempts)}")
        if counts:
            print(mode, " ".join(f"{key}={value}" for key, value in sorted(counts.items())))
            describe(f"{mode} equipment final_unlock_to_successful_retry", mode_samples[mode], milliseconds)


if __name__ == "__main__":
    main()
