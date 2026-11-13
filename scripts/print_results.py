#!/usr/bin/env python3
"""
Print Speedup / DRAM / coverage / accuracy for L1D prefetchers.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
from collections import defaultdict
from pathlib import Path

_NUM_PREFIX_RE = re.compile(r"^\d+\.")


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def load_suite(suite_name: str) -> dict:
    path = repo_root() / "scripts" / "suites" / f"{suite_name}.json"
    if not path.is_file():
        raise FileNotFoundError(
            f"suite definition not found: {path} "
            f"(expected scripts/suites/{suite_name}.json)"
        )
    with open(path) as fh:
        return json.load(fh)


# ---------------------------------------------------------------------------
# ChampSim ROI parsers 
# ---------------------------------------------------------------------------

def _infer_cpu_str(prefetcher_dir: str | None) -> str | None:
    if not prefetcher_dir:
        return "cpu0_L1D"
    if prefetcher_dir in ("no", "no-no"):
        return None
    comps = prefetcher_dir.split("-")
    if len(comps) >= 2:
        l1_pref, l2_pref = comps[0], comps[1]
        if not l2_pref.startswith("no"):
            return "cpu0_L2C"
        if not l1_pref.startswith("no"):
            return "cpu0_L1D"
        return None
    return "cpu0_L1D"


def _iter_roi_lines(filepath: str):
    in_roi = False
    with open(filepath) as fh:
        for line in fh:
            line = line.strip()
            if "Region of Interest Statistics" in line:
                in_roi = True
                continue
            if in_roi:
                yield line


def parse_speedup(filepath, prefetcher_dir=None):
    """Parse ROI IPC; aggregator turns baseline/test IPC into speedup."""
    for line in _iter_roi_lines(filepath):
        if line.startswith("CPU 0 cumulative IPC:"):
            parts = line.split()
            try:
                return float(parts[4])
            except (IndexError, ValueError):
                return None
    return None


def parse_dram(filepath, prefetcher_dir=None):
    for line in _iter_roi_lines(filepath):
        if line.startswith("LLC TOTAL"):
            parts = line.split()
            try:
                return float(parts[7])
            except (IndexError, ValueError):
                return None
    return None


def parse_coverage(filepath, prefetcher_dir=None):
    cpu_str = _infer_cpu_str(prefetcher_dir)
    if cpu_str is None:
        return None
    useful = None
    demand_misses = None
    for line in _iter_roi_lines(filepath):
        if f"{cpu_str} PREFETCH REQUESTED:" in line:
            parts = line.split()
            try:
                useful = float(parts[7])
            except (IndexError, ValueError):
                pass
        elif f"{cpu_str} LOAD" in line:
            parts = line.split()
            try:
                demand_misses = float(parts[7])
            except (IndexError, ValueError):
                pass
    if useful is not None and demand_misses is not None:
        return useful, demand_misses
    return None


def parse_accuracy(filepath, prefetcher_dir=None):
    cpu_str = _infer_cpu_str(prefetcher_dir)
    if cpu_str is None:
        return None
    for line in _iter_roi_lines(filepath):
        if f"{cpu_str} PREFETCH REQUESTED:" in line:
            parts = line.split()
            try:
                return float(parts[7]), float(parts[9])
            except (IndexError, ValueError):
                return None
    return None


METRICS = {
    "speedup": {
        "parse": parse_speedup,
        "aggregate": "speedup",
        "ylabel": "IPC Speedup",
    },
    "dram": {
        "parse": parse_dram,
        "aggregate": "ratio",
        "ylabel": "Normalized DRAM Traffic",
    },
    "coverage": {
        "parse": parse_coverage,
        "aggregate": "absolute",
        "ylabel": "Coverage",
        "value_kind": "coverage",
    },
    "accuracy": {
        "parse": parse_accuracy,
        "aggregate": "absolute",
        "ylabel": "Accuracy",
        "value_kind": "accuracy",
    },
}


# ---------------------------------------------------------------------------
# Gather + aggregate
# ---------------------------------------------------------------------------

def _strip_numeric_prefix(name: str) -> str:
    m = _NUM_PREFIX_RE.match(name)
    return name[m.end():] if m else name


def gather_data(results_dir, suite_name, prefetchers, parse_fn, bench_map):
    data = defaultdict(dict)
    unparsed = defaultdict(list)

    for prefetcher in prefetchers:
        pref_dir = os.path.join(results_dir, suite_name, prefetcher)
        if not os.path.isdir(pref_dir):
            print(f"  warning: missing prefetcher directory: {pref_dir}")
            continue

        for benchmark in bench_map:
            bench_dir = os.path.join(pref_dir, benchmark)
            if not os.path.isdir(bench_dir):
                print(f"  warning: missing benchmark directory: {bench_dir}")
                continue

            for filename in os.listdir(bench_dir):
                if not filename.endswith(".txt"):
                    continue
                simpoint = _strip_numeric_prefix(filename[:-4])
                filepath = os.path.join(bench_dir, filename)
                value = parse_fn(filepath, prefetcher)
                if value is None:
                    unparsed[prefetcher].append(f"{benchmark}/{simpoint}")
                    continue
                data[prefetcher][f"{benchmark}/{simpoint}"] = value

    return data, unparsed


def _effective_simpoint_weight(w):
    if w is None:
        return 1.0
    wf = float(w)
    return wf if wf > 0 else None


def _weighted_arithmetic_mean(values, weights):
    if not values:
        return 0.0
    total_w = sum(weights)
    if total_w == 0:
        return sum(values) / len(values)
    return sum(v * w for v, w in zip(values, weights)) / total_w


def _weighted_harmonic_speedup(base_speedups, test_speedups, weights):
    if not weights or not base_speedups or not test_speedups:
        return 0.0
    base_time = 0.0
    test_time = 0.0
    for b, t, w in zip(base_speedups, test_speedups, weights):
        if b <= 0 or t <= 0:
            return 0.0
        base_time += w / b
        test_time += w / t
    if test_time == 0:
        return 0.0
    return base_time / test_time


def _weighted_geomean_speedup(base_speedups, test_speedups, weights):
    if not weights or not base_speedups or not test_speedups:
        return 0.0
    log_sum = 0.0
    total_w = 0.0
    for b, t, w in zip(base_speedups, test_speedups, weights):
        if b <= 0 or t <= 0:
            return 0.0
        log_sum += math.log(t / b) * w
        total_w += w
    if total_w == 0:
        return 0.0
    return math.exp(log_sum / total_w)


def _geomean(values):
    values = [v for v in values if v > 0]
    if not values:
        return 0.0
    return math.exp(sum(math.log(v) for v in values) / len(values))


def _convert_absolute(raw, value_kind):
    if value_kind == "coverage":
        useful, demand_misses = raw
        if useful == 0:
            return None
        return useful / (useful + demand_misses)
    if value_kind == "accuracy":
        useful, useless = raw
        if useful == 0:
            return 1.0
        return useful / (useful + useless)
    if value_kind is None:
        return float(raw)
    raise ValueError(f"unknown value_kind: {value_kind}")


def aggregate(data, bench_map, prefetchers, baseline, metric_spec,
              speedup_mean_type="harmonic"):
    benchmark_order = list(bench_map.keys())
    mode = metric_spec["aggregate"]
    if mode in ("speedup", "ratio") and baseline is None:
        raise ValueError(
            f"aggregate mode '{mode}' requires a baseline prefetcher"
        )

    plot_prefetchers = [p for p in prefetchers if p != baseline]
    results = defaultdict(dict)

    for benchmark in benchmark_order:
        weight_map = bench_map[benchmark]
        simpoints = list(weight_map.keys())

        for prefetcher in plot_prefetchers:
            if mode == "speedup":
                base_vals, test_vals, weights = [], [], []
                for sp in simpoints:
                    label = f"{benchmark}/{_strip_numeric_prefix(sp)}"
                    b = data.get(baseline, {}).get(label)
                    t = data.get(prefetcher, {}).get(label)
                    if b is None or t is None or b <= 0 or t <= 0:
                        continue
                    w = _effective_simpoint_weight(weight_map[sp])
                    if w is None:
                        continue
                    base_vals.append(float(b))
                    test_vals.append(float(t))
                    weights.append(w)
                if not weights:
                    results[prefetcher][benchmark] = 0.0
                    continue
                if speedup_mean_type == "harmonic":
                    v = _weighted_harmonic_speedup(base_vals, test_vals, weights)
                else:
                    v = _weighted_geomean_speedup(base_vals, test_vals, weights)
                results[prefetcher][benchmark] = v

            elif mode == "ratio":
                values, weights = [], []
                for sp in simpoints:
                    label = f"{benchmark}/{_strip_numeric_prefix(sp)}"
                    b = data.get(baseline, {}).get(label)
                    t = data.get(prefetcher, {}).get(label)
                    if b is None or t is None or b == 0:
                        continue
                    w = _effective_simpoint_weight(weight_map[sp])
                    if w is None:
                        continue
                    values.append(float(t) / float(b))
                    weights.append(w)
                results[prefetcher][benchmark] = (
                    _weighted_arithmetic_mean(values, weights) if values else 0.0
                )

            elif mode == "absolute":
                value_kind = metric_spec.get("value_kind")
                values, weights = [], []
                for sp in simpoints:
                    label = f"{benchmark}/{_strip_numeric_prefix(sp)}"
                    raw = data.get(prefetcher, {}).get(label)
                    if raw is None:
                        continue
                    converted = _convert_absolute(raw, value_kind)
                    if converted is None:
                        continue
                    w = _effective_simpoint_weight(weight_map[sp])
                    if w is None:
                        continue
                    values.append(converted)
                    weights.append(w)
                results[prefetcher][benchmark] = (
                    _weighted_arithmetic_mean(values, weights) if values else 0.0
                )
            else:
                raise ValueError(f"unknown aggregate mode: {mode}")

    for prefetcher in plot_prefetchers:
        per_bench = [results[prefetcher].get(bm, 0.0) for bm in benchmark_order]
        results[prefetcher]["geomean"] = _geomean(per_bench)

    return dict(results), plot_prefetchers, benchmark_order


# ---------------------------------------------------------------------------
# Printing
# ---------------------------------------------------------------------------

def _display_label(prefetcher: str, display_names: dict) -> str:
    return display_names.get(prefetcher, prefetcher)


def print_benchmark_stats(
    results,
    plot_prefetchers,
    benchmark_order,
    metric_name,
    display_names=None,
):
    """Print per-benchmark table plus geomean (full names, all benches)."""
    display_names = display_names or {}
    col_headers = [_display_label(p, display_names) for p in plot_prefetchers]
    row_labels = list(benchmark_order)

    label_width = max(len("Benchmark"), max(len(label) for label in row_labels))
    col_widths = [
        max(len(h), 6)  # at least wide enough for "0.0000"
        for h in col_headers
    ]

    def fmt_row(label: str, values: list[str]) -> str:
        cells = [f"{label:<{label_width}}"]
        for width, value in zip(col_widths, values):
            cells.append(f"{value:>{width}}")
        return "  ".join(cells)

    def sep_row() -> str:
        return fmt_row(
            "-" * label_width,
            ["-" * w for w in col_widths],
        )

    print()
    print("=" * 30)
    print(f"> {metric_name.upper()} per Benchmark:")
    print("=" * 30)
    print(fmt_row("Benchmark", col_headers))
    print(sep_row())

    for benchmark in benchmark_order:
        values = [
            f"{results.get(p, {}).get(benchmark, 0.0):.4f}"
            for p in plot_prefetchers
        ]
        print(fmt_row(benchmark, values))


def print_geomean_summary(
    geomeans_by_metric,
    plot_prefetchers,
    display_names=None,
):
    display_names = display_names or {}
    label_width = max(
        len("Prefetcher"),
        max(len(_display_label(p, display_names)) for p in plot_prefetchers),
    )

    print()
    print("=" * 30)
    print("> GEOMEAN Summary:")
    print("=" * 30)

    for metric in ("speedup", "dram", "coverage", "accuracy"):
        geomeans = geomeans_by_metric.get(metric, {})
        print()
        print(f"{metric.upper()}:")
        for prefetcher in plot_prefetchers:
            label = _display_label(prefetcher, display_names)
            value = geomeans.get(prefetcher, 0.0)
            print(f"  {label:<{label_width}}  {value:.4f}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

SUITE_NAME = "spec2017"
BASELINE = "no"


def validate_prefetcher(root: Path, name: str) -> None:
    pref_dir = root / "prefetcher" / name
    if not pref_dir.is_dir():
        available = sorted(
            p.name for p in (root / "prefetcher").iterdir() if p.is_dir()
        )
        print(
            f"error: unknown L1D prefetcher '{name}' "
            f"(expected a directory under prefetcher/)\n"
            f"available: {', '.join(available)}",
            file=sys.stderr,
        )
        sys.exit(1)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Print Speedup / DRAM / coverage / accuracy for one or more L1D "
        )
    )
    parser.add_argument(
        "prefetchers",
        nargs="+",
        metavar="PREFETCHER",
        help="L1D prefetcher module name(s) (e.g. caerus berti bop)",
    )
    parser.add_argument(
        "--results-dir",
        type=Path,
        default=None,
        help="results root (default: <repo>/results)",
    )
    args = parser.parse_args()

    root = repo_root()

    # Preserve order, drop duplicates
    l1_names = list(dict.fromkeys(args.prefetchers))
    for name in l1_names:
        validate_prefetcher(root, name)
        if name == "no":
            print(
                "error: baseline prefetcher 'no' has nothing to compare; "
                "omit it (baseline is always no)",
                file=sys.stderr,
            )
            return 1

    results_dir = str(
        (args.results_dir.expanduser().resolve() if args.results_dir
         else root / "results")
    )

    try:
        suite = load_suite(SUITE_NAME)
    except FileNotFoundError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    bench_map = suite["benchmarks"]

    prefetchers = l1_names + [BASELINE]

    print("=" * 60)
    print("Speedup, DRAM Traffic, Coverage, Accuracy Results")
    print("=" * 60)

    geomeans_by_metric = {}
    plot_prefetchers = None

    for metric in ("speedup", "dram", "coverage", "accuracy"):
        spec = METRICS[metric]
        data, _unparsed = gather_data(
            results_dir, SUITE_NAME, prefetchers, spec["parse"], bench_map,
        )

        try:
            results, plot_prefetchers, bench_order = aggregate(
                data, bench_map, prefetchers, BASELINE, spec,
                speedup_mean_type="harmonic",
            )
        except ValueError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1

        geomeans_by_metric[metric] = {
            p: results.get(p, {}).get("geomean", 0.0) for p in plot_prefetchers
        }

        print_benchmark_stats(
            results,
            plot_prefetchers,
            bench_order,
            metric,
        )

    print_geomean_summary(geomeans_by_metric, plot_prefetchers)

    return 0


if __name__ == "__main__":
    sys.exit(main())
