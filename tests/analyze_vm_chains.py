"""Compare native waits on the final equipment and cell-transition handlers."""

import argparse
from collections import Counter, defaultdict
from pathlib import Path

from analyze_gameplay_trace import frame_metrics, parse_log, percentile, read_csv, show
from analyze_resume_trace import analyze


EQUIP = {"onobjectequipped", "onobjectunequipped"}
CELL = {"oncellattach", "oncelldetach", "oncellload", "onload", "onunload", "onlocationchange"}


def render_summary(label, presents, engine, phase, ms):
    frames = [frame for qpc, frame in engine if phase["start"] <= qpc < phase["end"]]
    if not frames:
        raise ValueError(f"Missing render counter samples for {label}")
    count = frames[-1] - frames[0] + 1
    fps = count * 1000 / ((phase["end"] - phase["start"]) * ms)
    try:
        _, presents_count, periods = frame_metrics(presents, engine, phase, ms)
    except ValueError as error:
        print(f"    {label}: frames={count} fps={show(fps)} p99 unavailable ({error})")
        return
    print(f"    {label}: frames={count} fps={show(fps)} present_samples={presents_count} "
          f"frame_p99_ms={show(percentile(periods[0], .99))}")


def summary(directory, events, signal, settle=None):
    phases, ms = parse_log(directory / "vm.log")
    if settle:
        presents = read_csv(directory / "present.csv")
        engine = [(int(row["qpc"]), int(row["frame"])) for row in read_csv(directory / "engine-render.csv")]
    trials = defaultdict(list)
    for row in read_csv(directory / "vm-events.csv"):
        if not phases[int(row["trial"])]["enabled"]:
            trials[int(row["trial"])].append(row)

    native_total, tails, adjacent, near_tail_pairs = Counter(), Counter(), Counter(), Counter()
    for trial, rows in sorted(trials.items()):
        rows.sort(key=lambda row: int(row["qpc"]))
        signals = [int(row["qpc"]) for row in rows if row["kind"] in signal]
        if not signals:
            raise ValueError(f"Missing workload signal in {directory} trial {trial}")
        start = signals[0]
        settled = next((int(row["qpc"]) for row in rows if row["kind"] == settle), None)
        if settle and settled is None:
            raise ValueError(f"Missing cell arrival in {directory} trial {trial}")
        stable = next((int(row["qpc"]) for row in rows if row["kind"] == "v"), None)
        if settle and stable is None:
            raise ValueError(f"Missing post-load measurement in {directory} trial {trial}")
        by_stack = defaultdict(list)
        starts, completed = {}, []
        queued = {}
        for row in rows:
            at = int(row["qpc"])
            stack, kind = row["stack"], row["kind"]
            by_stack[stack].append(row)
            if kind == "Q" and row["depth"] == "0":
                queued.setdefault(stack, at)
            if row["depth"] != "0" or row["name3"].lower() not in events:
                continue
            root_start = queued.get(stack, at) if settle else at
            if kind == "P" and root_start >= start and (stable is None or root_start < stable):
                starts[stack] = row
            elif kind == "O" and stack in starts:
                completed.append((starts.pop(stack), row))
        if not completed:
            print(f"  {trial}: no tracked roots completed, open={len(starts)}")
            continue

        print(f"  {trial}: roots={len(completed)} open={len(starts)} after_signal_ms="
              f"{show((int(completed[-1][1]['qpc']) - signals[-1]) * ms)}"
              f" ready_probe_p95_ms={show(percentile(phases[trial]['probes'], .95))}")
        if settled is not None:
            print(f"    cell_ready_sample_ms={show((settled - start) * ms)}")
            phase = {**phases[trial], "start": stable}
            render_summary("post_load", presents, engine, phase, ms)
            phase["start"] = phase["end"] - round(2000 / ms)
            render_summary("last_two_seconds", presents, engine, phase, ms)
        for index, (push, end) in enumerate(completed):
            stack = end["stack"]
            trace = by_stack[stack]
            native, _ = analyze(trace, ms)
            delayed = [(name, queue) for name, queue, *_ in native if queue >= 1]
            for name, queue in delayed:
                native_total[name] += queue
            names = [call[0] if call[1] >= 1 else None for call in native]
            pairs = [(a, b) for a, b in zip(names, names[1:]) if a and b]
            if len(completed) - index <= 3:
                near_tail_pairs.update(pairs)
            if index == len(completed) - 1:
                root_duration = (int(end["qpc"]) - int(push["qpc"])) * ms
                after_settle = (int(end["qpc"]) - settled) * ms if settled is not None else None
                print(f"    tail={end['name1']}.{end['name3']} stack={stack} root_duration_ms={show(root_duration)} "
                      f"after_ready_ms={show(after_settle)} native_queue_ms={show(sum(q for _, q in delayed))} "
                      f"native_dispatch_ms={show(sum(call[3] for call in native))}")
                print("    chain", " ".join(f"{name}={show(queue)}" for name, queue in delayed))
                tails.update(sorted({name for name, _ in delayed}))
                # Adjacent native dispatches in the trace can still have intervening bytecode.
                adjacent.update(pairs)
    for label, counter in (("root_native_wait_ms", native_total), ("tail_presence", tails), ("adjacent_tail_calls", adjacent)):
        print(f"  {label}: " + " ".join(f"{name}={value:.3f}" for name, value in counter.most_common(10)))
    return tails, adjacent, near_tail_pairs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("equipment", type=Path)
    parser.add_argument("cells", type=Path)
    args = parser.parse_args()
    print("Equipment (stock order)")
    equipment, equipment_pairs, equipment_near = summary(args.equipment, EQUIP, {"E", "U"})
    print("Cell transitions (stock order)")
    cells, cell_pairs, cell_near = summary(args.cells, CELL, {"b"}, "c")
    print("Common delayed native methods in tails:", sorted(equipment.keys() & cells.keys()))
    print("Common adjacent delayed native pairs in tails:", sorted(equipment_pairs.keys() & cell_pairs.keys()))
    print("Common pairs when including the last three roots (equipment, cells):")
    for pair in sorted(equipment_near.keys() & cell_near.keys()):
        print(f"  {pair}: {equipment_near[pair]}, {cell_near[pair]}")


if __name__ == "__main__":
    main()
