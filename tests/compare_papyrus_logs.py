"""Compare two Papyrus logs: error, warning and stack-dump lines grouped by normalized message.

Usage: python tests/compare_papyrus_logs.py <baseline Papyrus.0.log> <candidate Papyrus.0.log>

Timestamps, stack IDs, handles, form IDs and other numbers are replaced so that the same message
from different runs falls into one group. Messages found only in the candidate are listed first.
"""

import argparse
import re
from collections import Counter
from pathlib import Path

# Any bracketed prefix: the AM/PM marker follows the system locale (for example "오후").
TIMESTAMP = re.compile(r"^\[\d{2}/\d{2}/\d{4} - [^\]]*\]\s*")
NUMBERS = re.compile(r"0x[0-9A-Fa-f]+|\b[0-9A-Fa-f]{8}\b|\b\d+(?:\.\d+)?\b")
CATEGORIES = (
    ("error", re.compile(r"\berror\b", re.IGNORECASE)),
    ("warning", re.compile(r"\bwarning\b", re.IGNORECASE)),
    ("stack dump", re.compile(r"dumping stack|stack dump|suspended stack", re.IGNORECASE)),
    ("overstressed", re.compile(r"overstress", re.IGNORECASE)),
)


def read(path):
    groups = {name: Counter() for name, _ in CATEGORIES}
    lines = 0
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = TIMESTAMP.sub("", raw).strip()
        if not line:
            continue
        lines += 1
        for name, pattern in CATEGORIES:
            if pattern.search(line):
                groups[name][NUMBERS.sub("#", line)[:220]] += 1
                break
    return lines, groups


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--show", type=int, default=25, help="messages listed per section")
    args = parser.parse_args()
    base_lines, base = read(args.baseline)
    cand_lines, cand = read(args.candidate)
    print(f"lines: baseline={base_lines} candidate={cand_lines}")
    for name, _ in CATEGORIES:
        b, c = base[name], cand[name]
        print(f"{name}: baseline={sum(b.values())} ({len(b)} kinds) candidate={sum(c.values())} ({len(c)} kinds)")
    for name, _ in CATEGORIES:
        b, c = base[name], cand[name]
        only = [(msg, n) for msg, n in c.most_common() if msg not in b]
        if only:
            print(f"\n== {name}: only in candidate ({len(only)} kinds)")
            for msg, n in only[: args.show]:
                print(f"  {n:5d}  {msg}")
        gone = [(msg, n) for msg, n in b.most_common() if msg not in c]
        if gone:
            print(f"\n== {name}: only in baseline ({len(gone)} kinds)")
            for msg, n in gone[: args.show]:
                print(f"  {n:5d}  {msg}")


if __name__ == "__main__":
    main()
