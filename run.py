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
    return {n: (m[n].get("note", ""), validate(root, lf, lf_root, n, m[n]["runs"])) for n in SETS if n in m}


CASE_ORDER = ("none", "decode_b_nos", "decode_a_nos", "decode_b_er", "decode_a_er", "decode_b_hb",
              "encode_a_nos", "encode_b_nos", "encode_c_nos")


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


def codec_section(lf, note, runs):
    c0 = runs[0][1]["codec"]
    any_case = c0["cases"]["none"]
    t = [d["codec"]["throughput"] for _, d in runs]
    return [f"## Codec (note {note})", "",
            f"Per call, timed between `start()` and `stop()` and not subtracted: the first row is the empty "
            f"window, the clock's own cost. CPU {c0['cpu']}, {any_case['samples']:,} samples per case, "
            f"{any_case['warmup_discarded']:,} warm-up discarded, inputs cycled over {c0['inputs']:,} different "
            f"messages. A: fields copied into a hash map (the conventional design, the baseline); B: views over "
            f"the receive buffer, fields appended to a fixed buffer; C: a pre-encoded NewOrderSingle with "
            f"fixed-width slots. Every encoder formats the timestamp and ClOrdID per order. Harness: "
            f"lockfree-ipc `{c0['lockfree_ipc_commit'][:7]}`.", "",
            lf.table(["Operation", "Message", "Repr.", "Bytes", "p50", "p99", "p99.9", "max"], codec_rows(lf, runs)),
            "",
            f"Throughput, {t[0]['operation']}, {t[0]['messages']:,} messages, {t[0]['repeats']} repeats per run: "
            f"median {lf.rng([x['p50_per_s'] / 1e6 for x in t], lambda v: f'{v:.2f}')} million messages per "
            f"second (slowest repeat {min(x['min_per_s'] for x in t) / 1e6:.2f}M, fastest "
            f"{max(x['max_per_s'] for x in t) / 1e6:.2f}M).", ""]


def results_md(lf, sets):
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
            lines += codec_section(lf, note, runs)
    return "\n".join(lines).rstrip() + "\n"


def readme_blocks(lf, sets):
    if "codec" not in sets:
        return {"codec-summary": "Codec figures: not yet measured (design note 002).\n"}
    runs = sets["codec"][1]
    rows = [r for r in codec_rows(lf, runs) if r[1] == "NewOrderSingle"]
    return {"codec-summary": lf.table(["Operation", "Message", "Repr.", "Bytes", "p50", "p99", "p99.9", "max"], rows) +
            f"\n\nNs per call, range over {len(runs)} runs, clock cost included (about the empty window's p50). "
            f"Full tables and conditions: [RESULTS.md](RESULTS.md).\n"}


def report(root, check):
    lf, lf_root = harness(root)
    sets = load_sets(root, lf, lf_root)
    results = results_md(lf, sets)
    readme_path = root / "README.md"
    readme = readme_path.read_text()
    blocks = readme_blocks(lf, sets)
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
