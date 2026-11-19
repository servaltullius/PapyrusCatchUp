"""Summarize a catch-up A/B capture: VM catch-up counters per trial and the paired equip drain times.

Run analyze_gameplay_trace.py on the same capture for response and frame-time percentiles.
"""

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path
from statistics import median

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_gameplay_trace import parse_log, read_csv, stack_metrics  # noqa: E402


def catchup_rows(log_text):
    rows = {}
    for line in log_text.splitlines():
        if "catch-up trial=" in line:
            fields = dict(re.findall(r"(\w+)=([\w.]+)", line))
            rows[int(fields["trial"])] = fields
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    phases, ms = parse_log(args.capture / "vm.log")
    counters = catchup_rows((args.capture / "vm.log").read_text(encoding="utf-8-sig"))
    events = defaultdict(list)
    for row in read_csv(args.capture / "vm-events.csv"):
        events[int(row["trial"])].append(row)

    drains = defaultdict(list)
    # extra_calls: queued messages the extra cycles dispatched. leftover_tasklets: tasklets that were
    # already waiting when a cycle started (op limit or latent return), which its batch also ran.
    # blocked_skips: frames that started no cycle because the front call had to wait. Captures made
    # before these counters existed print "-".
    # change_call_holds: front checks that held a call only because the call itself is a change (test
    # control of the catchup-holdcalls mode).
    # skip: whether the trial ran under the skip rule (test control; catchup-skipfront turns it on in
    # "on" trials, APB_CATCHUP_SKIP=1 in every trial with catch-up). Captures made before the harness
    # logged it print "-"; there a front_skips above zero is the only sign of it.
    # order_rebuilds: extra cycles of the skip rule after which the queue had to be put in order by
    # rebuilding it; "-" for captures of builds that did not count them.
    print("trial mode skip signals matched handlers open drain_ms frames avg_frame_ms cycles with_tasklets "
          "extra_calls leftover_tasklets blocked_stops blocked_skips change_call_holds front_skips order_rebuilds "
          "idle_stops time_stops extra_ms_per_frame max_frame_extra_ms")
    for trial, phase in phases.items():
        signals, matched, completed, opened, drain, *_ = stack_metrics(events[trial], ms)
        mode = "on" if phase["enabled"] else "off"
        if drain is not None:
            drains[mode].append(drain)
        c = counters.get(trial, {})
        print(trial, mode, c.get("skip_blocked", "-"), signals, matched, completed, opened,
              f"{drain:.3f}" if drain is not None else "n/a",
              c.get("frames", "-"), c.get("avg_frame_ms", "-"), c.get("cycles", "-"), c.get("with_tasklets", "-"),
              c.get("extra_calls", "-"), c.get("leftover_tasklets", "-"), c.get("blocked_stops", "-"),
              c.get("blocked_skips", "-"), c.get("change_call_holds", "-"), c.get("front_skips", "-"),
              c.get("order_rebuilds", "-"), c.get("idle_stops", "-"), c.get("time_stops", "-"), c.get("extra_ms_per_frame", "-"),
              c.get("max_frame_extra_ms", "-"))
    for mode in ("off", "on"):
        values = drains[mode]
        if values:
            print(f"{mode}: drain_median_ms={median(values):.3f} min={min(values):.3f} max={max(values):.3f} n={len(values)}")
    if drains["off"] and drains["on"]:
        change = median(drains["on"]) / median(drains["off"]) - 1
        print(f"drain median change on vs off: {change * 100:+.1f}%")


if __name__ == "__main__":
    main()
