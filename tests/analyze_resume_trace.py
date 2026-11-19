"""Separate native dispatch, VM return handling, and queued tasklet resumption."""

import argparse
import re
from collections import Counter, defaultdict, deque
from pathlib import Path

from analyze_gameplay_trace import percentile, read_csv, show


def identity(row):
    return row["depth"], row["name1"].lower(), row["name3"].lower()


def analyze(rows, milliseconds):
    waiting = defaultdict(deque)
    active = {}
    native = []
    ready = defaultdict(deque)
    resumes = []
    for row in sorted(rows, key=lambda event: int(event["qpc"])):
        qpc = int(row["qpc"])
        key = identity(row)
        depth = row["depth"]
        match row["kind"]:
            case "Q":
                waiting[depth].append(qpc)
            case "P":
                queued = waiting[depth].popleft() if waiting[depth] else qpc
                active[key] = {"queued": queued, "push": qpc, "lock_misses": []}
            case "M":
                if key in active:
                    active[key]["marshal"] = qpc
            case "N":
                if key in active and "marshal" in active[key]:
                    active[key]["return"] = qpc
                    active[key]["body_ticks"] = int(row["detail"])
            case "L":
                for frame, span in active.items():
                    if frame[0] == depth and "return" in span:
                        span["lock_misses"].append(qpc)
            case "O":
                span = active.pop(key, None)
                if span and "return" in span:
                    native.append((
                        f"{key[1]}.{key[2]}",
                        (span["push"] - span["queued"]) * milliseconds,
                        (span["marshal"] - span["push"]) * milliseconds,
                        span["body_ticks"] * milliseconds,
                        (qpc - span["return"]) * milliseconds,
                        (qpc - span["push"]) * milliseconds,
                        span["lock_misses"],
                    ))
            case "R":
                ready[depth].append(qpc)
            case "T":
                if ready[depth]:
                    resumes.append((qpc - ready[depth].popleft()) * milliseconds)
    return native, resumes


def describe(label, values):
    print(f"{label} n={len(values)} p50={show(percentile(values, .5))} "
          f"p95={show(percentile(values, .95))} max={show(max(values, default=None))} "
          f"over_20ms={sum(value > 20 for value in values)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--partial", action="store_true")
    args = parser.parse_args()
    log = (args.capture / "vm.log").read_text(encoding="utf-8-sig")
    if not args.partial and "gameplay trace complete" not in log:
        raise ValueError("Incomplete trace")
    milliseconds = 1000 / int(re.search(r"qpc_hz=(\d+)", log)[1])
    lock_trace = "Installed native dispatch, return lock" in log
    trials = defaultdict(lambda: defaultdict(list))
    for row in read_csv(args.capture / "vm-events.csv"):
        trials[int(row["trial"])][row["stack"]].append(row)

    all_native, all_resumes, equipment_native, equipment_resumes = [], [], [], []
    counts = Counter()
    roots = {"onobjectequipped", "onobjectunequipped"}
    for trial, stacks in sorted(trials.items()):
        outcomes = []
        for stack, rows in stacks.items():
            counts.update(row["kind"] for row in rows)
            native, resumes = analyze(rows, milliseconds)
            all_native.extend(native)
            all_resumes.extend(resumes)
            ends = [row for row in rows if row["kind"] == "O" and row["depth"] == "0"
                    and row["name3"].lower() in roots]
            if ends:
                root = max(ends, key=lambda row: int(row["qpc"]))
                equipment_native.extend(native)
                equipment_resumes.extend(resumes)
                outcomes.append((int(root["qpc"]), stack, root["name1"], native, resumes))
        for _, stack, name, native, resumes in sorted(outcomes, reverse=True)[:3]:
            longest = sorted(native, key=lambda call: call[5], reverse=True)[:2]
            formatted = " ".join(
                f"{method}(queue={queue:.3f},before={before:.3f},body={body:.3f},after={after:.3f},misses={len(misses)})"
                for method, queue, before, body, after, _, misses in longest
            )
            print(f"trial={trial} stack={stack} {name} resume_max_ms={show(max(resumes, default=None))} {formatted}")

    if not all_native or not all_resumes:
        raise ValueError("Missing matched native calls or tasklet resumptions")
    print("raw", " ".join(f"{kind}={counts[kind]}" for kind in ("M", "N", "n", "R", "T", "L")))
    for group, native, resumes in (
        ("all", all_native, all_resumes), ("equipment", equipment_native, equipment_resumes)
    ):
        for i, label in enumerate(("queue_to_push", "push_to_marshal", "marshal", "return_to_pop"), start=1):
            describe(f"{group} {label}_ms", [call[i] for call in native])
        describe(f"{group} ready_to_resume_ms", resumes)
        if lock_trace:
            slow = [call for call in native if call[4] > 20]
            locked = [call for call in slow if call[6]]
            print(f"{group} return_over_20ms={len(slow)} with_lock_misses={len(locked)} "
                  f"max_misses_in_slow_call={max((len(call[6]) for call in locked), default=0)}")
            retry_intervals = [
                (later - earlier) * milliseconds
                for call in locked for earlier, later in zip(call[6], call[6][1:])
            ]
            describe(f"{group} repeated_lock_miss_interval_ms", retry_intervals)
        for method, queue, before, body, after, span, misses in sorted(
            native, key=lambda call: call[5], reverse=True
        )[:5]:
            print(f"{group} slow {method} span_ms={span:.3f} queue_ms={queue:.3f} "
                  f"before_ms={before:.3f} marshal_ms={body:.3f} after_ms={after:.3f} lock_misses={len(misses)}")


if __name__ == "__main__":
    main()
