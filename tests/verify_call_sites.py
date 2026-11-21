"""Offline check of the VM catch-up hook sites and Address Library IDs for the runtimes the plugin accepts.

1. Every Address Library ID the plugin resolves must exist in the Address Library file of each accepted
   runtime: SE IDs in version-1-5-97-0.bin, AE IDs in the versionlib files of AE 1.6.640, 1.6.659,
   1.6.1130, 1.6.1170 and 1.6.1179. CommonLibSSE-NG ends the game when an ID is missing, so a gap here
   is a crash at startup. Covered: the RelocationID constants and accessor vtables of
   src/VMCatchUp.cpp, the game reads of src/NativeClassification.h, and the CommonLibSSE-NG singletons
   the plugin reaches (UI, SkyrimVM). Counts are checked so that a change of declaration style cannot
   shrink the set unnoticed.
2. The call-site rules of VMCatchUp.cpp (Chain::Verify, ResolveJob, src/CallSiteFinder.h) are replayed
   on the stock SE 1.5.97 and AE 1.6.1170 executables (decrypted): functions must start a .pdata
   entry, the job tail must match its 74-byte pattern with the scope calls on the stock empty
   functions, and every other site must be the only call to its target carrying its prefix (the
   per-runtime prefix for the two retry sites). On 1.6.1170 every site must be the one the measured
   0.3.0 builds patched at a fixed offset. Only stock code is replayed: the paths for calls that other
   plugins redirected are covered by the C++ unit tests.

The other AE runtimes have no executable here; in the game their sites are only accepted when they
carry the same patterns, which this script cannot check.

Usage: python tests/verify_call_sites.py <Address Library plugin folder> <1.5.97 SkyrimSE.exe> <1.6.1170 SkyrimSE.exe>
"""

import bisect
import re
import struct
import sys
from pathlib import Path

import pefile

ROOT = Path(__file__).resolve().parent.parent
AE_RUNTIMES = ("1-6-640-0", "1-6-659-0", "1-6-1130-0", "1-6-1170-0", "1-6-1179-0")
# CommonLibSSE-NG singletons the plugin uses (UI::GetSingleton, SkyrimVM::GetSingleton): (SE, AE).
LIBRARY_IDS = {"UI": (514178, 400327), "SkyrimVM": (514315, 400475)}
EXPECTED = {"functions": 18, "vtables": 3, "reads": 30}
# Offsets of the E8 calls the 1.6.1170 builds measured until 0.3.0 patched, in their functions.
AE1170_SITES = {
    "updateGame": 0x1E, "reset": 0x94, "prepare": 0x650, "taskletCall": 0x82F, "queueCall": 0x633,
    "resumeCall": 0x56, "firstPop": 0xFA, "nextPop": 0x1BF, "drainDispatch": 0x18E, "drainReturn": 0x177,
    "retryDispatch": 0x44F, "retryPass": 0x10E,
}
TAIL_CALLS = {"scopeCtor": 0x0B, "scopeDtor": 0x27, "submit": 0x34, "wait": 0x3C, "reset": 0x44}


class CheckFailed(Exception):
    pass


def require(condition, message):
    if not condition:
        raise CheckFailed(message)


def read_address_library(path):
    """Address Library v1/v2 files: id -> offset."""
    ids = {}
    with open(path, "rb") as f:
        fmt = struct.unpack("<i", f.read(4))[0]
        require(fmt in (1, 2), f"{path}: unsupported format {fmt}")
        f.read(16)  # version
        f.read(struct.unpack("<i", f.read(4))[0])  # module name
        ptr_size, count = struct.unpack("<ii", f.read(8))
        pid = poff = 0
        for _ in range(count):
            t = f.read(1)[0]
            low, high = t & 0xF, t >> 4
            if low == 0: pid = struct.unpack("<Q", f.read(8))[0]
            elif low == 1: pid += 1
            elif low == 2: pid += f.read(1)[0]
            elif low == 3: pid -= f.read(1)[0]
            elif low == 4: pid += struct.unpack("<H", f.read(2))[0]
            elif low == 5: pid -= struct.unpack("<H", f.read(2))[0]
            elif low == 6: pid = struct.unpack("<H", f.read(2))[0]
            elif low == 7: pid = struct.unpack("<I", f.read(4))[0]
            base = poff // ptr_size if high & 8 else poff
            h = high & 7
            if h == 0: off = struct.unpack("<Q", f.read(8))[0]
            elif h == 1: off = base + 1
            elif h == 2: off = base + f.read(1)[0]
            elif h == 3: off = base - f.read(1)[0]
            elif h == 4: off = base + struct.unpack("<H", f.read(2))[0]
            elif h == 5: off = base - struct.unpack("<H", f.read(2))[0]
            elif h == 6: off = struct.unpack("<H", f.read(2))[0]
            else: off = struct.unpack("<I", f.read(4))[0]
            if high & 8:
                off *= ptr_size
            poff = off
            ids[pid] = off
    return ids


def source():
    vm = (ROOT / "src" / "VMCatchUp.cpp").read_text(encoding="utf-8")
    natives = (ROOT / "src" / "NativeClassification.h").read_text(encoding="utf-8")
    named = {name: (int(se), int(ae)) for name, se, ae in
             re.findall(r"constexpr auto (\w+) = REL::RelocationID\((\d+), (\d+)\);", vm)}
    block = vm[vm.index("accessorVtableIDs{"):]
    block = block[:block.index("} };")]
    vtables = [(int(se), int(ae)) for se, ae in re.findall(r"\{ (\d+), (\d+) \}", block)]
    reads = [(int(se), int(ae)) for se, ae in
             re.findall(r'\{ "\w+", "\w+", NativeOrigin::kGame, (\d+), (\d+) \}', natives)]
    found = {"functions": len(named), "vtables": len(vtables), "reads": len(reads)}
    require(found == EXPECTED, f"declarations found {found}, expected {EXPECTED}: update the parser or EXPECTED")
    patterns = {}
    for name, body in re.findall(r"constexpr std::array<PatternByte, (?:\d+)> (\w+)\{([^}]*)\};", vm):
        patterns[name] = [None if b.strip() == "W" else int(b, 16) for b in body.split(",") if b.strip()]
    for name, value in re.findall(r"constexpr std::size_t (tail\w+) = (0x[0-9A-F]+);", vm):
        require(TAIL_CALLS[name[4].lower() + name[5:]] == int(value, 16), f"{name} differs from TAIL_CALLS")
    return named, vtables, reads, patterns


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(str(path), fast_load=True)
        self.pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]])
        self.functions = sorted((e.struct.BeginAddress, e.struct.EndAddress) for e in self.pe.DIRECTORY_ENTRY_EXCEPTION)
        self.size = self.pe.OPTIONAL_HEADER.SizeOfImage

    def read(self, rva, n):
        return self.pe.get_data(rva, n)

    def function_code(self, rva, limit):
        """FunctionCode() of VMCatchUp.cpp: empty unless rva starts a .pdata entry."""
        i = bisect.bisect_right(self.functions, (rva, 1 << 62)) - 1
        if i < 0 or self.functions[i][0] != rva:
            return b""
        return self.read(rva, min(self.functions[i][1] - rva, limit))


def calls_to(base, code, target):
    return [i for i in range(len(code) - 4)
            if code[i] == 0xE8 and base + i + 5 + struct.unpack_from("<i", code, i + 1)[0] == target]


def call_target(base, code, at):
    return base + at + 5 + struct.unpack_from("<i", code, at + 1)[0]


def matches(code, start, pattern):
    return start >= 0 and start + len(pattern) <= len(code) and all(
        p is None or code[start + j] == p for j, p in enumerate(pattern))


def unique_call(base, code, target, prefix):
    hits = [h for h in calls_to(base, code, target) if matches(code, h + 1 - len(prefix), prefix)]
    return hits[0] if len(hits) == 1 else None


def replay(image, ids, p, se):
    """Chain::Verify / ResolveJob on stock code. Returns {site: offset or None}."""
    out = {}
    job = ids["vmUpdateJobID"]
    code = image.function_code(job, 0x180)
    require(code, "job does not start a .pdata entry")
    ctor, dtor = image.read(ids["jobScopeCtorID"], 4), image.read(ids["jobScopeDtorID"], 3)
    require(ctor == bytes([0x48, 0x8B, 0xC1, 0xC3]) and (dtor[0] == 0xC3 or dtor == bytes([0xC2, 0, 0])), "job scope is not empty")
    tail_pattern = p["jobTailPattern"]
    tails = [i for i in range(len(code)) if matches(code, i, tail_pattern)]
    require(len(tails) == 1, f"job tail pattern matched {len(tails)} times")
    tail = tails[0]
    for name, target in (("scopeCtor", "jobScopeCtorID"), ("scopeDtor", "jobScopeDtorID"), ("submit", "jobsSubmitID"),
                         ("wait", "jobsWaitID"), ("reset", "jobsResetID")):
        at = tail + TAIL_CALLS[name]
        require(call_target(job, code, at) == ids[target], f"job tail {name} does not call its stock target")
        out[name] = at
    out["updateGame"] = unique_call(job, code, ids["skyrimVMUpdateGameID"], p["updateGamePrefix"])
    require(out["updateGame"] is not None and out["updateGame"] < tail, "updateGame (carried)")

    drain, update, attempt = ids["drainFuncQueueID"], ids["vmUpdateID"], ids["attemptFunctionCallID"]
    retry, resume, tasklet = ids["retrySuspendedID"], ids["codeTaskletResumeID"], ids["codeTaskletRunID"]
    drain_code = image.function_code(drain, 0x400)
    pops = calls_to(drain, drain_code, ids["popFuncMessageID"])
    require(len(pops) == 2 and all(matches(drain_code, h + 1 - len(p["popPrefix"]), p["popPrefix"]) for h in pops), "queue pops")
    out["firstPop"], out["nextPop"] = pops
    suffix = "SE" if se else "AE"
    checks = (
        ("drainDispatch", drain, drain_code, attempt, "drainDispatchPrefix"),
        ("drainReturn", drain, drain_code, ids["returnFromLatentID"], "drainReturnPrefix"),
        ("retryPass", update, image.function_code(update, 0x1000), retry, "retryPassPrefix" + suffix),
        ("queueCall", attempt, image.function_code(attempt, 0x1000), ids["queueFuncCallID"], "queueCallPrefix"),
        ("prepare", attempt, image.function_code(attempt, 0x1000), ids["prepareArgsID"], "preparePrefix"),
        ("retryDispatch", retry, image.function_code(retry, 0xA00), attempt, "retryDispatchPrefix" + suffix),
        ("resumeCall", resume, image.function_code(resume, 0x200), attempt, "resumeCallPrefix"),
    )
    for name, function, fcode, target, prefix in checks:
        require(fcode, f"{name}: function does not start a .pdata entry")
        out[name] = unique_call(function, fcode, target, p[prefix])
        require(out[name] is not None, f"{name}: no unique call carrying {prefix}")
    tcode = image.function_code(tasklet, 0x1400)
    require(tcode, "CodeTasklet::Run does not start a .pdata entry")
    if se:
        require(len(calls_to(tasklet, tcode, resume)) == 1 and not calls_to(tasklet, tcode, attempt),
                "SE CodeTasklet::Run does not delegate through Resume alone")
        out["taskletCall"] = None
    else:
        out["taskletCall"] = unique_call(tasklet, tcode, attempt, p["taskletCallPrefix"])
        require(out["taskletCall"] is not None, "taskletCall")
    return out


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    library, se_exe, ae_exe = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    failed = False
    try:
        named, vtables, reads, patterns = source()
    except CheckFailed as error:
        print(f"source: {error}")
        sys.exit(1)
    pairs = list(named.values()) + vtables + reads + list(LIBRARY_IDS.values())
    se_all, ae_all = {se for se, _ in pairs}, {ae for _, ae in pairs}
    print(f"IDs: {len(named)} constants, {len(vtables)} vtables, {len(reads)} game reads, {len(LIBRARY_IDS)} library singletons")
    tables = {"1-5-97-0": read_address_library(library / "version-1-5-97-0.bin")}
    for runtime in AE_RUNTIMES:
        tables[runtime] = read_address_library(library / f"versionlib-{runtime}.bin")
    for runtime, table in tables.items():
        wanted = se_all if runtime == "1-5-97-0" else ae_all
        missing = sorted(wanted - table.keys())
        print(f"{runtime.replace('-', '.')}: {len(wanted)} IDs, missing {missing}")
        failed |= bool(missing)

    for label, exe, table, pick, se in (("SE 1.5.97", se_exe, tables["1-5-97-0"], 0, True),
                                         ("AE 1.6.1170", ae_exe, tables["1-6-1170-0"], 1, False)):
        image = Image(exe)
        ids = {name: table[pair[pick]] for name, pair in named.items()}
        try:
            sites = replay(image, ids, patterns, se)
        except CheckFailed as error:
            print(f"{label}: FAILED: {error}")
            failed = True
            continue
        shown = {k: (hex(v) if v is not None else "via Resume") for k, v in sites.items()}
        print(f"{label}: every site found under the C++ rules: {shown}")
        if not se:
            differ = {k: (hex(sites[k]), hex(v)) for k, v in AE1170_SITES.items() if sites[k] != v}
            print(f"{label}: sites that differ from the measured 0.3.0 build: {differ or 'none'}")
            failed |= bool(differ)
    print("FAILED" if failed else "all checks passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
