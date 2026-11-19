"""Find the callback of engine-registered Papyrus natives in the unpacked AE 1.6.1170 executable.

Usage: python natives.py <tools dir> Class.Name [Class.Name ...]

The engine's registration code builds each native as (see ID 55394):
    lea r8,  [class name]
    lea rdx, [function name]
    mov rcx, <object>
    call <NativeFunctionBase constructor>
    mov [obj], <vtable>
    lea rax, [callback]
    mov [obj+0x50], rax
For every lea of the function-name string this prints the class string loaded just before it and
the callback stored at +0x50 just after it.
"""
import io
import contextlib
import struct
import sys

sys.path.insert(0, sys.argv[1])
_argv = sys.argv
sys.argv = ['skydis.py', 'id:55394', '0']
with contextlib.redirect_stdout(io.StringIO()):
    import skydis
    from xref import data, lo, func_of
sys.argv = _argv
img = bytes(skydis.img)
ids = skydis.ids


def cstr(rva):
    end = img.find(b'\x00', rva)
    try:
        return img[rva:end].decode('ascii')
    except UnicodeDecodeError:
        return None


def lea_sites(target):
    """Offsets (RVA) of `lea reg, [rip+disp]` instructions that address target."""
    out = []
    j = 0
    while True:
        j = data.find(b'\x8d', j)
        if j < 0:
            break
        if j >= 1 and data[j - 1] in (0x48, 0x4c) and (data[j + 1] & 0xC7) == 0x05:
            disp = struct.unpack_from('<i', data, j + 2)[0]
            if lo + j + 6 + disp == target:
                out.append((lo + j - 1, data[j - 1], data[j + 1]))
        j += 1
    return out


def find(class_name, name):
    results = []
    pat = b'\x00' + name.encode() + b'\x00'
    i = 0
    while (i := img.find(pat, i)) >= 0:
        string_rva = i + 1
        i += 1
        for site, rex, modrm in lea_sites(string_rva):
            # function name goes in RDX: 48 8D 15
            if not (rex == 0x48 and modrm == 0x15):
                continue
            # class name: lea r8, [rip+..] (4C 8D 05) within the 16 bytes before
            cls = None
            for back in range(7, 40):
                p = site - back - lo
                if data[p:p + 3] == b'\x4c\x8d\x05':
                    t = lo + p + 7 + struct.unpack_from('<i', data, p + 3)[0]
                    cls = cstr(t)
                    break
            # callback: lea rax, [rip+..] (48 8D 05) followed by mov [reg+0x50], rax within 0x40 bytes after
            callback = None
            for fwd in range(7, 0x48):
                p = site + fwd - lo
                if data[p:p + 3] == b'\x48\x8d\x05':
                    nxt = data[p + 7:p + 11]
                    if nxt[0] in (0x48, 0x49) and nxt[1] == 0x89 and (nxt[2] & 0xC0) == 0x40 and nxt[3] == 0x50:
                        callback = lo + p + 7 + struct.unpack_from('<i', data, p + 3)[0]
                        break
            if callback is None:
                # other form: lea r9, [callback] (4C 8D 0D) just before the class and name, and a
                # constructor that takes the callback as its fourth argument
                for back in range(7, 48):
                    p = site - back - lo
                    if data[p:p + 3] == b'\x4c\x8d\x0d':
                        callback = lo + p + 7 + struct.unpack_from('<i', data, p + 3)[0]
                        break
            results.append((cls, site, callback))
    return [r for r in results if r[0] is not None and r[0].lower() == class_name.lower()], results


for arg in sys.argv[2:]:
    class_name, name = arg.split('.', 1)
    hits, everything = find(class_name, name)
    if not hits:
        others = sorted({str(r[0]) for r in everything})
        print(f'{arg}: NOT FOUND in the executable (same name under: {", ".join(others) or "none"})')
        continue
    for cls, site, callback in hits:
        cb = f'{callback:X} id={ids.get(callback)}' if callback else 'callback not recognised'
        print(f'{arg}: registered at {site:X} (func id={ids.get(func_of(site))}) callback {cb}')
