"""Per-trial frame intervals from the engine render counter (engine-render.csv) of a gameplay trace capture.

Works when PresentMon rows cannot be matched to engine frames, e.g. under CPU starvation.
The counter is polled about every 1.5 ms, so single intervals carry that much timing error.
"""

import argparse
import math
import sys
from collections import defaultdict
from pathlib import Path
from statistics import mean, median

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_gameplay_trace import parse_log, read_csv  # noqa: E402


def percentile(samples, p):
    return sorted(samples)[math.ceil(len(samples) * p) - 1] if samples else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    phases, ms = parse_log(args.capture / "vm.log")
    engine = [(int(row["qpc"]), int(row["frame"])) for row in read_csv(args.capture / "engine-render.csv")]
    groups = defaultdict(list)
    print("trial mode frames avg_ms median_ms p95_ms p99_ms max_ms")
    for trial, phase in phases.items():
        samples = [(qpc, frame) for qpc, frame in engine if phase["start"] <= qpc < phase["end"]]
        intervals = []
        for (q0, f0), (q1, f1) in zip(samples, samples[1:]):
            # One interval per observed counter step, divided evenly if the poll missed frames.
            steps = f1 - f0
            if steps > 0:
                intervals.extend([(q1 - q0) * ms / steps] * steps)
        mode = "on" if phase["enabled"] else "off"
        groups[mode].extend(intervals)
        print(trial, mode, len(intervals), f"{mean(intervals):.3f}", f"{median(intervals):.3f}",
              f"{percentile(intervals, .95):.3f}", f"{percentile(intervals, .99):.3f}", f"{max(intervals):.3f}")
    for mode in ("off", "on"):
        values = groups[mode]
        print(f"{mode}: frames={len(values)} avg_ms={mean(values):.3f} median_ms={median(values):.3f} "
              f"p95_ms={percentile(values, .95):.3f} p99_ms={percentile(values, .99):.3f} max_ms={max(values):.3f}")


if __name__ == "__main__":
    main()
