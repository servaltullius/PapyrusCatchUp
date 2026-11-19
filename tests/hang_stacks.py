"""Print the call stack of every thread in a minidump of the game, for a run that hung.

Usage: python tests/hang_stacks.py <stacks.dmp or full.dmp> [--first <thread id> ...] [--depth 48]
                                   [--addrlib <versionlib-*.bin>] [--names <folder with skydb.py>]

The dumps come from build/catchup/write-game-dump.ps1. Threads with the same stack are printed once
with their ids. --first puts the named threads on top (the "thread" column of the probe's probe.tsv
gives the main thread's id).

Frames are unwound with each module's unwind data, read from the module file on disk. A file that is
missing, or is not the build the dump was taken from (its link time stamp or image size differs),
gives no usable unwind data: the walk stops at the first frame in such a module and says so, because
every frame beyond it would be a guess. Analyse a dump before the DLLs it names are rebuilt.

SkyrimSE.exe frames are labelled with the nearest Address Library ID at or below the address. The
library is looked for beside the SKSE plugins the dump names; --addrlib names it directly. With
--names, the folder of a "skydb.py" resolver (function names for IDs) is used for those labels.
"""

import argparse
import bisect
import glob
import mmap
import os
import struct
import sys
from collections import defaultdict

THREAD_LIST, MODULE_LIST, MEMORY_LIST, MEMORY64_LIST = 3, 4, 5, 9
REGISTERS = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]
CONTEXT_OFFSET = {name: 0x78 + 8 * index for index, name in enumerate(REGISTERS)}
CONTEXT_OFFSET["rip"] = 0xF8

# Modules of this project: a thread with one of them on its stack is marked and printed early.
OWN_MODULES = ("apbquestprobe.dll", "apbgameplaytrace.dll", "adaptivepapyrusbudget.dll", "apbtestsaveloader.dll")


class Module:
    def __init__(self, base, size, stamp, path, version):
        self.base, self.end, self.size, self.stamp, self.path, self.version = base, base + size, size, stamp, path, version
        self.name = os.path.basename(path)
        self._image = None
        self._problem = None
        self.checked = False  # whether a walk needed this module's file

    def image(self):
        """The module file's unwind data, or None with self.problem() saying why it cannot be used."""
        if not self.checked:
            self.checked = True
            try:
                image = Image(self.path)
            except (OSError, ValueError, struct.error) as error:
                self._problem = f"file not readable ({error.__class__.__name__})"
            else:
                if image.stamp != self.stamp or image.size != self.size:
                    self._problem = "the file on disk is another build than the one in the dump"
                else:
                    self._image = image
        return self._image

    def problem(self):
        self.image()
        return self._problem


class Dump:
    """x64 minidump: modules, thread registers and the memory the dump holds."""

    def __init__(self, path):
        self.file = open(path, "rb")
        self.data = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)
        signature, _, count, directory = struct.unpack_from("<IIII", self.data, 0)
        if signature != 0x504D444D:
            raise ValueError("not a minidump")
        self.streams = {}
        for index in range(count):
            kind, size, offset = struct.unpack_from("<III", self.data, directory + 12 * index)
            self.streams.setdefault(kind, (offset, size))
        self.modules = self._modules()
        self._bases = [module.base for module in self.modules]
        self.threads = self._threads()
        self._ranges = self._memory()
        self._starts = [entry[0] for entry in self._ranges]

    def _text(self, offset):
        length = struct.unpack_from("<I", self.data, offset)[0]
        return self.data[offset + 4: offset + 4 + length].decode("utf-16-le", "replace")

    def _modules(self):
        offset, _ = self.streams[MODULE_LIST]
        modules = []
        for index in range(struct.unpack_from("<I", self.data, offset)[0]):
            # MINIDUMP_MODULE: base, size, checksum, time stamp, name, then VS_FIXEDFILEINFO.
            entry = offset + 4 + 108 * index
            base, size, _, stamp, name = struct.unpack_from("<QIIII", self.data, entry)
            high, low = struct.unpack_from("<II", self.data, entry + 32)
            modules.append(Module(base, size, stamp, self._text(name), (high >> 16, high & 0xFFFF, low >> 16, low & 0xFFFF)))
        return sorted(modules, key=lambda module: module.base)

    def _threads(self):
        offset, _ = self.streams[THREAD_LIST]
        threads = {}
        for index in range(struct.unpack_from("<I", self.data, offset)[0]):
            entry = offset + 4 + 48 * index
            thread, _, _, _, _, start, size, memory, _, context = struct.unpack_from("<IIIIQQIIII", self.data, entry)
            registers = {name: struct.unpack_from("<Q", self.data, context + at)[0] for name, at in CONTEXT_OFFSET.items()}
            threads[thread] = {"registers": registers, "stack": (start, size, memory)}
        return threads

    def _memory(self):
        ranges = []
        if MEMORY64_LIST in self.streams:
            offset, _ = self.streams[MEMORY64_LIST]
            count, position = struct.unpack_from("<QQ", self.data, offset)
            for index in range(count):
                start, size = struct.unpack_from("<QQ", self.data, offset + 16 + 16 * index)
                ranges.append((start, size, position))
                position += size
        if MEMORY_LIST in self.streams:
            offset, _ = self.streams[MEMORY_LIST]
            for index in range(struct.unpack_from("<I", self.data, offset)[0]):
                ranges.append(struct.unpack_from("<QII", self.data, offset + 4 + 16 * index))
        for thread in self.threads.values():
            # dbghelp leaves the file offset of a thread's stack at zero when the stack bytes are
            # stored in a memory list (dumps with all memory, or with indirectly referenced memory).
            # Offset zero is the file header, so such a descriptor is not a memory range.
            if thread["stack"][2] != 0:
                ranges.append(thread["stack"])
        return sorted(ranges)

    def read(self, address, size):
        index = bisect.bisect_right(self._starts, address) - 1
        # Ranges can overlap (a stack and the memory around the addresses it points at), so a few
        # ranges that start earlier may still hold the address.
        for index in range(index, max(index - 64, -1), -1):
            start, length, offset = self._ranges[index]
            if start <= address and address + size <= start + length:
                return self.data[offset + address - start: offset + address - start + size]
        return None

    def pointer(self, address):
        data = self.read(address, 8)
        return None if data is None else struct.unpack("<Q", data)[0]

    def module_of(self, address):
        index = bisect.bisect_right(self._bases, address) - 1
        return self.modules[index] if index >= 0 and address < self.modules[index].end else None


class Image:
    """Function table (.pdata) and unwind data of a PE32+ file."""

    def __init__(self, path):
        with open(path, "rb") as file:
            self.data = file.read()
        data = self.data
        header = struct.unpack_from("<I", data, 0x3C)[0]
        sections = struct.unpack_from("<H", data, header + 6)[0]
        self.stamp = struct.unpack_from("<I", data, header + 8)[0]
        optional_size = struct.unpack_from("<H", data, header + 20)[0]
        optional = header + 24
        if struct.unpack_from("<H", data, optional)[0] != 0x20B:
            raise ValueError("not a PE32+ file")
        self.size = struct.unpack_from("<I", data, optional + 56)[0]
        table, table_size = struct.unpack_from("<II", data, optional + 112 + 3 * 8)
        self.sections = []
        for index in range(sections):
            entry = header + 24 + optional_size + index * 40
            virtual_size, address, raw_size, raw = struct.unpack_from("<IIII", data, entry + 8)
            self.sections.append((address, max(virtual_size, raw_size), raw))
        self.functions = []
        if table:
            start = self.offset(table)
            for index in range(table_size // 12):
                begin, end, unwind = struct.unpack_from("<III", data, start + 12 * index)
                if begin:
                    self.functions.append((begin, end, unwind))
        self.functions.sort()
        self.begins = [function[0] for function in self.functions]

    def offset(self, rva):
        for address, size, raw in self.sections:
            if address <= rva < address + size:
                return raw + rva - address
        raise ValueError(f"RVA {rva:#x} is in no section")

    def function(self, rva):
        index = bisect.bisect_right(self.begins, rva) - 1
        return self.functions[index] if index >= 0 and rva < self.functions[index][1] else None


def step(dump, registers):
    """Unwinds one frame (x64 unwind codes). Returns the caller's registers, or a text saying why
    the walk cannot go on from here."""
    rip = registers["rip"]
    module = dump.module_of(rip)
    if not module:
        return "this address is in no module"
    image = module.image()
    if not image:
        return f"{module.name}: {module.problem()}"
    registers = dict(registers)
    function = image.function(rip - module.base)
    if function is None:  # a leaf function: the return address is at [rsp]
        caller = dump.pointer(registers["rsp"])
        if caller is None:
            return "the stack memory ends here"
        registers["rip"], registers["rsp"] = caller, registers["rsp"] + 8
        return registers
    inside = rip - module.base - function[0]
    unwind = function[2]
    machine_frame = False
    for depth in range(32):
        at = image.offset(unwind & ~1)
        data = image.data
        flags, count, frame = data[at] >> 3, data[at + 2], data[at + 3]
        frame_register, frame_offset = frame & 0xF, frame >> 4
        codes = [struct.unpack_from("<H", data, at + 4 + 2 * index)[0] for index in range(count)]
        index = 0
        while index < count:
            code = codes[index]
            code_offset, operation, info = code & 0xFF, (code >> 8) & 0xF, code >> 12
            applies = depth > 0 or inside >= code_offset
            slots = 1
            if operation == 0:  # PUSH_NONVOL
                if applies:
                    registers[REGISTERS[info]] = dump.pointer(registers["rsp"])
                    registers["rsp"] += 8
            elif operation == 1:  # ALLOC_LARGE
                if info == 0:
                    size, slots = codes[index + 1] * 8, 2
                else:
                    size, slots = codes[index + 1] | (codes[index + 2] << 16), 3
                if applies:
                    registers["rsp"] += size
            elif operation == 2:  # ALLOC_SMALL
                if applies:
                    registers["rsp"] += info * 8 + 8
            elif operation == 3:  # SET_FPREG
                if applies:
                    if registers[REGISTERS[frame_register]] is None:
                        return "a saved frame pointer is not in the dump"
                    registers["rsp"] = registers[REGISTERS[frame_register]] - frame_offset * 16
            elif operation == 4:  # SAVE_NONVOL
                slots = 2
                if applies:
                    registers[REGISTERS[info]] = dump.pointer(registers["rsp"] + codes[index + 1] * 8)
            elif operation == 5:  # SAVE_NONVOL_FAR
                slots = 3
                if applies:
                    registers[REGISTERS[info]] = dump.pointer(registers["rsp"] + (codes[index + 1] | (codes[index + 2] << 16)))
            elif operation == 8:  # SAVE_XMM128
                slots = 2
            elif operation == 9:  # SAVE_XMM128_FAR
                slots = 3
            elif operation == 10:  # PUSH_MACHFRAME
                if applies:
                    top = registers["rsp"] + (8 if info else 0)
                    registers["rip"], registers["rsp"] = dump.pointer(top), dump.pointer(top + 24)
                    machine_frame = True
            index += slots
            if registers["rsp"] is None:
                return "the stack memory ends here"
        if flags & 4:  # CHAININFO: go on with the function this chunk belongs to
            _, _, unwind = struct.unpack_from("<III", data, at + 4 + 2 * ((count + 1) & ~1))
            continue
        break
    if not machine_frame:
        caller = dump.pointer(registers["rsp"])
        if caller is None:
            return "the stack memory ends here"
        registers["rip"], registers["rsp"] = caller, registers["rsp"] + 8
    return registers


def walk(dump, thread, depth):
    """The frames of a thread, innermost first, and why the walk ended (None: the stack's own end)."""
    registers = dict(dump.threads[thread]["registers"])
    frames = []
    for _ in range(depth):
        frames.append(registers["rip"])
        caller = step(dump, registers)
        if isinstance(caller, str):
            return frames, caller
        if not caller["rip"]:
            return frames, None  # the thread's first frame returns to nowhere
        if caller["rsp"] <= registers["rsp"]:
            return frames, "the stack pointer did not move towards the stack's base"
        registers = caller
    return frames, f"--depth {depth} reached, the stack goes on"


def read_address_library(path):
    """{offset: id} from an Address Library file (format 1 or 2)."""
    with open(path, "rb") as file:
        data = file.read()
    position = 0

    def take(layout):
        nonlocal position
        value = struct.unpack_from(layout, data, position)[0]
        position += struct.calcsize(layout)
        return value

    if take("<i") not in (1, 2):
        raise ValueError("unsupported Address Library format")
    position += 16  # game version
    name_length = take("<i")
    position += name_length  # module name
    pointer_size = take("<i")
    offsets = {}
    last_id = last_offset = 0
    for _ in range(take("<i")):
        kind = take("<B")
        low, high = kind & 0xF, kind >> 4
        if low == 0: identifier = take("<Q")
        elif low == 1: identifier = last_id + 1
        elif low == 2: identifier = last_id + take("<B")
        elif low == 3: identifier = last_id - take("<B")
        elif low == 4: identifier = last_id + take("<H")
        elif low == 5: identifier = last_id - take("<H")
        elif low == 6: identifier = take("<H")
        elif low == 7: identifier = take("<I")
        else: raise ValueError("bad id encoding")
        previous = last_offset // pointer_size if high & 8 else last_offset
        selector = high & 7
        if selector == 0: offset = take("<Q")
        elif selector == 1: offset = previous + 1
        elif selector == 2: offset = previous + take("<B")
        elif selector == 3: offset = previous - take("<B")
        elif selector == 4: offset = previous + take("<H")
        elif selector == 5: offset = previous - take("<H")
        elif selector == 6: offset = take("<H")
        else: offset = take("<I")
        if high & 8:
            offset *= pointer_size
        offsets.setdefault(offset, identifier)
        last_id, last_offset = identifier, offset
    return offsets


def find_address_library(dump, game):
    """The Address Library file of the game version in the dump, looked for where SKSE plugins of
    the dump were loaded from: the same folder and, for a mod manager's layout, its sibling mods."""
    name = "versionlib-{}-{}-{}-{}.bin".format(*game.version)
    seen = set()
    for module in dump.modules:
        folder = os.path.dirname(module.path)
        if os.path.normcase(folder).endswith(os.path.normcase(os.path.join("SKSE", "Plugins"))) and folder not in seen:
            seen.add(folder)
            candidates = [os.path.join(folder, name)]
            mods = os.path.dirname(os.path.dirname(os.path.dirname(folder)))
            candidates += glob.glob(os.path.join(glob.escape(mods), "*", "SKSE", "Plugins", name))
            for candidate in candidates:
                if os.path.isfile(candidate):
                    return candidate
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dump")
    parser.add_argument("--first", type=int, nargs="*", default=[], help="thread ids to print first")
    parser.add_argument("--depth", type=int, default=48)
    parser.add_argument("--addrlib", help="Address Library file of the game version in the dump")
    parser.add_argument("--names", help="folder with skydb.py, for function names of SkyrimSE.exe frames")
    args = parser.parse_args()

    dump = Dump(args.dump)
    game = next((module for module in dump.modules if module.name.lower() == "skyrimse.exe"), None)
    notes = []
    identifiers = None
    resolver = None
    if game:
        library = args.addrlib or find_address_library(dump, game)
        if not library:
            notes.append("no Address Library file found for game version {}.{}.{}.{}: SkyrimSE.exe frames carry no IDs (--addrlib names one)"
                         .format(*game.version))
        elif args.names:
            sys.path.insert(0, args.names)
            from skydb import SkyDB  # noqa: E402
            resolver = SkyDB(library, game.path)
        else:
            offsets = read_address_library(library)
            identifiers = (sorted(offsets), offsets)

    def label(address):
        module = dump.module_of(address)
        if not module:
            return f"0x{address:X}"
        rva = address - module.base
        if module is game and resolver:
            return "SkyrimSE  " + resolver.resolve(rva)[3]
        if module is game and identifiers:
            index = bisect.bisect_right(identifiers[0], rva) - 1
            if index >= 0:
                start = identifiers[0][index]
                return f"SkyrimSE+{rva:X}  (ID {identifiers[1][start]}+0x{rva - start:X})"
        return f"{module.name}+{rva:X}"

    stacks = defaultdict(list)
    for thread in dump.threads:
        frames, ended = walk(dump, thread, args.depth)
        stacks[(tuple(label(address) for address in frames), ended)].append(thread)

    def own(frames):
        return any(frame.lower().startswith(OWN_MODULES) for frame in frames)

    def order(item):
        (frames, _), threads = item
        named = [args.first.index(thread) for thread in threads if thread in args.first]
        return (min(named) if named else len(args.first), not own(frames), -len(threads))

    unusable = sorted({f"{module.name}: {module.problem()}" for module in dump.modules if module.checked and module.problem()})
    print(f"{os.path.basename(args.dump)}: {len(dump.threads)} threads, {len(stacks)} different stacks, {len(dump.modules)} modules")
    for note in notes:
        print(f"note: {note}")
    if unusable:
        print("modules on a stack whose file gave no unwind data (a walk stops at its first frame in one of them,")
        print("because the frames beyond it would be guesses):")
        for line in unusable:
            print(f"  {line}")
    for (frames, ended), threads in sorted(stacks.items(), key=order):
        mark = "  <-- frames of this project's DLLs" if own(frames) else ""
        print(f"\n=== {len(threads)} thread(s): {' '.join(map(str, sorted(threads)))}{mark}")
        for index, frame in enumerate(frames):
            print(f"  [{index:2}] {frame}")
        if ended:
            print(f"       walk stopped: {ended}")


if __name__ == "__main__":
    sys.exit(main())
