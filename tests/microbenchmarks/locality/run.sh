#!/bin/bash
#SBATCH -N 2
#SBATCH --ntasks-per-node=48
#SBATCH -t 00:20:00
#SBATCH -o locbench.%j.out
module purge >/dev/null 2>&1
module load gnu14 hwloc pmix ucx >/dev/null 2>&1
cd $HOME/laws-bench
for v in module cma xpmem; do
  echo "######## SOS=$v"
  srun --mpi=pmix --cpu-bind=cores ./locbench.$v
done
echo "######## SOS=xpmem UCX_ATOMIC_MODE=cpu"
UCX_ATOMIC_MODE=cpu srun --mpi=pmix --cpu-bind=cores ./locbench.xpmem
