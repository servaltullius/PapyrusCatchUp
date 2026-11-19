"""Summarise what a function in the unpacked AE 1.6.1170 executable writes and calls.

Usage: python review.py <tools dir> <depth> name=rva [name=rva ...]

For each function: its size, every instruction that writes to memory that is not addressed through
RSP or RBP, every lock-prefixed instruction, and every call. Direct callees are summarised the same
way down to <depth> levels. Nothing here proves a function pure; it lists what has to be read.
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
import capstone as cs
from capstone import x86

ids = skydis.ids
md = skydis.d
READ_ONLY_FIRST_OPERAND = ('cmp', 'test', 'bt', 'ucomiss', 'comiss', 'ucomisd', 'comisd', 'push', 'call', 'jmp', 'prefetch', 'nop')
seen = {}


def function_body(rva, limit=0x1800):
    out = []
    furthest = rva
    pos = rva
    while pos < rva + limit:
        chunk = skydis.read(pos, 16)
        insn = next(md.disasm(chunk, pos), None)
        if insn is None:
            break
        out.append(insn)
        pos += insn.size
        if insn.mnemonic.startswith('j') and insn.operands and insn.operands[0].type == x86.X86_OP_IMM:
            target = insn.operands[0].imm
            if rva <= target < rva + limit:
                furthest = max(furthest, target)
        if insn.mnemonic in ('ret', 'int3') or (insn.mnemonic == 'jmp' and pos > furthest):
            if pos > furthest:
                break
    return out


def summarise(rva):
    if rva in seen:
        return seen[rva]
    body = function_body(rva)
    writes, calls, tail = [], [], None
    for insn in body:
        text = f'{insn.address:X} {insn.mnemonic} {insn.op_str}'
        if insn.mnemonic == 'call' or (insn.mnemonic == 'jmp' and insn is body[-1]):
            op = insn.operands[0]
            if op.type == x86.X86_OP_IMM:
                calls.append((insn.mnemonic, op.imm))
            else:
                calls.append((insn.mnemonic, insn.op_str))
            continue
        locked = insn.prefix[0] == 0xF0
        if insn.operands and not insn.mnemonic.startswith(READ_ONLY_FIRST_OPERAND):
            op = insn.operands[0]
            if op.type == x86.X86_OP_MEM and op.mem.base not in (x86.X86_REG_RSP, x86.X86_REG_RBP) and op.mem.segment == 0:
                writes.append(('LOCK ' if locked else '') + text)
                continue
            if op.type == x86.X86_OP_MEM and op.mem.segment != 0:
                writes.append('SEG ' + text)
                continue
        if locked:
            writes.append('LOCK ' + text)
    seen[rva] = (len(body), sum(i.size for i in body), writes, calls)
    return seen[rva]


def show(name, rva, depth, indent=''):
    count, size, writes, calls = summarise(rva)
    print(f'{indent}{name} @ {rva:X} id={ids.get(rva)} insns={count} bytes={size:#x}')
    for w in writes:
        print(f'{indent}    write: {w}')
    for kind, target in calls:
        if isinstance(target, int):
            print(f'{indent}    {kind}: {target:X} id={ids.get(target)}')
        else:
            print(f'{indent}    {kind} (indirect): {target}')
    if depth > 0:
        done = set()
        for kind, target in calls:
            if isinstance(target, int) and target not in done:
                done.add(target)
                show(f'callee id={ids.get(target)}', target, depth - 1, indent + '      ')


depth = int(sys.argv[2])
for arg in sys.argv[3:]:
    name, rva = arg.split('=')
    show(name, int(rva, 16), depth)
    print()
