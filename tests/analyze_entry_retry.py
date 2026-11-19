"""Measure function-entry lock waits and check actual same-frame late retries."""

import argparse
from collections import Counter, defaultdict
from pathlib import Path
from statistics import median

from analyze_gameplay_trace import parse_log, percentile, read_csv, show, stack_metrics
from analyze_resume_trace import analyze


ROOTS = {"onobjectequipped", "onobjectunequipped"}


def analyze_trial(rows, ms):
    pending = {}
    entry_waits = defaultdict(list)
    by_stack = defaultdict(list)
    active = {}
    advances = []
    counts = Counter()
    root_ends = []
    frames = set()
    unlocks = {}
    for row in sorted(rows, key=lambda event: int(event["qpc"])):
        qpc = int(row["qpc"])
        kind, stack, depth, thread, name, detail = (
            row[key] for key in ("kind", "stack", "depth", "thread", "name3", "detail")
        )
        by_stack[stack].append(row)
        if kind == "O" and depth == "0" and name.lower() in ROOTS:
            root_ends.append(row)
        if kind == "X":
            unlocks[detail] = qpc
        if kind not in {"I", "C", "K", "V", "J", "Y", "y", "j", "P"}:
            continue
        counts[kind] += 1
        last_miss = None
        if kind in {"I", "C"}:
            key = stack, depth, name.lower(), detail
            if kind == "I":
                first, _ = pending.get(key, (qpc, qpc))
                pending[key] = first, qpc
            elif key in pending:
                first, last_miss = pending.pop(key)
                entry_waits[stack].append((qpc - first) * ms)
        if kind == "y":
            if thread in active or detail in frames:
                raise ValueError("Multiple advanced retries in the same frame")
            frames.add(detail)
            active[thread] = (stack, depth, qpc, detail, set())
        elif thread in active:
            owner, target_depth, start, frame, events = active[thread]
            if owner != stack or target_depth != depth:
                continue
            if kind == "j":
                advances.append((stack, frame, start, "C" in events, "P" in events, int(detail) * ms))
                del active[thread]
            else:
                events.add(kind)
                if kind == "C" and last_miss is not None and last_miss <= unlocks.get(detail, -1) <= start:
                    counts["late_unlock_matched"] += 1
    if active or not root_ends:
        raise ValueError("Incomplete late retry or equipment trace")

    tail = root_ends[-1]
    stack = tail["stack"]
    signals, matched, completed, opened, drain, *_ = stack_metrics(rows, ms)
    if opened or drain is None or not completed:
        raise ValueError("Incomplete equipment trace")
    root_enter = next(row for row in by_stack[stack] if row["kind"] == "P" and row["depth"] == "0")
    root_queue = next(row for row in by_stack[stack] if row["kind"] == "Q" and row["depth"] == "0")
    calls, _ = analyze(by_stack[stack], ms)
    root_wait = (int(root_enter["qpc"]) - int(root_queue["qpc"])) * ms
    nested_wait = sum(call[1] for call in calls)
    return tail, signals, matched, completed, drain, root_wait, nested_wait, entry_waits, counts, advances


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    log = (args.capture / "vm.log").read_text(encoding="utf-8-sig")
    if "Installed same-frame ordered entry retry experiment" not in log:
        raise ValueError("Missing entry retry experiment")
    phases, ms = parse_log(args.capture / "vm.log")
    trials = defaultdict(list)
    for row in read_csv(args.capture / "vm-events.csv"):
        trials[int(row["trial"])].append(row)
    modes = defaultdict(list)
    print("trial mode matched/signals/completed tail_stack tail root_qp_ms native_qp_ms entry_lock_ms misses tasklet_passes qualifying_passes head ready advanced acquired entered unlock_matched tail_adv max_extra_ms")
    by_load = defaultdict(lambda: defaultdict(list))
    for trial, rows in sorted(trials.items()):
        tail, signals, matched, completed, drain, root, native, waits, counts, advances = analyze_trial(rows, ms)
        stack = tail["stack"]
        mode = "on" if phases[trial]["enabled"] else "off"
        lock = sum(waits[stack])
        if advances and mode == "off":
            raise ValueError("Unexpected advance while disabled")
        modes[mode].append((root, native, lock, drain, waits, counts, advances))
        by_load[completed][mode].append((root + native, lock, drain))
        print(trial, mode, f"{matched}/{signals}/{completed}", stack, tail["name1"],
              show(root), show(native), show(lock), counts["I"], counts["K"], counts["V"],
              counts["J"], counts["Y"], len(advances),
              sum(acquired for _, _, _, acquired, _, _ in advances), sum(entered for _, _, _, _, entered, _ in advances),
              counts["late_unlock_matched"],
              sum(advanced_stack == stack for advanced_stack, *_ in advances),
              show(max((elapsed for *_, elapsed in advances), default=None)))
    for mode, results in sorted(modes.items()):
        print(mode, " ".join(f"{label}={show(median(row[index] for row in results))}" for index, label in enumerate(
            ("tail_root_qp_median_ms", "tail_native_qp_median_ms", "tail_entry_lock_median_ms", "drain_median_ms")
        )), f"tail_total_qp_median_ms={show(median(row[0] + row[1] for row in results))}")
        all_waits = [wait for row in results for waits in row[4].values() for wait in waits]
        print(f"  entry_lock_samples={len(all_waits)} entry_lock_p95_ms={show(percentile(all_waits, .95))} "
              f"head_checks={sum(row[5]['J'] for row in results)} advanced={sum(len(row[6]) for row in results)}")
    for completed, groups in sorted(by_load.items()):
        if groups.keys() == {"off", "on"}:
            for mode, samples in sorted(groups.items()):
                queue, lock, drain = map(median, zip(*samples))
                print(f"same_completed={completed} {mode} n={len(samples)} tail_total_qp_ms={show(queue)} "
                      f"tail_entry_lock_ms={show(lock)} drain_ms={show(drain)}")


if __name__ == "__main__":
    main()
