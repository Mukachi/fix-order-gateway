#!/usr/bin/env python3
"""Results report: RESULTS.md and the README's figures, generated from committed JSON.

  python run.py report          write RESULTS.md and fill the README's result blocks
  python run.py report --check  regenerate in memory; exit 1 naming any difference

As lockfree-ipc's report (its note 007), and built on it: its environment
checker, conditions table and formatting are used from the copy CMake fetched
at the pinned commit (build/_deps/lockfree_ipc-src), so a run here is held to
the same rules. Only files listed in results/published.toml are read, each
validated first: environment checker, published, bench mode, clean build, its
component's block present, one commit, machine and kernel command line per
set. Figures are min-max over a set's runs. Standard library only.

Runs measured as episodes (note 009, amendment of 2026-10-22) are published
per predictor state: each case's states from every run are grouped across the
set by mispredictions per message (the benchmark's own split rule), and each
state's p50 must agree across runs within 3% (S8) unless the manifest lists the
case under `withheld`.
"""
import argparse
import importlib.util
import json
import re
import subprocess
import sys
import tomllib
from pathlib import Path

SETS = ("codec",)
BLOCK = re.compile(r"(<!-- results:(?P<name>[a-z0-9-]+) -->\n)(?P<body>.*?)(<!-- /results -->)", re.S)
MEAN = re.compile(r"\b(mean|average|avg)\b", re.I)


class ReportError(Exception):
    pass


def harness(root):
    """lockfree-ipc's run.py, from the fetched copy, as a module."""
    path = root / "build" / "_deps" / "lockfree_ipc-src" / "run.py"
    if not path.is_file():
        raise ReportError(f"{path} missing: configure the build first (cmake -S . -B build)")
    spec = importlib.util.spec_from_file_location("lfipc_run", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod, path.parent


def validate(root, lf, lf_root, name, files):
    runs = []
    for f in files:
        path = root / "results" / f
        if not path.is_file():
            raise ReportError(f"{name}: {f}: missing")
        check = subprocess.run([sys.executable, "-I", str(lf_root / "tests" / "check_environment_json.py"), str(path)],
                               capture_output=True, text=True)
        if check.returncode != 0:
            fails = [l for l in check.stdout.splitlines() if l.startswith("FAIL")]
            raise ReportError(f"{name}: {f}: environment check failed: {fails[:2]}")
        d = json.loads(path.read_text())
        e = d["environment"]
        if not (e["published"] and e["ok"] and e["bench_mode"]["ok"]):
            raise ReportError(f"{name}: {f}: not a published run (refused or outside results/)")
        if not e["build"]["clean"]:
            raise ReportError(f"{name}: {f}: built from a tree with uncommitted changes")
        if name not in d or not d.get("clock", {}).get("ok"):
            raise ReportError(f"{name}: {f}: no {name} block, or the clock refused")
        runs.append((f, d))
    if not runs:
        raise ReportError(f"{name}: no runs listed")
    for key, get in (("commit", lambda e: e["build"]["commit"]),
                     ("machine", lambda e: (e["machine"]["product"], e["cpu"]["model"], e["cpu"]["microcode"])),
                     ("kernel command line", lambda e: e["kernel"]["cmdline"])):
        if len({get(d["environment"]) for _, d in runs}) > 1:
            raise ReportError(f"{name}: runs mix more than one {key}")
    return runs


def load_sets(root, lf, lf_root):
    manifest = root / "results" / "published.toml"
    if not manifest.is_file():
        raise ReportError("results/published.toml missing")
    m = {k: v for k, v in tomllib.loads(manifest.read_text()).items() if k != "notes"}
    unknown = set(m) - set(SETS)
    if unknown:
        raise ReportError(f"published.toml: unknown sets {sorted(unknown)}")
    sets = {n: (m[n].get("note", ""), validate(root, lf, lf_root, n, m[n]["runs"])) for n in SETS if n in m}
    withheld = {n: set(m[n].get("withheld", [])) for n in SETS if n in m}
    return sets, withheld


def by_state(entries, what):
    """Group (run, state) entries across a set's runs into predictor states, by
    mispredictions per message, with the benchmark's split rule (at least 1.5x
    and 0.5 apart). Returns [(name, entries)], fewest mispredictions first."""
    key = lambda e: e[1]["mispredictions_per_message"]
    entries = sorted(entries, key=key)
    groups = [[entries[0]]]
    for prev, cur in zip(entries, entries[1:]):
        if key(cur) - key(prev) >= 0.5 and key(cur) >= 1.5 * key(prev):
            groups.append([])
        groups[-1].append(cur)
    if len(groups) > 2:
        raise ReportError(f"{what}: {len(groups)} predictor states across the set's runs")
    for g in groups:
        if len({r for r, _ in g}) < len(g):
            raise ReportError(f"{what}: one run has two states that the set groups as one")
    names = ["one"] if len(groups) == 1 else ["fast", "slow"]
    return list(zip(names, groups))


def agree(what, name, p50s, withheld):
    if len(p50s) > 1 and max(p50s) > 1.03 * min(p50s) and not withheld:
        raise ReportError(f"{what}: {name} state p50 {min(p50s):.1f}-{max(p50s):.1f} ns across runs, more than 3% "
                          f"apart (S8): withhold the case in published.toml and record why in note 009")


PATHS = ("a", "scalar", "simd", "simd_padded", "index")
CASE_ORDER = (("none",)
              + tuple(f"{op}_{m}" for m in ("nos", "er", "hb")
                      for op in ("frame_scalar", "frame_simd") + tuple(f"decode_{p}" for p in PATHS))
              + ("encode_a_nos", "encode_b_nos", "encode_c_nos")
              # Note 002's runs, before the paths were split (B is the scalar path).
              + ("decode_b_nos", "decode_b_er", "decode_b_hb"))


def is_episodes(runs):
    return "states" in runs[0][1]["codec"]["cases"]["none"]


def state_rows(lf, runs, withheld):
    rows = []
    n = len(runs)
    for k in CASE_ORDER:
        present = [(i, d["codec"]["cases"][k]) for i, (_, d) in enumerate(runs) if k in d["codec"]["cases"]]
        if not present:
            continue
        c = present[0][1]
        held = k in withheld
        for name, g in by_state([(i, st) for i, cs in present for st in cs["states"]], k):
            sts = [st for _, st in g]
            agree(k, name, [st["p50_ns"] for st in sts], held)
            shares = [next((st["share"] for r, st in g if r == i), 0.0) for i in range(n)]
            figures = (["withheld (note 009)", "–", "–", "–"] if held else
                       [lf.rng([st[f] for st in sts]) for f in ("p50_ns", "p99_ns", "p999_ns", "max_ns")])
            rows.append([c["operation"], c["message"], c["representation"], c["bytes"] or "–", name,
                         lf.rng([st["mispredictions_per_message"] for st in sts], lambda v: f"{v:.2f}"),
                         lf.rng(shares, lambda v: f"{100 * v:.0f}%")] + figures)
    return rows


def state_throughput_rows(lf, runs):
    t = [d["codec"]["throughput"] for _, d in runs]
    rows = []
    for path in t[0]["paths"]:
        entries = [(i, st) for i, x in enumerate(t) for st in x["paths"][path]["states"]]
        for name, g in by_state(entries, f"throughput {path}"):
            sts = [st for _, st in g]
            rows.append([path, name, lf.rng([st["mispredictions_per_message"] for st in sts], lambda v: f"{v:.2f}"),
                         sum(st["repeats"] for st in sts),
                         lf.rng([st["p50_per_s"] / 1e6 for st in sts], lambda v: f"{v:.2f}"),
                         f"{min(st['min_per_s'] for st in sts) / 1e6:.2f}",
                         f"{max(st['max_per_s'] for st in sts) / 1e6:.2f}"])
    return t[0], rows


def state_section(lf, note, runs, withheld):
    c0 = runs[0][1]["codec"]
    t0, trows = state_throughput_rows(lf, runs)
    ghz = [x for _, d in runs for x in (d["codec"]["core_ghz_start"], d["codec"]["core_ghz_end"])]
    return [f"## Codec (note {note})", "",
            f"Per call, timed between `start()` and `stop()` and not subtracted: the first row is the empty "
            f"window, the clock's own cost. CPU {c0['cpu']}, core clock {min(ghz):.2f}–{max(ghz):.2f} GHz (measured at "
            f"each run's start and end); {c0['inputs']:,} different messages visited in a random order, each in its "
            f"own 512-byte slot. Each case runs as {c0['episodes']} episodes of {c0['messages_per_episode']:,} "
            f"timed calls ({c0['warmup_per_episode']:,} warm-up discarded per episode), each after the same fixed "
            f"work ({c0['prior_work']}). On this CPU the branch predictor has two stable states for the decode "
            f"code, and the work run just before decoding sets which one an episode gets (note 009): episodes are "
            f"classified by their counted branch mispredictions per message, and results are given per state, "
            f"never pooled. The share of episodes in each state is as observed and is not reproducible: it "
            f"drifts with conditions outside the code. Decoding: A, fields copied into a hash map (the "
            f"conventional design, the baseline, note 002); scalar, views over the buffer found a byte at a time; "
            f"SIMD, the same with SOH found 32 bytes at a time and a 32-byte CheckSum; SIMD padded, the same on a "
            f"buffer with 64 bytes of padding; two-stage, bitmaps of '=' and SOH built first, here per message "
            f"(note 009). Encoding (note 002): A, fields as strings in a vector; B, appended to a fixed buffer; C, "
            f"a pre-encoded NewOrderSingle with fixed-width slots; each formats the timestamp and ClOrdID per "
            f"order. Harness: lockfree-ipc `{c0['lockfree_ipc_commit'][:7]}`.", "",
            lf.table(["Operation", "Message", "Path", "Bytes", "State", "Mispred./msg", "Share", "p50", "p99",
                      "p99.9", "max"], state_rows(lf, runs, withheld)),
            "",
            f"Throughput, {t0['operation']}, {t0['messages']:,} messages (a random sequence of the inputs) in "
            f"batches of whole messages up to {t0['batch_bytes']:,} bytes (two-stage indexes each batch inside the "
            f"timing), 21 repeats per run, each after the same fixed work, classified by state as above; million "
            f"messages per second:", "",
            lf.table(["Path", "State", "Mispred./msg", "Repeats", "Median", "Slowest repeat", "Fastest repeat"],
                     trows), ""]


def codec_rows(lf, runs):
    rows = []
    for k in CASE_ORDER:
        cs = [d["codec"]["cases"][k] for _, d in runs if k in d["codec"]["cases"]]
        if not cs:
            continue
        c = cs[0]
        rows.append([c["operation"], c["message"], c["representation"], c["bytes"] or "–",
                     lf.rng([x["p50_ns"] for x in cs]), lf.rng([x["p99_ns"] for x in cs]),
                     lf.rng([x["p999_ns"] for x in cs]), lf.rng([x["max_ns"] for x in cs])])
    return rows


def throughput_rows(lf, runs):
    t = [d["codec"]["throughput"] for _, d in runs]
    if "paths" not in t[0]:  # note 002's runs: one path, whole stream, no batches
        t = [{**x, "batch_bytes": 0, "paths": {"B": x}} for x in t]
    rows = []
    for path in t[0]["paths"]:
        xs = [x["paths"][path] for x in t]
        rows.append([path, lf.rng([x["p50_per_s"] / 1e6 for x in xs], lambda v: f"{v:.2f}"),
                     f"{min(x['min_per_s'] for x in xs) / 1e6:.2f}", f"{max(x['max_per_s'] for x in xs) / 1e6:.2f}"])
    return t[0], rows


def codec_section(lf, note, runs):
    c0 = runs[0][1]["codec"]
    any_case = c0["cases"]["none"]
    t0, trows = throughput_rows(lf, runs)
    return [f"## Codec (note {note})", "",
            f"Per call, timed between `start()` and `stop()` and not subtracted: the first row is the empty "
            f"window, the clock's own cost. CPU {c0['cpu']}, {any_case['samples']:,} samples per case, "
            f"{any_case['warmup_discarded']:,} warm-up discarded, {c0['inputs']:,} different messages visited in a "
            f"random order (cycling in a fixed order lets the branch predictor memorise the cycle), each in its own "
            f"512-byte slot. Decoding: A, fields copied into a hash map (the conventional "
            f"design, the baseline, note 002); scalar, views over the buffer found a byte at a time; SIMD, the "
            f"same with SOH found 32 bytes at a time and a 32-byte CheckSum; SIMD padded, the same on a buffer "
            f"with 64 bytes of padding; two-stage, bitmaps of '=' and SOH built first, here per message (note "
            f"009). Encoding (note 002): A, fields as strings in a vector; B, appended to a fixed buffer; C, a "
            f"pre-encoded NewOrderSingle with fixed-width slots; each formats the timestamp and ClOrdID per "
            f"order. Harness: lockfree-ipc `{c0['lockfree_ipc_commit'][:7]}`.", "",
            lf.table(["Operation", "Message", "Path", "Bytes", "p50", "p99", "p99.9", "max"], codec_rows(lf, runs)),
            "",
            (f"Throughput, {t0['operation']}, {t0['messages']:,} messages (a random sequence of the inputs) in batches of whole messages up to "
             f"{t0['batch_bytes']:,} bytes (what one socket read hands the decoder; two-stage indexes each batch "
             f"inside the timing), 21 repeats per run, million messages per second:" if t0["batch_bytes"] else
             f"Throughput, {t0['operation']}, {t0['messages']:,} messages, 21 repeats per run, million "
             f"messages per second:"), "",
            lf.table(["Path", "Median", "Slowest repeat", "Fastest repeat"], trows), ""]


def results_md(lf, sets, withheld):
    lines = ["# Results", "",
             "Generated by `python run.py report` from the runs listed in `results/published.toml`. "
             "Do not edit: `python run.py report --check` fails if this file and the JSON disagree. "
             "Latencies in ns unless marked µs; percentiles only.", ""]
    if not sets:
        return "\n".join(lines + ["No published results yet.", ""]).rstrip() + "\n"
    lines += lf.conditions(sets)
    for name in SETS:
        if name in sets:
            note, runs = sets[name]
            lines += (state_section(lf, note, runs, withheld[name]) if is_episodes(runs)
                      else codec_section(lf, note, runs))
    return "\n".join(lines).rstrip() + "\n"


def readme_blocks(lf, sets, withheld):
    if "codec" not in sets:
        return {"codec-summary": "Codec figures: not yet measured (design note 002).\n"}
    runs = sets["codec"][1]
    if is_episodes(runs):
        rows = [r for r in state_rows(lf, runs, withheld["codec"]) if r[1] == "NewOrderSingle" and r[0] != "frame"]
        return {"codec-summary": lf.table(["Operation", "Message", "Repr.", "Bytes", "State", "Mispred./msg", "Share",
                                           "p50", "p99", "p99.9", "max"], rows) +
                f"\n\nNs per call, range over {len(runs)} runs, clock cost included (about the empty window's p50). "
                f"The branch predictor has two stable states for this code; figures are per state, and the share "
                f"of each is not reproducible (note 009). Full tables and conditions: [RESULTS.md](RESULTS.md).\n"}
    rows = [r for r in codec_rows(lf, runs) if r[1] == "NewOrderSingle" and r[0] != "frame"]
    return {"codec-summary": lf.table(["Operation", "Message", "Repr.", "Bytes", "p50", "p99", "p99.9", "max"], rows) +
            f"\n\nNs per call, range over {len(runs)} runs, clock cost included (about the empty window's p50). "
            f"Full tables and conditions: [RESULTS.md](RESULTS.md).\n"}


def report(root, check):
    lf, lf_root = harness(root)
    sets, withheld = load_sets(root, lf, lf_root)
    results = results_md(lf, sets, withheld)
    readme_path = root / "README.md"
    readme = readme_path.read_text()
    blocks = readme_blocks(lf, sets, withheld)
    found = {m["name"] for m in BLOCK.finditer(readme)}
    if set(blocks) - found:
        raise ReportError(f"README.md has no block for {sorted(set(blocks) - found)}")
    new_readme = BLOCK.sub(lambda m: m.group(1) + blocks.get(m["name"], m["body"]) + m.group(4), readme)
    for text, what in ((results, "RESULTS.md"), (new_readme, "README.md")):
        if MEAN.findall(text):
            raise ReportError(f"{what}: generated text mentions {MEAN.findall(text)}")
    if not check:
        (root / "RESULTS.md").write_text(results)
        readme_path.write_text(new_readme)
        print(f"wrote RESULTS.md and {len(blocks)} README block(s)")
        return 0
    diffs = []
    current = (root / "RESULTS.md").read_text() if (root / "RESULTS.md").is_file() else ""
    if current != results:
        diffs.append("RESULTS.md")
    old = {m["name"]: m["body"] for m in BLOCK.finditer(readme)}
    new = {m["name"]: m["body"] for m in BLOCK.finditer(new_readme)}
    diffs += [f"README.md block '{k}'" for k in new if old.get(k) != new[k]]
    if diffs:
        print("report --check: out of date: " + ", ".join(diffs) + " (run: python run.py report)")
        return 1
    print("report --check: RESULTS.md and README.md agree with the published JSON")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    rp = sub.add_parser("report", help="generate RESULTS.md and README blocks from published JSON")
    rp.add_argument("--check", action="store_true", help="compare only; exit 1 on any difference")
    rp.add_argument("--root", type=Path, default=Path(__file__).resolve().parent, help=argparse.SUPPRESS)
    args = ap.parse_args()
    try:
        return report(args.root, args.check)
    except ReportError as e:
        print(f"report: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
