"""Which equipment handlers finish last in each trial, and what the last one waited on.

Usage: python tail_chain.py <capture folder> [...]
For every trial: the three handlers that end last, and for the one that ends last the queued calls it
made in order (Q -> P pairs on its stack) with how long each waited in the queue.
"""
import csv
import re
import sys
from collections import defaultdict, deque
from pathlib import Path

for folder in map(Path, sys.argv[1:]):
    log = (folder / 'vm.log').read_text(encoding='utf-8-sig')
    ms = 1000 / int(re.search(r'qpc_hz=(\d+)', log)[1])
    enabled = {int(m[1]): m[2] for m in re.finditer(r'gameplay window trial=(\d+) .*?enabled=(\w+)', log)}
    trials = defaultdict(list)
    with (folder / 'vm-events.csv').open(encoding='utf-8-sig', newline='') as handle:
        for row in csv.DictReader(handle):
            trials[int(row['trial'])].append(row)
    last_names = defaultdict(int)
    for trial, events in sorted(trials.items()):
        events.sort(key=lambda r: int(r['qpc']))
        signals = [int(r['qpc']) for r in events if r['kind'] in ('E', 'U')]
        if not signals:
            continue
        roots, ended = {}, []
        for row in events:
            key = row['stack'], row['depth']
            method = row['name3'].lower()
            name = f"{row['name1']}.{row['name3']}"
            qpc = int(row['qpc'])
            if row['kind'] == 'P' and method in ('onobjectequipped', 'onobjectunequipped') and qpc >= signals[0]:
                roots[key] = (name, qpc)
            elif row['kind'] == 'O' and key in roots and roots[key][0] == name:
                ended.append((qpc, key, roots[key][0], roots.pop(key)[1]))
        ended.sort()
        tail = ended[-3:]
        drain = (ended[-1][0] - signals[-1]) * ms
        print(f"{folder.name} trial {trial} enabled={enabled.get(trial)} handlers={len(ended)} drain_ms={drain:.1f} "
              f"last three: {', '.join(t[2] for t in tail)}")
        end_qpc, (stack, depth), name, start_qpc = ended[-1]
        last_names[name.split('.')[0]] += 1
        waiting = deque()
        calls = []
        for row in events:
            qpc = int(row['qpc'])
            if row['stack'] != stack or not (start_qpc <= qpc <= end_qpc):
                continue
            if row['kind'] == 'Q':
                waiting.append(qpc)
            elif row['kind'] == 'P' and waiting and int(row['depth']) > int(depth):
                calls.append((f"{row['name1']}.{row['name3']}", (qpc - waiting.popleft()) * ms))
        print('    last handler ran %.0f ms; queued calls: %s' % ((end_qpc - start_qpc) * ms,
              ' > '.join(f'{n}({w:.0f})' for n, w in calls)))
    print(f"{folder.name}: script of the last handler over all trials: {dict(last_names)}")
