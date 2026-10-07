#!/bin/bash
#PBS -N pumpkin-hetero
#PBS -l select=1:ncpus=128:mem=256gb
#PBS -l walltime=72:00:00
#PBS -j oe

# Heterogeneous 8-core mixes.
# Four L2 prefetchers (ipop, alecto, ip_stride, pumpkin) and 50 mixes.
#
# Keep this equal to ncpus in the select line above. PBS_NODEFILE has one
# line per chunk, not per CPU, so the worker pool cannot be read from it.
NCPUS=128

set -euo pipefail

module load Python/3.12.3-GCCcore-13.3.0

source ~/venvs/molos/bin/activate

PUMPKIN_ROOT="$HOME/Projects/pumpkin"
MOLOS="$HOME/molos/main.py"
JOBSHEET="$TMPDIR/pumpkin_hetero.json"

# A unique batch avoids colliding with previous submitted jobs.
BATCH="pumpkin-hetero-${PBS_JOBID%%.*}"

cd "$PUMPKIN_ROOT"

cat > "$JOBSHEET" <<JSON
{
    "champsim_root": "$PUMPKIN_ROOT",
    "warmup_instructions": 10000000,
    "simulation_instructions": 100000000,
    "base_config": "params/8core_baseline.json",
    "variants": [
        {
            "L1D": {
                "prefetcher": "no"
            },
            "L2C": {
                "prefetcher": "ipop"
            }
        },
        {
            "L1D": {
                "prefetcher": "no"
            },
            "L2C": {
                "prefetcher": "alecto"
            }
        },
        {
            "L1D": {
                "prefetcher": "no"
            },
            "L2C": {
                "prefetcher": "ip_stride"
            }
        },
        {
            "L1D": {
                "prefetcher": "no"
            },
            "L2C": {
                "prefetcher": "pumpkin"
            }
        }
    ],
    "result_dir_suffix": "8core_dram3200_2025_newconfig",
    "benchmarks": [
        "hpca27_multicore"
    ],
    "multicore": {
        "enabled": true,
        "num_mixes": 30,
        "seed": 2025,
        "homogeneous": false
    }
}
JSON

python "$MOLOS" batch_compile "$JOBSHEET" "$BATCH"

echo "Compiled batch: $BATCH"
python "$MOLOS" batch_list "$BATCH"

echo "Workers: ${NCPUS}"

python "$MOLOS" batch_run "$BATCH" \
    --local \
    --workers "$NCPUS"

echo "Completed batch: $BATCH"
