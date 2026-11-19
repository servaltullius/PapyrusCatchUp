"""Catalog Skyrim SE/AE saves and check whether each can load in an MO2 profile without the
'content no longer present' prompt (which would stall an unattended survey run).

Usage:
    python tests/save_catalog.py <profile dir> <save .ess or dir> [...] [--tsv out.tsv]

For every save prints character, level, location, in-game date, file time and plugin counts, then
'ok' when every plugin the save lists is active in the profile, or the missing plugins.
Reads the save header and the first part of its LZ4 body only; never writes the save.
"""
import argparse
import datetime
import struct
import sys
from pathlib import Path

import lz4.block

BASE_MASTERS = ["Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"]


class Reader:
    def __init__(self, data: bytes, pos: int = 0):
        self.data = data
        self.pos = pos

    def take(self, fmt: str):
        size = struct.calcsize(fmt)
        if self.pos + size > len(self.data):
            raise ValueError("truncated save")
        values = struct.unpack_from(fmt, self.data, self.pos)
        self.pos += size
        return values[0] if len(values) == 1 else values

    def wstring(self) -> str:
        length = self.take("<H")
        if self.pos + length > len(self.data):
            raise ValueError("truncated string")
        raw = self.data[self.pos:self.pos + length]
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            text = raw.decode("cp1252", errors="replace")
        self.pos += length
        return text


def read_save(path: Path) -> dict:
    data = path.read_bytes()
    if not data.startswith(b"TESV_SAVEGAME"):
        raise ValueError("not a Skyrim save")
    r = Reader(data, 13)
    header_size = r.take("<I")
    header_end = r.pos + header_size
    version = r.take("<I")
    if version < 12:
        raise ValueError(f"save version {version} is not Skyrim SE/AE")
    info = {"version": version, "number": r.take("<I"), "player": r.wstring(), "level": r.take("<I"),
            "location": r.wstring(), "game_date": r.wstring(), "race": r.wstring()}
    r.take("<H")          # sex
    r.take("<ff")         # current and level-up experience
    filetime = r.take("<Q")
    width, height = r.take("<II")
    compression = r.take("<H")
    info["file_time"] = (datetime.datetime(1601, 1, 1) + datetime.timedelta(microseconds=filetime // 10)).strftime("%Y-%m-%d %H:%M")
    if r.pos != header_end:
        raise ValueError(f"unexpected header size {header_size}")
    body_start = header_end + width * height * 4
    if compression == 0:
        body = data[body_start:]
    else:
        r = Reader(data, body_start)
        uncompressed, compressed = r.take("<II")
        if compression != 2:
            raise ValueError(f"compression type {compression} is not LZ4")
        body = lz4.block.decompress(data[r.pos:r.pos + compressed], uncompressed_size=uncompressed)
    b = Reader(body)
    form_version = b.take("<B")
    b.take("<I")          # plugin info size
    plugins = [b.wstring() for _ in range(b.take("<B"))]
    light = [b.wstring() for _ in range(b.take("<H"))] if form_version >= 78 else []
    info.update(form_version=form_version, plugins=plugins, light=light)
    return info


def active_plugins(profile: Path) -> set[str]:
    names = {name.lower() for name in BASE_MASTERS}
    game = None
    ini = profile.parent.parent / "ModOrganizer.ini"
    if ini.exists():
        for line in ini.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("gamePath="):
                game = Path(line.split("(", 1)[-1].rstrip(")").replace("\\\\", "\\"))
    if game and (game / "Skyrim.ccc").exists():
        names |= {line.strip().lower() for line in (game / "Skyrim.ccc").read_text(errors="replace").splitlines() if line.strip()}
    for line in (profile / "plugins.txt").read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("*"):
            names.add(line[1:].strip().lower())
    return names


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("profile", type=Path)
    parser.add_argument("saves", nargs="+", type=Path)
    parser.add_argument("--tsv", type=Path)
    args = parser.parse_args()
    active = active_plugins(args.profile)
    files = []
    for item in args.saves:
        files += sorted(item.glob("*.ess")) if item.is_dir() else [item]
    rows = []
    for path in files:
        try:
            save = read_save(path)
        except (OSError, ValueError, lz4.block.LZ4BlockError) as error:
            rows.append([str(path), "error", str(error)])
            print(f"{path}\terror\t{error}")
            continue
        listed = save["plugins"] + save["light"]
        missing = [name for name in listed if name.lower() not in active]
        status = "ok" if not missing else "missing:" + ";".join(missing)
        row = [str(path), save["player"], str(save["level"]), save["location"], save["game_date"], save["file_time"],
               f"plugins={len(listed)}", f"active={len(active)}", status]
        rows.append(row)
        print("\t".join(row))
    if args.tsv:
        args.tsv.write_text("\n".join("\t".join(row) for row in rows) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
