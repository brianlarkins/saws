#!/bin/bash

bench=$1
tpn=$2

if [[ ! -d "$bench"  ]]; then
  echo "can't find directory $bench"
  echo "leaving now\n"
  exit 0
fi

bind="unknown"

cd $bench
outfile="../data/$bench.dat"
rm $outfile
#echo "# $bench benchmark $tpn" > $outfile
#echo "# baseline first" > $outfile
#echo "# steal-half second" > $outfile

for i in *base
do
  if [ "$bench" = "uts-scioto" ]; then
    tail -n 4 $i | head -n 1 >> $outfile
  else
    tail -n 1 $i >> $outfile
    echo "" >> $outfile
  fi
done

echo "" >> $outfile
echo "" >> $outfile

for i in *half
do
  if [ "$bench" = "uts-scioto" ]; then
    tail -n 4 $i | head -n 1 >> $outfile
  else
    tail -n 1 $i >> $outfile
    echo "" >> $outfile
  fi
done

cd -
