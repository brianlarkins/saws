#!/bin/bash

function makefile() {
  n=$1
  tpn=$2
  wtime=$3 # ignored
  xpath=$4
  exe=$5
  args_b=$6
  args_h=$7
  label=$8

  cputask=$(($cpn / $tpn))    # threads/proc = tasks/node / cores/node
  ntasks=$((n * $tpn))        # ntasks = nodes * tasks/node
  printf -v pntasks "%04d" $ntasks

  blockpin=0

  sfname="$label.$pntasks.$tpn.sh"

  runner="srun"

  if [[ $n -lt 4 ]]; then
    wtime="01:00:00"
  elif [[ $n -le 8 ]]; then
    wtime="10:00"
  else
    wtime="05:00"
  fi

  echo "nodes: $n total tasks: $ntasks tpn: $tpn cputasks: $cputask $uenv $exe $label $sfname"

  echo '#!/bin/bash' > $sfname
  #if  [ ! -z "acct" ]; then
  #  echo "#SBATCH --account=$acct" >> $sfname
  #fi
  #if  [ ! -z "queue" ]; then
  #  echo "#SBATCH -p $queue" >> $sfname
  #fi
  #echo "#SBATCH -t $wtime" >> $sfname
  echo "#SBATCH -n $ntasks" >> $sfname
  echo "#SBATCH -N $n" >> $sfname
  echo "#SBATCH --ntasks-per-node=$tpn" >> $sfname
  #if  [ ! -z "$total_mem" ]; then
  #  echo "#SBATCH --mem=$total_mem" >> $sfname
  #fi

  echo ' ' >> $sfname

  echo "for i in \$(seq -w 01 10); do" >> $sfname
  echo "  $runner --mpi=pmi2 -o $HOME/saws/runs/$exe/$exe.$pntasks.${label}_half.\$i $uenv $xpath/$exe $args_h" >> $sfname
  echo "  $runner --mpi=pmi2 -o $HOME/saws/runs/$exe/$exe.$pntasks.${label}_base.\$i $uenv $xpath/$exe $args_b" >> $sfname
  echo "done" >> $sfname
}
