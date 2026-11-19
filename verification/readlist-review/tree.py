"""Size of the call tree below a function: how much would have to be read to judge it.

Usage: python tree.py <tools dir> <max depth> name=rva [...]
Prints, per root, the number of distinct functions reachable through direct calls within the depth,
their instructions, the non-stack memory writes among them (lock-prefixed ones counted apart, since
those are reference counts and locks) and the indirect calls, which this tool cannot follow.
"""
import io
import contextlib
import sys

sys.path.insert(0, sys.argv[1])
_argv = sys.argv
sys.argv = ['skydis.py', 'id:55394', '0']
with contextlib.redirect_stdout(io.StringIO()):
    import skydis
sys.argv = _argv
sys.argv = ['review.py', sys.argv[1], '0']
import importlib.util
spec = importlib.util.spec_from_file_location('review', __file__.replace('tree.py', 'review.py'))
review = importlib.util.module_from_spec(spec)
with contextlib.redirect_stdout(io.StringIO()):
    spec.loader.exec_module(review)
sys.argv = _argv
ids = skydis.ids

depth = int(sys.argv[2])
for arg in sys.argv[3:]:
    name, rva = arg.split('=')
    root = int(rva, 16)
    reach = {root: 0}
    frontier = [root]
    truncated = False
    while frontier:
        nxt = []
        for f in frontier:
            _, _, _, calls = review.summarise(f)
            for _, target in calls:
                if isinstance(target, int) and target not in reach:
                    if reach[f] + 1 > depth:
                        truncated = True
                        continue
                    reach[target] = reach[f] + 1
                    nxt.append(target)
        frontier = nxt
    insns = plain = locked = indirect = 0
    plain_list = []
    for f in reach:
        count, _, writes, calls = review.summarise(f)
        insns += count
        for w in writes:
            if w.startswith(('LOCK', 'SEG')):
                locked += 1
            else:
                plain += 1
                plain_list.append(f'{ids.get(f)}:{w.split(" ", 1)[1]}')
        indirect += sum(1 for _, t in calls if not isinstance(t, int))
    print(f'{name} root id={ids.get(root)}: functions={len(reach)} insns={insns} plain_writes={plain} lock_or_tls={locked} '
          f'indirect_calls={indirect}{" TRUNCATED" if truncated else ""}')
    for w in plain_list[:6]:
        print(f'      {w}')
