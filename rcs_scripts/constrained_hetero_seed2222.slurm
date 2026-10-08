#!/bin/bash
#SBATCH --job-name=pumpkin-hetero
#SBATCH --partition=large
#SBATCH --qos=normal
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=140
#SBATCH --mem=256G
#SBATCH --time=72:00:00
#SBATCH --output=pumpkin-hetero-%j.out

# Heterogeneous 8-core mixes.
# Four L2 prefetchers (ipop, alecto, ip_stride, pumpkin) and 35 mixes.
#
# QOS caps: small=16 CPUs, medium=72, large=142. This job needs the
# large partition. normal is an allowed QOS there; its wall limit is
# 3 days, which matches --time. One task owns all 140 cores; batch_run
# fans work out inside that allocation.
NCPUS=${SLURM_CPUS_PER_TASK}

set -euo pipefail

module load Python/3.12.3-GCCcore-13.3.0

source ~/venvs/molos/bin/activate

PUMPKIN_ROOT="$HOME/Projects/pumpkin"
MOLOS="$HOME/molos/main.py"
JOBSHEET="$TMPDIR/pumpkin_hetero.json"

# A unique batch avoids colliding with previous submitted jobs.
BATCH="constrained-pumpkin-hetero-2222-${SLURM_JOB_ID}"

cd "$PUMPKIN_ROOT"

cat > "$JOBSHEET" <<JSON
{
    "champsim_root": "$PUMPKIN_ROOT",
    "warmup_instructions": 10000000,
    "simulation_instructions": 100000000,
    "base_config": "params/8core_constrained.json",
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
    "result_dir_suffix": "8core_2222_constrained_newconfig",
    "benchmarks": [
        "hpca27_multicore"
    ],
    "multicore": {
        "enabled": true,
        "num_mixes": 35,
        "seed": 2222,
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
