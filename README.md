# Margo Artifact (HPCA 2027)

Artifact for **Margo: Prefetcher Selection via Marginal Utility**. Evaluates L2 prefetcher organisation mechanisms on SPECspeed 2026 using [ChampSim](https://github.com/ChampSim/ChampSim).

This artifact contains the source code and simulation framework used to evaluate Margo. The repository contains:
- Source code for Margo. (`prefetcher/margo`)
- Source code for other prefetcher organisation mechanisms(Alecto, IPOP, PPF). (`prefetcher/`)
- Source code for managed sub-prefetchers prefetchers (Stride, Next-Line, MLOP, Berti, Bingo, PMP, Pythia, Triangel). (`prefetcher/`)
- Modified champsim framework used to evaluate all presented prefetchers.
- Scripts to download SPECspeed 2026 traces, run simulations, and collect results. (`scripts/`)

## Requirements

Installing and running this artifact requires the following software dependencies:

- Python 3.9+
- g++ with C++17 support
- make
- git

The artifact is tested with the following system specs:

- Fedora 44
- Linux Kernel 7.3.3
- GCC 16.2.1
- Python 3.14.10

## Setup

To download the SPECspeed 2017 traces:

```bash
# Download SPECspeed 2017 traces (default: traces/spec2017/)
python3 scripts/download_spec2017_traces.py
```

## Run Baseline and Margo

The libraries ChampSim depends on (fmt, CLI11, nlohmann-json, zlib, bzip2, and liblzma) are included in this repository under `third_party/` and are compiled automatically the first time `make` is run (this is invoked by `scripts/run_experiments.py`). 

In the commands below, change the parameter `N` to run `N` experiments in parallel.

```bash
# Run baseline (no prefetching) and Margo
python3 scripts/run_experiments.py no -j N
python3 scripts/run_experiments.py margo -j N
```

This takes approximately 2 hours on a Xeon Platinum 8168 machine using 40 cores (`N`=40). On a machine with less cores, use a smaller `N` (up to the number of cores on your CPU) but expect a longer run time.

Results are written to `results/spec2017/<prefetcher>/`.

Then use the `scripts/print_results.py` script to print the results table:

```bash
# Print results (Margo vs baseline)
python3 scripts/print_results.py margo
```

The expected output is shown in [Expected Results](#expected-results).


## [OPTIONAL] Run Baseline and All Prefetchers

In the commands below, change the parameter `N` to run `N` experiments in parallel.

To reproduce all prefetchers evaluated in the paper:

```bash
# [OPTIONAL] Run baseline and all prefetchers
for pref in no ip_stride mlop berti bingo pmp pythia triangel next_line caerus alecto ipop ppf margo; do
    python3 scripts/run_experiments.py "$pref" -j N
done
```

This takes approximately 1 hour per prefetcher on a Xeon Platinum 8168 machine using 40 cores (`N`=40). On a machine with less cores, use a smaller `N` (up to the number of cores on your CPU) but expect a longer run time.

Then print result tables for all prefetchers:

```bash
# Print results (all prefetchers vs baseline)
python3 scripts/print_results.py ip_stride mlop berti bingo pmp pythia triangel next_line caerus alecto ipop ppf margo
```

The expected output is shown in [Expected Results](#expected-results).

**Prefetcher organisation mechanisms included in this artifact:**

| Name | Description |
|------|-------------|
| `margo` | Margo: Prefetcher Selection via Marginal Utility [HPCA'27]  |
| `alecto` | Integrating Prefetcher Selection with Dynamic Request Allocation Improves Prefetching Efficiency [HPCA'25] |
| `ipop` | I-POP: Ignite Positive Prefetchers [HPCA'26] |
| `ppf` | Perceptron-Based Prefetch Filtering [ISCA'19] |

Alecto and I-POP's implementation was provided by the I-POP authors and adapted to this version of ChampSim. PPF's filtering mechanism is extracted from SPP-PPF and generalised to filter prefetch requests of any sub-prefetcher.

**Sub-Prefetchers included in this artifact:**

| Name | Description |
|------|-------------|
| `no` | No prefetching (baseline) |
| `ip_stride` | Stride prefetcher |
| `next_line` | Next-line prefetcher |
| `mlop` | Multi-Lookahead Offset Prefetching [DPC-3'19] |
| `berti` | Berti: an Accurate Local-Delta Data Prefetcher [MICRO'22] |
| `bingo` | Bingo Spatial Data Prefetcher [HPCA'19] |
| `pmp` | Merging Similar Patterns for Hardware Prefetching [MICRO'22] |
| `pythia` | Pythia: A Customizable Hardware Prefetching Framework Using Online Reinforcement Learning [MICRO'21] |
| `triangel` | Triangel: A High-Performance, Accurate, Timely On-Chip Temporal Prefetcher [ISCA'24] |

The Stride, Next-Line, MLOP, Berti, Bingo, PMP, and Pythia prefetchers use open-source implementations. Triangel is adapted from its gem5 implementation. PPF's filtering mechanism is extracted from SPP-PPF and generalised to filter prefetch requests of any sub-prefetcher.

## Expected Results

The `scripts/print_results.py` script produces a table containing the per-benchmark and geomean results for Speedup, DRAM Traffic, Accuracy, and Coverage.

Using the geomean results as an example:

After running the baseline and Margo, the results should look like (TODO: update with measured values after running the experiments):

```bash
==============================
> GEOMEAN Summary:
==============================

SPEEDUP:
  margo       X.XXXX

DRAM:
  margo       X.XXXX

COVERAGE:
  margo       X.XXXX

ACCURACY:
  margo       X.XXXX

```

[OPTIONAL] After running the baseline and all prefetchers, the results should look like (TODO: update with measured values after running the experiments):

```bash
==============================
> GEOMEAN Summary:
==============================

SPEEDUP:
  ip_stride   X.XXXX
  mlop        X.XXXX
  berti       X.XXXX
  bingo       X.XXXX
  pmp         X.XXXX
  pythia      X.XXXX
  triangel    X.XXXX
  next_line   X.XXXX
  caerus      X.XXXX
  alecto      X.XXXX
  ipop        X.XXXX
  ppf         X.XXXX
  margo       X.XXXX

DRAM:
  ip_stride   X.XXXX
  mlop        X.XXXX
  berti       X.XXXX
  bingo       X.XXXX
  pmp         X.XXXX
  pythia      X.XXXX
  triangel    X.XXXX
  next_line   X.XXXX
  caerus      X.XXXX
  alecto      X.XXXX
  ipop        X.XXXX
  ppf         X.XXXX
  margo       X.XXXX

COVERAGE:
  ip_stride   X.XXXX
  mlop        X.XXXX
  berti       X.XXXX
  bingo       X.XXXX
  pmp         X.XXXX
  pythia      X.XXXX
  triangel    X.XXXX
  next_line   X.XXXX
  caerus      X.XXXX
  alecto      X.XXXX
  ipop        X.XXXX
  ppf         X.XXXX
  margo       X.XXXX

ACCURACY:
  ip_stride   X.XXXX
  mlop        X.XXXX
  berti       X.XXXX
  bingo       X.XXXX
  pmp         X.XXXX
  pythia      X.XXXX
  triangel    X.XXXX
  next_line   X.XXXX
  caerus      X.XXXX
  alecto      X.XXXX
  ipop        X.XXXX
  ppf         X.XXXX
  margo       X.XXXX
```

These results can be used to reproduce the Speedup, DRAM Traffic, Coverage, and Accuracy figures in the paper (TODO: add figure numbers).

## License

This artifact is licensed under the [Apache License, Version 2.0](LICENSE).

- **Margo prefetcher**: Copyright 2026 The Margo Authors
- **ChampSim simulator**: Copyright 2023 The ChampSim Contributors
