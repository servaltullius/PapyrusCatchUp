import csv, re, sys
from pathlib import Path
folder = Path(sys.argv[1]); want = int(sys.argv[2])
log = (folder / 'vm.log').read_text(encoding='utf-8-sig')
ms = 1000 / int(re.search(r'qpc_hz=(\d+)', log)[1])
rows = [r for r in csv.DictReader((folder / 'vm-events.csv').open(encoding='utf-8-sig', newline='')) if int(r['trial']) == want]
rows.sort(key=lambda r: int(r['qpc']))
signals = [int(r['qpc']) for r in rows if r['kind'] in ('E', 'U')]
roots, ended = {}, []
for r in rows:
    key = r['stack'], r['depth']; m = r['name3'].lower(); q = int(r['qpc'])
    if r['kind'] == 'P' and m in ('onobjectequipped', 'onobjectunequipped') and q >= signals[0]:
        roots[key] = q
    elif r['kind'] == 'O' and key in roots and m in ('onobjectequipped', 'onobjectunequipped'):
        ended.append((q, key, roots.pop(key)))
end, (stack, depth), start = max(ended)
print(f'{folder.name} trial {want}: last handler stack {stack}, {((end - start) * ms):.0f} ms')
prev = start
for r in rows:
    q = int(r['qpc'])
    if r['stack'] == stack and start <= q <= end and r['kind'] in ('Q', 'P'):
        print(f"  +{(q - start) * ms:7.1f} ms  d={r['depth']} {r['kind']} {r['name1']}.{r['name3']} {r['detail']}")
