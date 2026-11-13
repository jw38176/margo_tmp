#!/usr/bin/env python3
"""
Run SPECspeed2017 ChampSim experiments for a single L1D prefetcher.

use -j N to run N jobs in parallel.
"""

from __future__ import annotations

import argparse
import copy
import json
import os
import subprocess
import sys
import tempfile
import uuid
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

from download_spec2017_traces import DEFAULT_TRACES_DIR

WARMUP_INSTRUCTIONS = 50_000_000
SIMULATION_INSTRUCTIONS = 200_000_000
SUITE_NAME = "spec2017"
BASE_CONFIG_REL = Path("params") / "baseline.json"
SUITE_JSON_REL = Path("scripts") / "suites" / "spec2017.json"


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def deep_merge(base: dict, overrides: dict) -> dict:
    result = copy.deepcopy(base)
    for key, value in overrides.items():
        if key in result and isinstance(result[key], dict) and isinstance(value, dict):
            result[key] = deep_merge(result[key], value)
        else:
            result[key] = copy.deepcopy(value)
    return result


def load_json(path: Path) -> dict:
    with open(path) as fh:
        return json.load(fh)


def flatten_traces(suite: dict) -> list[tuple[str, str, str]]:
    """Return ``(category, trace_name, trace_ext)`` for every simpoint."""
    ext = suite["trace_ext"]
    traces = []
    for category, simpoints in suite["benchmarks"].items():
        for trace_name in simpoints:
            traces.append((category, trace_name, ext))
    return traces


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


def validate_traces(
    traces_dir: Path, jobs: list[tuple[str, str, str]]
) -> list[tuple[str, str, Path]]:
    if not traces_dir.is_dir():
        print(
            f"error: traces directory not found: {traces_dir}\n"
            f"place SPECspeed2017 .champsimtrace.xz files there, or pass --traces-dir",
            file=sys.stderr,
        )
        sys.exit(1)

    resolved = []
    missing = []
    for category, trace_name, ext in jobs:
        path = traces_dir / f"{trace_name}{ext}"
        if not path.is_file():
            missing.append(str(path))
        else:
            resolved.append((category, trace_name, path))

    if missing:
        print(
            f"error: {len(missing)} trace file(s) missing under {traces_dir}:",
            file=sys.stderr,
        )
        for path in missing[:10]:
            print(f"  {path}", file=sys.stderr)
        if len(missing) > 10:
            print(f"  ... and {len(missing) - 10} more", file=sys.stderr)
        sys.exit(1)

    return resolved


def build_champsim(root: Path, config: dict, jobs: int) -> Path:
    executable_name = f"champsim_{uuid.uuid4().hex[:8]}"
    config = copy.deepcopy(config)
    config["executable_name"] = executable_name

    fd, tmp_path = tempfile.mkstemp(suffix=".json", prefix="run_exp_cfg_")
    try:
        with os.fdopen(fd, "w") as fh:
            json.dump(config, fh, indent=4)

        print(f"Configuring with {tmp_path}...")
        r = subprocess.run(["./config.sh", tmp_path], cwd=root)
        if r.returncode != 0:
            print("error: configuration failed", file=sys.stderr)
            sys.exit(1)

        print(f"Building ({executable_name}) with make -j {jobs}...")
        r = subprocess.run(["make", "-j", str(jobs)], cwd=root)
        if r.returncode != 0:
            print("error: build failed", file=sys.stderr)
            sys.exit(1)
    finally:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass

    binary = root / "bin" / executable_name
    if not binary.is_file():
        print(f"error: binary not found at {binary}", file=sys.stderr)
        sys.exit(1)
    return binary


def run_one(
    binary: Path,
    trace_path: Path,
    result_path: Path,
    warmup: int,
    sim_instr: int,
) -> tuple[bool, str]:
    result_path.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(binary),
        "--warmup_instructions",
        str(warmup),
        "--simulation_instructions",
        str(sim_instr),
        str(trace_path),
    ]
    try:
        with open(result_path, "w") as out:
            proc = subprocess.run(
                cmd,
                stdout=out,
                stderr=subprocess.STDOUT,
                check=False,
            )
        if proc.returncode != 0:
            return False, f"exit {proc.returncode}"
        return True, "ok"
    except OSError as exc:
        return False, str(exc)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Build ChampSim for one L1D prefetcher and run all SPECspeed2017 "
            "traces locally."
        )
    )
    parser.add_argument(
        "prefetcher",
        help="L1D prefetcher module name (e.g. caerus, berti, no)",
    )
    parser.add_argument(
        "-j",
        type=int,
        default=1,
        metavar="N",
        help="parallel simulations (default: 1); also used for make -j",
    )
    parser.add_argument(
        "--traces-dir",
        type=Path,
        default=DEFAULT_TRACES_DIR,
        help=f"directory containing SPECspeed2017 traces (default: {DEFAULT_TRACES_DIR})",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="rerun even if the result .txt already exists",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.j < 1:
        print("error: -j must be >= 1", file=sys.stderr)
        return 1

    root = repo_root()
    validate_prefetcher(root, args.prefetcher)

    base_path = root / BASE_CONFIG_REL
    suite_path = root / SUITE_JSON_REL
    if not base_path.is_file():
        print(f"error: base config not found: {base_path}", file=sys.stderr)
        return 1
    if not suite_path.is_file():
        print(f"error: suite definition not found: {suite_path}", file=sys.stderr)
        return 1

    base = load_json(base_path)
    variant = {
        "L1D": {"prefetcher": args.prefetcher}
    }
    config = deep_merge(base, variant)
    config_name = args.prefetcher

    suite = load_json(suite_path)
    flat = flatten_traces(suite)
    traces = validate_traces(args.traces_dir.expanduser().resolve(), flat)

    results_root = root / "results" / SUITE_NAME / config_name
    pending: list[tuple[str, str, Path, Path]] = []
    skipped = 0
    for category, trace_name, trace_path in traces:
        result_path = results_root / category / f"{trace_name}.txt"
        if result_path.is_file() and not args.force:
            skipped += 1
            continue
        pending.append((category, trace_name, trace_path, result_path))

    print(f"L1D prefetcher : {args.prefetcher}")
    print(f"Config name    : {config_name}")
    print(f"Traces dir     : {args.traces_dir.expanduser()}")
    print(f"Results dir    : {results_root}")
    print(f"Warmup / sim   : {WARMUP_INSTRUCTIONS} / {SIMULATION_INSTRUCTIONS}")
    print(f"Parallelism    : {args.j}")
    print(f"Jobs           : {len(pending)} to run, {skipped} skipped (existing)")
    print()

    if not pending:
        print("Nothing to do.")
        return 0

    # Always build so the binary matches the requested prefetcher, even when
    # some results already exist (resume after a partial run).
    binary = build_champsim(root, config, args.j)
    print(f"Binary         : {binary}\n")

    total = len(pending)
    succeeded = 0
    failed = 0
    done = 0

    def _label(category: str, trace_name: str) -> str:
        return f"{category}/{trace_name}"

    with ThreadPoolExecutor(max_workers=args.j) as pool:
        futures = {
            pool.submit(
                run_one,
                binary,
                trace_path,
                result_path,
                WARMUP_INSTRUCTIONS,
                SIMULATION_INSTRUCTIONS,
            ): (category, trace_name)
            for category, trace_name, trace_path, result_path in pending
        }
        for fut in as_completed(futures):
            category, trace_name = futures[fut]
            done += 1
            ok, detail = fut.result()
            if ok:
                succeeded += 1
                mark = "ok"
            else:
                failed += 1
                mark = f"FAIL ({detail})"
            print(f"  [{done}/{total}] {_label(category, trace_name)} → {mark}")

    print()
    print(f"Done: {succeeded} succeeded, {failed} failed, {skipped} skipped")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
