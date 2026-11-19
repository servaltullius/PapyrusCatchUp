"""Queue waits of ALL scripts in a capture, not only the equipment handlers: per trial the number of
queued native calls that ran, their mean and median wait, and the share that ran in the same frame
(under 5 ms) or waited two frames or more."""
import csv, re, sys
from collections import defaultdict, deque
from pathlib import Path
from statistics import mean, median
for folder in map(Path, sys.argv[1:]):
    log = (folder / 'vm.log').read_text(encoding='utf-8-sig')
    ms = 1000 / int(re.search(r'qpc_hz=(\d+)', log)[1])
    enabled = {int(m[1]): m[2] == 'true' for m in re.finditer(r'gameplay window trial=(\d+) .*?enabled=(\w+)', log)}
    frame = {int(m[1]): float(m[2]) for m in re.finditer(r'catch-up trial=(\d+) .*?avg_frame_ms=([\d.]+)', log)}
    trials = defaultdict(list)
    for r in csv.DictReader((folder / 'vm-events.csv').open(encoding='utf-8-sig', newline='')):
        trials[int(r['trial'])].append(r)
    pooled = defaultdict(list)
    for trial, rows in sorted(trials.items()):
        rows.sort(key=lambda r: int(r['qpc']))
        waiting = defaultdict(deque); waits = []
        for r in rows:
            key = r['stack'], r['depth']
            if r['kind'] == 'Q':
                waiting[key].append(int(r['qpc']))
            elif r['kind'] == 'P' and waiting[key]:
                waits.append((int(r['qpc']) - waiting[key].popleft()) * ms)
        fm = 33.3
        same = sum(w < 5 for w in waits); two = sum(w > 1.5 * fm for w in waits)
        mode = 'on' if enabled.get(trial) else 'off'
        pooled[mode] += waits
        print(f'{folder.name} trial {trial} {mode}: queued calls={len(waits)} mean_wait_ms={mean(waits):.1f} median={median(waits):.1f} same_frame={same / len(waits):.1%} two_plus={two / len(waits):.1%} total_wait_s={sum(waits) / 1000:.1f}')
    for mode, waits in pooled.items():
        print(f'{folder.name} {mode}: calls={len(waits)} mean_wait_ms={mean(waits):.2f} same_frame={sum(w < 5 for w in waits) / len(waits):.1%} two_plus={sum(w > 50 for w in waits) / len(waits):.1%}')
