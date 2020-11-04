#!/bin/bash

cpn=24   #cores per node

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
    wtime="02:00:00"
  elif [[ $n -le 8 ]]; then
    wtime="10:00"
  else
    wtime="05:00"
  fi

  echo "nodes: $n total tasks: $ntasks tpn: $tpn cputasks: $cputask $uenv $exe $label $sfname"

  echo '#!/bin/bash' > $sfname
  echo '#SBATCH -A ccu104' >> $sfname
  echo '#SBATCH -p compute' >> $sfname
  echo "#SBATCH -t $wtime" >> $sfname
  echo "#SBATCH -n $ntasks" >> $sfname
  echo "#SBATCH -N $n" >> $sfname
  echo "#SBATCH --ntasks-per-node=$tpn" >> $sfname
  echo "#SBATCH -c $cputask" >> $sfname
  echo "#SBATCH --exclusive" >> $sfname
  if [[ $blockpin -eq 1 ]]; then
    echo "#SBATCH -m block:block" >> $sfname
  fi 
  echo ' ' >> $sfname

  echo "$runner -o $HOME/saws/runs/$exe/$exe.$pntasks.${label}_base $uenv $xpath/$exe $args_b" >> $sfname
  echo "$runner -o $HOME/saws/runs/$exe/$exe.$pntasks.${label}_half $uenv $xpath/$exe $args_h" >> $sfname
}


mkdir -p scripts
cd scripts

# uts
mkdir -p uts
cd uts
. $HOME/saws/examples/iterators/uts/sample_trees.sh
xpath=$HOME/saws/examples/iterators/uts
for i in 1 2 4 8 16 24 32 48 64 72
do
  for tpn in 24
  do
    makefile $i $tpn 5:00 "$xpath" "uts-scioto" "$T1XL -Q B" "$T1XL -Q H" uts_t1xl
    makefile $i $tpn 5:00 "$xpath" "uts-scioto" "$T1XXL -Q B" "$T1XXL -Q H" uts_t1xxl
    makefile $i $tpn 5:00 "$xpath" "uts-scioto" "$T1WL -Q B" "$T1WL -Q H" uts_t1w
  done
done
cd ..

# BPC
mkdir -p bpc
cd bpc
xpath=$HOME/saws/examples/bpc
for i in 1 2 4 8 16 24 32 48 64 72
do
  for tpn in 24
  do
    makefile $i $tpn 5:00 "$xpath" "bpc" "-B" bpc_nobounce_base
    makefile $i $tpn 5:00 "$xpath" "bpc" "-H" bpc_nobounce_half
    makefile $i $tpn 5:00 "$xpath" "bpc" "-d 300 -n 8192 -b -B" "-d 300 -n 8192 -b -H" bpc
  done
done
cd ..

# MADNESS
mkdir -p madness
cd madness
xpath=$HOME/saws/examples/madness
for i in 1 2 4 8 16 24 32 48 64 72
do
  for tpn in 24
  do
    makefile $i $tpn 5:00 "$xpath" "mad3d" "-t 10e-9 -B" "-t 10e-9 -H" mad3d
  done
done
cd ..

# time-td
mkdir -p td
cd td
xpath=$HOME/saws/tests/microbenchmarks
for i in 1 2 4 8 16 24 32 48 64 72
do
  for tpn in 24
  do
    makefile $i $tpn 5:00 "$xpath" "time-td" "100" "100" td
  done
done
cd ..
