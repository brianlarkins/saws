#!/bin/bash

cpn=128
acct="ccu104"
queue="compute"
total_mem="248G"

# load makefile function
source ./makegen.sh

# make directories
mkdir -p uts-scioto;
mkdir -p bpc;
mkdir -p mad3d;
mkdir -p scripts
cd scripts

# uts
mkdir -p uts
cd uts
. $HOME/saws/examples/uts/sample_trees.sh
xpath=$HOME/saws/examples/uts
for i in 1 2 3 4 8 12 16 20 24 28 32
do
  for tpn in 64
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
for i in 1 2 3 4 8 12 16 20 24 28 32
do
  for tpn in 64
  do
    makefile $i $tpn 5:00 "$xpath" "bpc" "-B" bpc_nobounce_base
    makefile $i $tpn 5:00 "$xpath" "bpc" "-H" bpc_nobounce_half
    makefile $i $tpn 5:00 "$xpath" "bpc" "-d 300 -n 16384 -b -B" "-d 300 -n 16384 -b -H" bpc
  done
done
cd ..

# MADNESS
mkdir -p madness
cd madness
xpath=$HOME/saws/examples/madness
for i in 1 2 3 4 8 12 16 20 24 28 32
do
  for tpn in 64
  do
    makefile $i $tpn 5:00 "$xpath" "mad3d" "-t 10e-9 -B" "-t 10e-9 -H" mad3d
  done
done
cd ..

# time-td
mkdir -p td
cd td
xpath=$HOME/saws/tests/microbenchmarks
for i in 1 2 3 4 8 12 16 20 24 28 32
do
  for tpn in 64 128
  do
    makefile $i $tpn 5:00 "$xpath" "time-td" "100" "100" td
  done
done
cd ..
