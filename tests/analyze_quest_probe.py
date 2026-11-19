"""Summarize quest probe runs (APBQuestProbe, segment A) and compare them across conditions.

Usage: python tests/analyze_quest_probe.py <run folder>...

Each run folder holds probe.log and probe.tsv from build/catchup/run-quest-probe.ps1. One line is
printed per trial, then the values that must agree between the runs: the order of MQ203 stages, the
path by which stage 48 was reached, and the end state. Differences are listed; nothing is judged.
"""

import argparse
import csv
import hashlib
import re
import sys
from collections import defaultdict
from pathlib import Path

MQ203 = "00036192"
SCENE_ACTOR_KINDS = ("topic", "equip")
MAIN_SPEAKERS = ("00019DFD", "00013485")  # Esbern, Delphine
# Menus this mod list opens on its own around a load; anything else during a trial points at input.
BACKGROUND_MENUS = {"AnimationInjectorLoading", "BTPS Menu", "BTPS Ovelay Menu", "BestiaryWidget", "CastingBar", "Cursor Menu",
                    "Fader Menu", "HUD Menu", "LoadWaitSpinner", "Loading Menu", "Mist Menu", "TrueHUD", "oxygenMeter2"}


def read_rows(path):
    trials = defaultdict(list)
    with path.open(encoding="utf-8", newline="") as handle:
        for row in csv.DictReader(handle, delimiter="\t", quoting=csv.QUOTE_NONE):
            row["ms"] = float(row["ms"])
            trials[int(row["trial"])].append(row)
    return trials


def read_log(path):
    text = path.read_text(encoding="utf-8-sig")
    header = re.search(r"quest probe segment=\S+ .*", text)
    results = {}
    for line in text.splitlines():
        if "quest probe trial=" in line:
            fields = dict(re.findall(r'(\w+)=("[^"]*"|\S+)', line))
            results[int(fields["trial"])] = fields
    condition = "off"
    if header and "catch_up=true" in header.group(0):
        # Probes before catch_up_mode was logged were only ever run in accelerate.
        mode = re.search(r"catch_up_mode=(\w+)", header.group(0))
        condition = mode.group(1) if mode else "accelerate"
        # skip_blocked: the test rule that lets extra cycles pass calls that must wait.
        if "skip_blocked=true" in header.group(0):
            condition += "+skip"
    return condition, (header.group(0) if header else "no header"), results


def snapshot(rows, label):
    """Lines of one snapshot, without the label, in a stable order."""
    prefix = label + ";"
    return sorted(row["text"][len(prefix):] for row in rows if row["kind"] == "snap" and row["text"].startswith(prefix))


def digest(lines):
    return hashlib.sha1("\n".join(lines).encode("utf-8")).hexdigest()[:10] if lines else "-"


def window(rows, center_ms, half_ms=2000.0):
    """Extra cycles, extra calls and held frames in the catch-up rows around a time."""
    cycles = calls = held = frames = 0
    for row in rows:
        if row["kind"] == "cu" and abs(row["ms"] - center_ms) <= half_ms:
            fields = dict(re.findall(r"(\w+)=(\d+)", row["text"]))
            cycles += int(fields.get("cycles", 0))
            calls += int(fields.get("extra_calls", 0))
            held += int(fields.get("held", 0))
            frames += int(fields.get("frames", 0))
    return f"cycles={cycles} calls={calls} held={held}/{frames}"


def describe(rows):
    stages = [(row["ms"], int(row["b"], 16)) for row in rows if row["kind"] == "stage" and row["a"] == MQ203]
    order = [stage for _, stage in stages]
    when = {stage: ms for ms, stage in reversed(stages)}  # first time each stage was set
    anim = [row["ms"] for row in rows if row["kind"] == "anim" and row["text"] == "AnimObjectUnequip"]
    path = "none"
    if 48 in when:
        if 60 in when and when[48] >= when[60]:
            path = "failsafe (stage 60)"
        elif any(ms <= when[48] for ms in anim):
            # The probe sees the animation graph event and the stage notification, not the Papyrus
            # handler in between; this says the graph event came first, not which handler ran.
            path = "after the graph event"
        else:
            path = "before stage 60, no graph event seen"
    phases = []
    for row in rows:
        if row["kind"] == "scene":
            phase = int(row["b"], 16)
            phase = phase - (1 << 32) if phase >= 1 << 31 else phase
            phases.append(f"{phase if row['text'] == 'playing' else 'x'}")
    actor_events = [f"{row['kind']}:{row['a']}:{row['b']}:{row['text']}" for row in rows if row["kind"] in SCENE_ACTOR_KINDS]
    # The lines Esbern and Delphine start once the scene runs (from stage 30), in order. Orgnar is left
    # out: he also greets the player and chats on his own, which differs from run to run without the
    # plugin, and so do the follower remarks Esbern makes on the way.
    dialogue = []
    scene_start = when.get(30, float("inf"))
    for row in rows:
        if row["kind"] == "topic" and row["text"] == "begin" and row["a"] in MAIN_SPEAKERS and row["ms"] >= scene_start:
            line = f"{row['a']}:{row['b']}"
            # The game sometimes reports the start of the same line twice in a row.
            if not dialogue or dialogue[-1] != line:
                dialogue.append(line)
    player_packages = sorted({row["text"] for row in rows if row["kind"] == "pkg" and row["text"] != "none"})
    forced = [row["text"] for row in rows if row["kind"] == "act"]
    end = snapshot(rows, "end")
    # Stray input: how far the player moved after being placed in the inn, and which menus opened.
    placed = next((row["ms"] for row in rows if row["kind"] == "act" and row["text"] == "Esbern to player (inn)"), None)
    drift = None
    menus = []
    if placed is not None:
        points = []
        for row in rows:
            if row["kind"] == "pos" and row["a"] == "00000014" and row["ms"] >= placed:
                values = dict(re.findall(r"(\w)=(-?\d+)", row["text"]))
                points.append((float(values["x"]), float(values["y"]), float(values["z"])))
        if points:
            drift = max(sum((a - b) ** 2 for a, b in zip(point, points[0])) ** 0.5 for point in points)
        menus = sorted({row["text"].rsplit(" ", 1)[0] for row in rows
                        if row["kind"] == "menu" and row["ms"] >= placed and row["text"].endswith(" open")})
    return {
        "order": order,
        "times": " ".join(f"{stage}@{ms / 1000:.1f}s" for ms, stage in stages),
        "path": path,
        "anim_to_48_ms": (when[48] - max(ms for ms in anim if ms <= when[48])) if path == "after the graph event" else None,
        "phases": ">".join(phases),
        "actor_events": digest(actor_events),
        "actor_event_count": len(actor_events),
        "dialogue": digest(dialogue),
        "dialogue_count": len(dialogue),
        "player_packages": player_packages,
        "forced": forced,
        "player_drift": drift,
        "menus": menus,
        "end": end,
        "end_digest": digest(end),
        "reload_same": snapshot(rows, "end_before_save") == snapshot(rows, "end_after_reload") if snapshot(rows, "end_after_reload") else None,
        "mid_same": snapshot(rows, "mid_before_save") == snapshot(rows, "mid_after_reload") if snapshot(rows, "mid_after_reload") else None,
        "cu_at_40": window(rows, when[40]) if 40 in when else "-",
        # Whether extra cycles ran around the animation event is asked of the event itself, so a trial
        # that never reached stage 48 still shows whether catch-up was active there.
        "cu_at_anim": window(rows, anim[0]) if anim else "-",
        "cu_at_48": window(rows, when[48]) if 48 in when else "-",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", nargs="+", type=Path)
    args = parser.parse_args()

    by_condition = defaultdict(list)
    for run in args.runs:
        condition, header, results = read_log(run / "probe.log")
        trials = read_rows(run / "probe.tsv") if (run / "probe.tsv").exists() else {}
        print(f"== {run.name} [{condition}] {header}")
        for trial in sorted(set(results) | set(trials)):
            result = results.get(trial, {})
            info = describe(trials.get(trial, []))
            by_condition[condition].append((run.name, trial, result, info))
            print(f"  trial {trial}: {result.get('result', 'no result line')} {result.get('reason', '')} "
                  f"scene_s={float(result.get('scene_ms', 0)) / 1000:.1f} foreground={result.get('foreground_pct', '-')}%")
            print(f"    stages: {info['times']}")
            print(f"    stage 48 path: {info['path']}"
                  + (f" ({info['anim_to_48_ms']:.0f} ms from graph event to stage notification)" if info["anim_to_48_ms"] is not None else ""))
            print(f"    scene phases: {info['phases']}")
            print(f"    Esbern and Delphine lines: {info['dialogue_count']} (order digest {info['dialogue']}); "
                  f"all scene actor events incl. Orgnar and equipment: {info['actor_event_count']}")
            suspect = [menu for menu in info["menus"] if menu not in BACKGROUND_MENUS]
            print("    player moved after being placed in the inn: "
                  + (f"{info['player_drift']:.0f} units" if info["player_drift"] is not None else "no position rows")
                  + f"; player packages: {', '.join(info['player_packages']) or 'none'}"
                  + f"; menus other than HUD and loading: {', '.join(suspect) or 'none'}")
            print(f"    end state digest: {info['end_digest']}  reload equal: {info['reload_same']}  mid reload equal: {info['mid_same']}")
            print(f"    catch-up: frames={result.get('cu_frames', '-')} engaged={result.get('cu_engaged', '-')} held={result.get('cu_held', '-')} "
                  f"cycles={result.get('cu_cycles', '-')} extra_calls={result.get('cu_extra_calls', '-')} "
                  f"front_skips={result.get('cu_front_skips', '-')} order_rebuilds={result.get('cu_order_rebuilds', '-')} "
                  f"gate_closes={result.get('cu_gate_closes', '-')} "
                  f"violations={result.get('cu_guard_violations', '-')}; around stage 40: {info['cu_at_40']}; "
                  f"around the animation event: {info['cu_at_anim']}; around stage 48: {info['cu_at_48']}")

    print("\n== values per condition (trials)")
    for title, key in (("MQ203 stage order", lambda i: " ".join(map(str, i["order"]))), ("stage 48 path", lambda i: i["path"]),
                       ("end state digest", lambda i: i["end_digest"]), ("Esbern and Delphine line order", lambda i: i["dialogue"])):
        print(f"  {title}:")
        for condition, entries in sorted(by_condition.items()):
            counts = defaultdict(int)
            for _, _, _, info in entries:
                counts[key(info)] += 1
            for value, count in sorted(counts.items(), key=lambda item: -item[1]):
                print(f"    {condition:15} x{count}  {value}")

    # The reference is the first off trial that reached the end snapshot. A trial that did not get
    # there has no end state to compare: it is named, not listed as a difference in every line.
    reference = next(((run, trial, info) for run, trial, _, info in by_condition.get("off", []) if info["end"]), None)
    if len(by_condition) > 1:
        print("\n== end state lines that differ from the first off trial with an end state"
              + (f" ({reference[0]} trial {reference[1]})" if reference else ""))
        if not reference:
            print("  no off trial reached the end snapshot, so there is nothing to compare with")
        clean = bool(reference)
        for condition, entries in sorted(by_condition.items()):
            for run, trial, _, info in entries:
                if not info["end"]:
                    clean = False
                    print(f"  {condition} {run} trial {trial}: no end state (the trial did not reach the end snapshot)")
                    continue
                if not reference:
                    continue
                base = reference[2]["end"]
                extra = [line for line in info["end"] if line not in base]
                missing = [line for line in base if line not in info["end"]]
                if extra or missing:
                    clean = False
                    print(f"  {condition} {run} trial {trial}:")
                    for line in missing:
                        print(f"    - {line}")
                    for line in extra:
                        print(f"    + {line}")
        if clean:
            print("  none")


if __name__ == "__main__":
    sys.exit(main())
