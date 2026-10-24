#!/usr/bin/env python3
"""Results report: RESULTS.md and the README's figures, generated from committed JSON.

  python run.py report          write RESULTS.md and fill the README's result blocks
  python run.py report --check  regenerate in memory; exit 1 naming any difference
  python run.py conformance [--check]
                                CONFORMANCE.md from the conformance test's output (note 006)

As lockfree-ipc's report (its note 007), and built on it: its environment
checker, conditions table and formatting are used from the copy CMake fetched
at the pinned commit (build/_deps/lockfree_ipc-src), so a run here is held to
the same rules. Only files listed in results/published.toml are read, each
validated first: environment checker, published, bench mode, clean build, its
component's block present, one commit, machine and kernel command line per
set. Figures are min-max over a set's runs. Standard library only.

Runs measured as episodes (note 009, amendment of 2026-10-22) are published
per predictor state: each case's states from every run are grouped across the
set by mispredictions per message (states at least 1.5x and 0.5 apart, as the
benchmark separates them), and each state's p50 must agree across runs within
3% (S8) unless the manifest lists the case under `withheld`.
"""
import argparse
import importlib.util
import json
import re
import subprocess
import sys
import tomllib
from pathlib import Path

SETS = ("codec", "store", "net", "order", "order_qf", "stage", "stage_qf")
BLOCK = re.compile(r"(<!-- results:(?P<name>[a-z0-9-]+) -->\n)(?P<body>.*?)(<!-- /results -->)", re.S)
MEAN = re.compile(r"\b(mean|average|avg)\b", re.I)


class ReportError(Exception):
    pass


def harness(root, lfipc=None):
    """lockfree-ipc's run.py, from the fetched copy, as a module. `lfipc` is
    where CMake fetched it (the test passes it); by default build/'s copy."""
    path = (Path(lfipc) if lfipc else root / "build" / "_deps" / "lockfree_ipc-src") / "run.py"
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
            counts = [next((st["episodes"] for r, st in g if r == i), 0) for i in range(n)]
            figures = (["withheld (note 009)", "–", "–", "–"] if held else
                       [lf.rng([st[f] for st in sts]) for f in ("p50_ns", "p99_ns", "p999_ns", "max_ns")])
            rows.append([c["operation"], c["message"], c["representation"], c["bytes"] or "–", name,
                         lf.rng([st["mispredictions_per_message"] for st in sts], lambda v: f"{v:.2f}"),
                         lf.rng(counts, str), lf.rng([cs["mixed_episodes"] for _, cs in present], str)] + figures)
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
            f"never pooled. Episodes: how many of a run's {c0['episodes']} were in the state; they come in "
            f"streaks (a case typically starts in one state and changes once or a few times), so the counts are "
            f"not probabilities and are not reproducible. Mixed: episodes during which the state changed, kept "
            f"out of every state's figures. Decoding: A, fields copied into a hash map (the "
            f"conventional design, the baseline, note 002); scalar, views over the buffer found a byte at a time; "
            f"SIMD, the same with SOH found 32 bytes at a time and a 32-byte CheckSum; SIMD padded, the same on a "
            f"buffer with 64 bytes of padding; two-stage, bitmaps of '=' and SOH built first, here per message "
            f"(note 009). Encoding (note 002): A, fields as strings in a vector; B, appended to a fixed buffer; C, "
            f"a pre-encoded NewOrderSingle with fixed-width slots; each formats the timestamp and ClOrdID per "
            f"order. Harness: lockfree-ipc `{c0['lockfree_ipc_commit'][:7]}`.", "",
            lf.table(["Operation", "Message", "Path", "Bytes", "State", "Mispred./msg", "Episodes", "Mixed", "p50",
                      "p99", "p99.9", "max"], state_rows(lf, runs, withheld)),
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


GUARANTEE = {"A": "power loss (controller cache protected)", "B": "power loss (controller cache protected)",
             "C": "the process dying; not power loss", "D": "the loss of this machine, with the standby elsewhere"}


def store_rows(lf, runs):
    rows = []
    pts = runs[0][1]["store"]["points"]
    for i, p0 in enumerate(pts):
        ps = [d["store"]["points"][i] for _, d in runs]
        if any((p["strategy"], p["bound"], p["rate_per_s"]) != (p0["strategy"], p0["bound"], p0["rate_per_s"]) for p in ps):
            raise ReportError("store: runs measured different points")
        if not all(p["complete"] for p in ps):
            raise ReportError(f"store: {p0['strategy']} {p0['bound']} at {p0['rate_per_s']:.0f}/s did not complete in every run")
        sustained = min(p["sustained_per_s"] for p in ps)
        rows.append([p0["strategy"], p0["bound"], f"{p0['rate_per_s']:,.0f}",
                     lf.rng([p["p50_ns"] for p in ps]), lf.rng([p["p99_ns"] for p in ps]),
                     lf.rng([p["p999_ns"] for p in ps]), lf.rng([p["max_ns"] for p in ps]),
                     f"{sustained:,.0f}" + (" (overloaded)" if sustained < 0.95 * p0["rate_per_s"] else "")])
    return rows


def store_section(lf, note, runs):
    s0 = runs[0][1]["store"]
    rp = [d["store"]["replay"] for _, d in runs]
    return [f"## Store and recovery (note {note})", "",
            f"Time from an order's scheduled arrival to the release of its bytes, by durability strategy, open "
            f"loop at fixed rates (queueing included), {s0['samples']:,} samples per point, "
            f"{s0['warmup_discarded']:,} warm-up discarded; the session on CPU {s0['cpu']}, D's standby on CPU "
            f"{s0['standby_cpu']} over loopback (the protocol's cost, without a network); journals on "
            f"{s0['journal_filesystem']} on an HPE Smart Array P440ar logical volume (one SAS disk, RAID 0) with a "
            f"battery-backed write cache. Each strategy survives: "
            + "; ".join(f"{k}, {v}" for k, v in GUARANTEE.items()) + ". Sustained: orders released per second, "
            f"first scheduled to last released, the lowest over the runs; overloaded where below the offered rate. "
            f"Harness: lockfree-ipc `{s0['lockfree_ipc_commit'][:7]}`.", "",
            lf.table(["Strategy", "Bound", "Offered /s", "p50", "p99", "p99.9", "max", "Sustained /s"], store_rows(lf, runs)),
            "",
            f"Replay (recovery): {rp[0]['records']:,} records ({rp[0]['bytes'] / 1e6:.0f} MB) in "
            + lf.rng([r["seconds"] for r in rp], lambda v: f"{v:.2f}") + " s, "
            + lf.rng([r["records_per_s"] / 1e6 for r in rp], lambda v: f"{v:.2f}") + " million records a second.", ""]


def net_groups(runs):
    """The set's runs by path label (loopback, card, ...), in the order first seen."""
    groups = {}
    for f, d in runs:
        groups.setdefault(d["net"]["path"], []).append(d["net"])
    for label, g in groups.items():
        shape = [(p["measure"], p["loop"], p["wait"], p["rate_per_s"]) for p in g[0]["points"]]
        for n in g:
            if [(p["measure"], p["loop"], p["wait"], p["rate_per_s"]) for p in n["points"]] != shape:
                raise ReportError(f"net: runs of '{label}' measured different points")
            if not all(p["complete"] for p in n["points"]):
                raise ReportError(f"net: a run of '{label}' has an incomplete point")
    return groups


def net_rows(lf, groups, figures=("p50_ns", "p99_ns")):
    first = next(iter(groups.values()))[0]
    rows = []
    for i, p0 in enumerate(first["points"]):
        what = ("round trip, " + p0["wait"]) if p0["measure"] == "round trip" else \
               f"order path, {p0['loop']}, {p0['rate_per_s']:,.0f}/s"
        row = [what]
        for label, g in groups.items():
            for f in figures:
                row.append(lf.rng([n["points"][i][f] for n in g]))
        rows.append(row)
    return rows


def net_section(lf, note, runs):
    groups = net_groups(runs)
    n0 = runs[0][1]["net"]
    header = ["Measure"] + [f"{label} {f}" for label in groups for f in ("p50", "p99")]
    conds = "; ".join(f"{label}: {len(g)} runs, busy_read {g[0].get('busy_read_us', '-')} us, card rx-usecs "
                      f"{g[0].get('card_rx_usecs', '-')}" for label, g in groups.items())
    return [f"## Transport and engine loop (note {note})", "",
            f"Round trip: a {n0['echo_bytes']}-byte message out over TCP and back from net_peer (busy-polling on its own "
            f"core), closed loop, per wait mode of our side; the floor of the kernel's path. Order path: orders "
            f"submitted by the application at a fixed rate (open loop) through the engine, the direct loop (strategy A) "
            f"or the pipeline, to net_peer's venue and its ExecutionReport back to the application's ring; latency from "
            f"each order's scheduled time; every order pays two fdatasync calls (the order journaled before it is sent, "
            f"its ExecutionReport before it reaches the application). Application on CPU {n0['app_cpu']}, engine "
            f"{n0['engine_cpu']}, pipeline journal {n0['journal_cpu']} and sender {n0['sender_cpu']}. Paths: loopback "
            f"(the kernel's TCP code, no card); card: the two ports of a Broadcom BCM57810 (10GBASE-T, bnx2x) joined by "
            f"a Cat6 cable, the far end in its own network namespace; 10GBASE-T's line coding costs ~2-2.6 us per link, "
            f"which SFP+ or fibre links do not. {conds}. Harness: lockfree-ipc `{n0['lockfree_ipc_commit'][:7]}`.", "",
            lf.table(header, net_rows(lf, groups)), ""]


def order_points(runs, block):
    """Each point by (gateway, loop, journal, wait, rate), its figures across the set's runs."""
    points = {}
    for f, d in runs:
        for p in d[block]["points"]:
            key = (p["gateway"], p["loop"], p["journal"], p["wait"], p["rate_per_s"])
            points.setdefault(key, []).append(p)
    return points


def sustained(p):
    return p["complete"] and p["sustained_per_s"] >= 0.95 * p["rate_per_s"]


def config_name(key):
    gateway, loop, journal, wait, _ = key
    if gateway == "quickfix":
        return f"QuickFIX 1.16.0, {journal}"
    return f"ours, {loop}, journal {journal}, {wait}"


def order_rows(lf, sets, rates=None, figures=("p50_ns", "p99_ns", "p999_ns", "max_ns")):
    """Rows for every configuration and rate measured in every run of its set; a
    rate not sustained in every run says so instead of giving a latency (S5)."""
    rows, highest, partial, top = [], {}, [], {}
    for name in ("order", "order_qf"):
        if name not in sets:
            continue
        runs = sets[name][1]
        for key, ps in order_points(runs, name).items():
            cfg = key[:4]
            top[cfg] = max(top.get(cfg, 0), key[4])
            if len(ps) < len(runs):
                partial.append(f"{config_name(key)} at {key[4]:,.0f}/s")
                continue
            ok = all(sustained(p) for p in ps)
            if ok:
                highest[cfg] = max(highest.get(cfg, 0), key[4])
            if rates is not None and key[4] not in rates:
                continue
            cells = [lf.rng([p[f] for p in ps]) for f in figures] if ok else ["not sustained"] * len(figures)
            venue = lf.rng([p["venue_p50_ns"] for p in ps]) if all(p["venue_read"] for p in ps) else "-"
            rows.append([config_name(key), ps[0]["survives"], f"{key[4]:,.0f}"] + cells + [venue])
    # Sustained at the highest rate offered: a lower bound, not a ceiling.
    highest = {k: (f"≥ {v:,.0f}/s" if v == top[k] else f"{v:,.0f}/s") for k, v in highest.items()}
    return rows, highest, partial


def journal_fs(sets):
    """Where our journals were: A on the disk, C on tmpfs (note 004's amendment); runs before it recorded neither (ext4)."""
    if "order" not in sets:
        return "–"
    fs = {(d["order"].get("journal_a_filesystem", "ext4"), d["order"].get("journal_c_filesystem", "ext4")) for _, d in sets["order"][1]}
    if len(fs) != 1:
        raise ReportError("order: runs kept their journals on different file systems")
    a, c = fs.pop()
    return f"A on {a}, C on {c}"


def order_section(lf, sets):
    rows, highest, partial = order_rows(lf, sets)
    notes = [sets[n][0] for n in ("order", "order_qf") if n in sets]
    counts = "; ".join(f"{n}: {len(sets[n][1])} runs" for n in ("order", "order_qf") if n in sets)
    peak = "; ".join(f"{config_name(k + (0,))}: {v}" for k, v in highest.items())
    out = [f"## Order to ExecutionReport (note {notes[0]})", "",
           "Application to application, open loop: limit NewOrderSingles submitted on a fixed schedule, latency from each "
           "order's scheduled time to its ExecutionReport (New) reaching the application; 100,000 orders a point after 5,000 "
           "of warm-up; over loopback to order_venue (our venue simulator behind an acceptor session, its own process, "
           "busy-polling on its own core), the same venue for both gateways. Ours: the application on CPU 2, the engine on 4 "
           "(the pipeline's journal and sender stages on 6 and 8). QuickFIX: a program written to its documented API, the "
           "application thread on CPU 2 and its other threads confined to as many reserved CPUs as there are. 'Survives' "
           "is the configuration's guarantee: QuickFIX's file store never syncs (counted: 0 fsync and 0 fdatasync calls), so "
           "it survives a process crash like our journal C; our journal A makes each input durable with fdatasync first and "
           "survives power loss, which QuickFIX documents no setting for. 'Venue p50' is the venue's own turnaround, read "
           f"to written. Journal files: {journal_fs(sets)}. {counts}.", "",
           lf.table(["Configuration", "Survives", "Offered /s", "p50", "p99", "p99.9", "max", "Venue p50"], rows), "",
           f"Highest rate sustained in every run: {peak}.", ""]
    if partial:
        out += [f"Measured in only some runs, so not published: {'; '.join(partial)}.", ""]
    return out


def stage_rows(lf, sets):
    rows = []
    for name, who in (("stage", "ours"), ("stage_qf", "QuickFIX 1.16.0")):
        if name not in sets:
            continue
        runs = sets[name][1]
        for i, p0 in enumerate(runs[0][1][name]["points"]):
            ps = [d[name]["points"][i] for _, d in runs]
            if any(p["stage"] != p0["stage"] for p in ps):
                raise ReportError(f"{name}: runs measured different stages")
            if not all(p["all_correct"] for p in ps):
                raise ReportError(f"{name}: a run's stage '{p0['stage']}' was not correct for every message")
            rows.append([who, p0["direction"], p0["stage"]] + [lf.rng([p[f] for p in ps]) for f in ("p50_ns", "p99_ns", "p999_ns")])
    return rows


def stage_section(lf, sets):
    note = (sets.get("stage") or sets.get("stage_qf"))[0]
    counts = "; ".join(f"{n}: {len(sets[n][1])} runs" for n in ("stage", "stage_qf") if n in sets)
    return [f"## The gateway's cost per message, by stage (note {note})", "",
            "In process, no network: each call timed between start() and stop() on CPU 2, unsubtracted, beside an empty "
            "window ('none') for the clock's own cost; 100,000 different messages after 10,000 of warm-up, visited in order "
            "(sequence numbers and ExecIDs advance), the same bytes for both (bench/stage_inputs.hpp). What a stage needs but "
            "does not own (decoding, before a check that reads a decoded message) is done outside its window. QuickFIX's "
            f"reference: a FIX::Message constructed from the same bytes with its FIX 4.4 dictionary validating. {counts}.", "",
            lf.table(["Gateway", "Message", "Stage", "p50", "p99", "p99.9"], stage_rows(lf, sets)), ""] + stage_by_run(lf, sets)


def stage_by_run(lf, sets):
    """Ours, run by run: each stage's p50 beside its branch mispredictions per
    message (counted over the stage's whole loop), for whether a stage's spread
    between runs follows the branch predictor (note 008)."""
    if "stage" not in sets:
        return []
    runs = sets["stage"][1]
    if not all("mispredictions_per_message" in p for _, d in runs for p in d["stage"]["points"]):
        return []
    head = ["Stage"] + [f"run {i + 1}: p50, mispredictions" for i in range(len(runs))]
    rows = []
    for i, p0 in enumerate(runs[0][1]["stage"]["points"]):
        cells = []
        for _, d in runs:
            p = d["stage"]["points"][i]
            m = p["mispredictions_per_message"]
            cells.append(f"{lf.ns(p['p50_ns'])}, {m:.2f}" if m >= 0 else f"{lf.ns(p['p50_ns'])}, –")
        rows.append([p0["stage"]] + cells)
    return ["Ours, run by run: each stage's p50 and its branch mispredictions per message, counted over the stage's whole "
            "loop (what it does outside the window included: the dictionary check's and the order book's include the "
            "decoding before them).", "", lf.table(head, rows), ""]


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
            if name == "store":
                lines += store_section(lf, note, runs)
            elif name == "net":
                lines += net_section(lf, note, runs)
            elif name in ("order", "order_qf"):
                if name == "order" or "order" not in sets:
                    lines += order_section(lf, sets)
            elif name in ("stage", "stage_qf"):
                if name == "stage" or "stage" not in sets:
                    lines += stage_section(lf, sets)
            else:
                lines += (state_section(lf, note, runs, withheld[name]) if is_episodes(runs)
                          else codec_section(lf, note, runs))
    return "\n".join(lines).rstrip() + "\n"


def readme_blocks(lf, sets, withheld):
    blocks = codec_blocks(lf, sets, withheld)
    if "store" in sets:
        rows = [r for r in store_rows(lf, sets["store"][1]) if r[2] == "1,000"]
        blocks["store-summary"] = (lf.table(["Strategy", "Bound", "Offered /s", "p50", "p99", "p99.9", "max", "Sustained /s"], rows)
                                   + f"\n\nOrder arrival to release of its bytes, ns, at 1,000 orders a second, range over "
                                   f"{len(sets['store'][1])} runs. Every rate, the guarantees and the conditions: [RESULTS.md](RESULTS.md).\n")
    else:
        blocks["store-summary"] = "Store figures: not yet measured (design note 004).\n"
    if "net" in sets:
        groups = net_groups(sets["net"][1])
        rows = net_rows(lf, groups, ("p50_ns",))
        blocks["net-summary"] = (lf.table(["Measure"] + [f"{label} p50" for label in groups], rows)
                                 + "\n\nRange over the runs of each path. Conditions, p99 and the method: "
                                 "[RESULTS.md](RESULTS.md).\n")
    else:
        blocks["net-summary"] = "Transport figures: not yet measured (design note 005).\n"
    if "order" in sets or "order_qf" in sets:
        rows, highest, _ = order_rows(lf, sets, rates=(1000.0, 10000.0), figures=("p50_ns", "p99_ns"))
        peak = "; ".join(f"{config_name(k + (0,))} {v}" for k, v in highest.items())
        blocks["order-summary"] = (lf.table(["Configuration", "Survives", "Offered /s", "p50", "p99", "Venue p50"], rows)
                                   + f"\n\nOrder to ExecutionReport, application to application, open loop, ns, range over the "
                                   f"runs. Highest rate sustained: {peak}. The guarantees, the method and every rate: "
                                   "[RESULTS.md](RESULTS.md).\n")
    else:
        blocks["order-summary"] = "Order-to-ExecutionReport figures: not yet measured (design note 008).\n"
    if "stage" in sets or "stage_qf" in sets:
        rows = [r[:3] + r[3:4] for r in stage_rows(lf, sets) if r[2] != "none"]
        blocks["stage-summary"] = (lf.table(["Gateway", "Message", "Stage", "p50"], rows)
                                   + "\n\nNs per call, in process, range over the runs, clock cost included. Method: "
                                   "[RESULTS.md](RESULTS.md).\n")
    else:
        blocks["stage-summary"] = "Per-message figures: not yet measured (design note 008).\n"
    return blocks


def codec_blocks(lf, sets, withheld):
    if "codec" not in sets:
        return {"codec-summary": "Codec figures: not yet measured (design note 002).\n"}
    runs = sets["codec"][1]
    if is_episodes(runs):
        rows = [r for r in state_rows(lf, runs, withheld["codec"]) if r[1] == "NewOrderSingle" and r[0] != "frame"]
        return {"codec-summary": lf.table(["Operation", "Message", "Repr.", "Bytes", "State", "Mispred./msg",
                                           "Episodes", "Mixed", "p50", "p99", "p99.9", "max"], rows) +
                f"\n\nNs per call, range over {len(runs)} runs, clock cost included (about the empty window's p50). "
                f"The branch predictor has two stable states for this code; figures are per state, never pooled, "
                f"and how many episodes fall in each is not reproducible (note 009). Full tables and conditions: "
                f"[RESULTS.md](RESULTS.md).\n"}
    rows = [r for r in codec_rows(lf, runs) if r[1] == "NewOrderSingle" and r[0] != "frame"]
    return {"codec-summary": lf.table(["Operation", "Message", "Repr.", "Bytes", "p50", "p99", "p99.9", "max"], rows) +
            f"\n\nNs per call, range over {len(runs)} runs, clock cost included (about the empty window's p50). "
            f"Full tables and conditions: [RESULTS.md](RESULTS.md).\n"}


def report(root, check, lfipc=None):
    lf, lf_root = harness(root, lfipc)
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


# The FIX Session Layer Test Cases, V1.0, June 2020 (note 006): every case in
# the document's order, whether the document makes it mandatory, a short
# title, and for a case not shown by the test, why.
NOT_IMPLEMENTED_17 = "not implemented: encryption is optional; EncryptMethod must be 0, any other is an invalid Logon (1B d, 1S d)"
CASES = [
    ("1B a", True, "Establish the transport connection"), ("1B b", True, "Send Logon"),
    ("1B c", True, "Valid Logon reply"), ("1B d", True, "Invalid Logon reply"),
    ("1B e", True, "First message not a Logon"),
    ("1S a", True, "Valid Logon"), ("1S b", True, "Logon with a duplicate identity"),
    ("1S c", True, "Logon from an unauthenticated or unconfigured identity"), ("1S d", True, "Invalid Logon"),
    ("2S", True, "First message not a Logon"),
    ("2a", True, "MsgSeqNum as expected"), ("2b", True, "MsgSeqNum higher than expected"),
    ("2c", True, "MsgSeqNum lower than expected, no PossDupFlag"), ("2d", True, "Garbled message"),
    ("2e", True, "PossDupFlag Y, OrigSendingTime not after SendingTime, MsgSeqNum lower"),
    ("2f", True, "PossDupFlag Y, OrigSendingTime after SendingTime"),
    ("2g", True, "PossDupFlag Y, OrigSendingTime missing"), ("2h", True, "BeginString as expected"),
    ("2i", True, "BeginString not as expected"), ("2j", True, "CompIDs as expected"),
    ("2k", True, "CompIDs not as expected"), ("2l", True, "BodyLength correct"), ("2m", True, "BodyLength incorrect"),
    ("2n", True, "SendingTime accurate"), ("2o", True, "SendingTime not accurate"), ("2p", True, "MsgType valid"),
    ("2q", True, "MsgType not valid"), ("2r", True, "MsgType valid but not supported"),
    ("2s", True, "BeginString, BodyLength, MsgType first"), ("2t", True, "BeginString, BodyLength, MsgType not first"),
    ("3a", True, "CheckSum valid"), ("3b", True, "CheckSum invalid"), ("3c", True, "Garbled message"),
    ("3d", True, "CheckSum last, three characters, SOH"), ("3e", True, "CheckSum not last, not three characters or no SOH"),
    ("4a", True, "Nothing sent for HeartBtInt"), ("4b", True, "TestRequest received"),
    ("5", True, "Valid Heartbeat"), ("6", True, "Nothing received for HeartBtInt plus 20%"),
    ("7", True, "Valid Reject"), ("8", True, "Valid ResendRequest"),
    ("9", False, "Synchronize sequence numbers"),
    ("10a", True, "GapFill, MsgSeqNum above expected"), ("10b", True, "GapFill, MsgSeqNum as expected"),
    ("10c", True, "GapFill, MsgSeqNum below expected, PossDupFlag Y"),
    ("10d", True, "GapFill, MsgSeqNum below expected, no PossDupFlag"), ("10e", True, "GapFill, NewSeqNo not above MsgSeqNum"),
    ("11a", True, "Reset, NewSeqNo above expected"), ("11b", True, "Reset, NewSeqNo equal to expected"),
    ("11c", True, "Reset, NewSeqNo below expected"),
    ("12", True, "Initiate logout"), ("13a", True, "Logout in reply to ours"), ("13b", True, "Unsolicited Logout"),
    ("14a", True, "Tag not defined in the specification"), ("14b", True, "Required tag missing"),
    ("14c", True, "Tag not defined for this message type"), ("14d", True, "Tag without a value"),
    ("14e", True, "Value out of range or not enumerated"), ("14f", True, "Value in the wrong format"),
    ("14g", True, "Header, body, trailer out of order"), ("14h", True, "Tag not in a group repeated"),
    ("14i", True, "NumInGroup count incorrect"), ("14j", True, "Group fields out of order"),
    ("14k", True, "Non-data value contains SOH"),
    ("14l", False, "Application not available",
     "not implemented: optional; the engine never refuses a message for being busy (it waits in the ring or the overflow, note 005)"),
    ("14m", True, "Conditionally required field missing"),
    ("14n", True, "Field in cleartext and encrypted, different values", "not applicable: encryption (17) not supported"),
    ("15", False, "Fields in different orders"),
    ("16a", True, "Message queued while disconnected"), ("16b", True, "Reconnect with queued messages"),
] + [(f"17{c}", False, "Encryption", NOT_IMPLEMENTED_17) for c in "abcdefghij"] + [
    ("18a", False, "Third-party addressing, as expected",
     "not implemented: optional; OnBehalfOfCompID and DeliverToCompID are accepted as header fields, not checked"),
    ("18b", False, "Third-party addressing, not as expected",
     "not implemented: optional; OnBehalfOfCompID and DeliverToCompID are accepted as header fields, not checked"),
    ("19a", True, "PossResend Y, already seen"),
    ("19b", True, "PossResend Y, not seen"),
    ("20", True, "ResendRequest while ours is outstanding"),
]
# FIX 4.4 Volume 4's Order State Change Matrices (note 007, D2): those in scope
# are shown by tests/matrices_test.cpp; the others say why not.
MATRICES = [
    ("A.1.a", "Filled order"), ("A.1.b", "Part-filled day order, done for day"),
    ("B.1.a", "Cancel request for a zero-filled order"), ("B.1.b", "Cancel request, executions while it is active"),
    ("B.1.c", "Cancel request, order filled before it can be accepted"),
    ("B.1.d", "Cancel request for an order not yet acknowledged"),
    ("B.1.e", "Cancel request crossing the acknowledgement"),
    ("B.1.f", "Cancel request for an unknown order"),
    ("C.1.a", "Replace to raise quantity, zero-filled"), ("C.1.b", "Replace to raise quantity, fill while pending"),
    ("C.1.c", "Replace to raise quantity of a filled order"), ("C.2.a", "Replace, not for quantity, rejected after a fill"),
    ("C.3.a", "Replace to lower quantity, above CumQty"), ("C.3.b", "Replace to lower quantity, equal to CumQty"),
    ("C.3.c", "Replace to lower quantity, below CumQty"),
    ("D.1.a", "Two replaces, both accepted"), ("D.1.b", "Replace rejected before pending, then another"),
    ("D.1.c", "Replace rejected after pending, then another"), ("D.2.a", "Chained replaces, processed in turn"),
    ("D.2.b", "Chained replaces, pending acknowledged first"), ("D.2.c", "Chained replaces, both rejected"),
    ("D.2.d", "Chained replaces, second rejected while pending"),
    ("E.1.a", "Telephoned order", "out of scope: an order the gateway did not send; its reports are delivered marked unknown"),
    ("E.1.b", "Unsolicited cancel of a part-filled order (what cancel-on-disconnect produces)"),
    ("E.1.c", "Unsolicited replacement", "out of scope: changes agreed by telephone (D2)"),
    ("E.1.d", "Unsolicited reduction of quantity", "out of scope: changes agreed by telephone or by an ECN decline (D2)"),
    ("E.1.e", "Unsolicited cancel of a cancel-if-not-best order", "out of scope: ExecInst Z, not used (D2)"),
    ("F.1.a", "Order rejected, duplicate ClOrdID"), ("F.1.b", "PossResend and duplicate ClOrdID"),
    ("F.1.c", "Order rejected, already submitted verbally", "out of scope: telephoned orders (D2)"),
] + [(f"G.1.{c}", "Order status requests", "out of scope: needs OrderStatusRequest, not among the five messages (D2)") for c in "abc"] + [
    (f"H.1.{c}", "GT orders over several days", "out of scope: GT orders and corporate actions (D2)") for c in "abcd"] + [
    ("I.1.a", "Fill or Kill that cannot be filled"), ("I.1.b", "Immediate or Cancel that cannot be immediately hit"),
    ("J.1.a", "Filled order, executions corrected and canceled"), ("J.1.b", "Canceled order, an execution busted, a new one"),
    ("J.1.c", "GT order renewed, quantities corrected"), ("J.1.d", "Done for day, a correction and a bust"),
] + [(f"K.1.{c}", "Trading halt", "out of scope: trading halts (D2)") for c in "ab"] + [
    ("L.1.a", "Guarantee of execution (Stopped)", "out of scope: stopped orders (D2)"),
    ("L.1.b", "CashOrderQty", "out of scope: orders by cash amount (D2)"),
]
MATRIX_LINE = re.compile(r"^(pass|FAIL)  ([A-L]\.\d\.[a-f]): (.*)$")

CONFORMANCE_LINE = re.compile(r"^(pass|FAIL)  (\d+[A-Z]?(?: [a-z])?|\d+[a-z]): (.*)$")


def conformance(root, check, test, matrices_test=None):
    out = subprocess.run([str(test)], capture_output=True, text=True).stdout
    matrices_test = matrices_test or Path(test).parent / "matrices_test"
    mout = subprocess.run([str(matrices_test)], capture_output=True, text=True).stdout
    shown, problems = {}, []
    for line in out.splitlines():
        m = CONFORMANCE_LINE.match(line)
        if not m:
            continue
        shown.setdefault(m.group(2), []).append((m.group(1) == "pass", m.group(3)))
    known = {c[0] for c in CASES}
    problems += [f"the test prints case {k}, which is not in the document" for k in shown if k not in known]
    rows, counts = [], {}
    for case in CASES:
        cid, mandatory, title = case[:3]
        reason = case[3] if len(case) > 3 else None
        lines = shown.get(cid, [])
        if reason and lines:
            problems.append(f"{cid}: listed as not shown ({reason.split(':')[0]}) but the test shows it")
        if not reason and not lines:
            problems.append(f"{cid}: not shown by the test")
        if reason:
            result = reason.split(":")[0]
            detail = reason.split(":", 1)[1].strip()
        else:
            result = "pass" if lines and all(ok for ok, _ in lines) else "FAIL"
            detail = "; ".join(w for _, w in lines)
        key = ("mandatory" if mandatory else "optional", result)
        counts[key] = counts.get(key, 0) + 1
        rows.append(f"| {cid} | {'yes' if mandatory else 'no'} | {title} | {result} | {detail} |")
    problems += [f"{r.split('|')[1].strip()}: FAIL" for r in rows if "| FAIL |" in r]
    summary = ", ".join(f"{n} {kind} {res}" for (kind, res), n in sorted(counts.items()))
    text = (
        "# Conformance\n\n"
        "Generated by `python run.py conformance` from the output of `tests/conformance_test.cpp`; do not edit.\n"
        "Basis: *FIX Session Layer Test Cases*, Version 1.0, June 2020, FIX Protocol Limited, every case in its\n"
        "order. Design: [note 006](docs/design/006-conformance.md). Each result shown by the test lists what it\n"
        "checked; a case not shown says why.\n\n"
        f"{len(CASES)} cases: {summary}.\n\n"
        "| Case | Mandatory | Condition | Result | Shown by, or why not |\n|---|---|---|---|---|\n"
        + "\n".join(rows) + "\n")
    mshown = {}
    for line in mout.splitlines():
        m = MATRIX_LINE.match(line)
        if m:
            mshown.setdefault(m.group(2), []).append((m.group(1) == "pass", m.group(3)))
    mknown = {c[0] for c in MATRICES}
    problems += [f"matrices_test prints {k}, which is not in Volume 4's list" for k in mshown if k not in mknown]
    mrows, mcounts = [], {}
    for case in MATRICES:
        cid, title = case[:2]
        reason = case[2] if len(case) > 2 else None
        lines = mshown.get(cid, [])
        if reason and lines:
            problems.append(f"{cid}: listed out of scope but the test shows it")
        if not reason and not lines:
            problems.append(f"{cid}: not shown by matrices_test")
        if reason:
            result, detail = "out of scope", reason.split(":", 1)[1].strip()
        else:
            result = "pass" if all(ok for ok, _ in lines) else "FAIL"
            detail = "; ".join(w for _, w in lines)
        mcounts[result] = mcounts.get(result, 0) + 1
        mrows.append(f"| {cid} | {title} | {result} | {detail} |")
    problems += [f"{r.split('|')[1].strip()}: FAIL" for r in mrows if "| FAIL |" in r]
    msummary = ", ".join(f"{n} {res}" for res, n in sorted(mcounts.items()))
    text += (
        "\n## Order State Change Matrices\n\n"
        "FIX 4.4 with Errata 20030618, Volume 4, every matrix in its order; shown by `tests/matrices_test.cpp`\n"
        "([note 007](docs/design/007-order-entry.md), D2): *scripted*, the venue's rows sent as the matrix gives them\n"
        "and our order's state checked after each; *simulated*, the venue simulator answering our requests, its\n"
        "reports checked against the same rows.\n\n"
        f"{len(MATRICES)} matrices: {msummary}.\n\n"
        "| Matrix | Scenario | Result | Shown by, or why not |\n|---|---|---|---|\n" + "\n".join(mrows) + "\n")
    path = root / "CONFORMANCE.md"
    if problems:
        print("conformance: " + "; ".join(problems))
        return 1
    if not check:
        path.write_text(text)
        print(f"wrote CONFORMANCE.md: {len(CASES)} cases, {summary}")
        return 0
    if not path.is_file() or path.read_text() != text:
        print("conformance --check: CONFORMANCE.md is out of date (run: python run.py conformance)")
        return 1
    print("conformance --check: CONFORMANCE.md agrees with the test")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    rp = sub.add_parser("report", help="generate RESULTS.md and README blocks from published JSON")
    rp.add_argument("--check", action="store_true", help="compare only; exit 1 on any difference")
    rp.add_argument("--root", type=Path, default=Path(__file__).resolve().parent, help=argparse.SUPPRESS)
    rp.add_argument("--lockfree-ipc", dest="lfipc", help="the fetched lockfree-ipc (default: build/_deps/lockfree_ipc-src)")
    cp = sub.add_parser("conformance", help="generate CONFORMANCE.md from the conformance test's output")
    cp.add_argument("--check", action="store_true", help="compare only; exit 1 on any difference")
    cp.add_argument("--root", type=Path, default=Path(__file__).resolve().parent, help=argparse.SUPPRESS)
    cp.add_argument("--test", type=Path, help="the conformance test (default: build/conformance_test)")
    cp.add_argument("--matrices", type=Path, help="the matrices test (default: beside the conformance test)")
    args = ap.parse_args()
    if args.cmd == "conformance":
        return conformance(args.root, args.check, args.test or args.root / "build" / "conformance_test", args.matrices)
    try:
        return report(args.root, args.check, args.lfipc)
    except ReportError as e:
        print(f"report: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
