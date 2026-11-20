#!/bin/bash
# runs/e0/build.sh -- build libtc, the benchmarks, and the tests against one SOS variant.
#
#   build.sh <xpmem|main> [source tree]
#
# Copies the source tree (default $HOME/laws/src, including .git so the build
# records its revision) to $HOME/laws/build-<variant> and builds there, so the
# two variants never share objects or libtc.a.
set -euo pipefail

variant=${1:?usage: build.sh <xpmem|main> [source tree]}
here=$(cd "$(dirname "$0")" && pwd)
set +u   # lmod and module files reference unset variables
source "$here/env.sh" "$variant"
set -u
src=${2:-$E0_ROOT/src}

mkdir -p "$E0_BUILD"
rsync -a --delete --exclude 'runs/e0/results/' "$src/" "$E0_BUILD/"
cd "$E0_BUILD"

log=$E0_BUILD/build.log
{
  echo "# build $(date -Is) on $(hostname)"
  echo "# variant $variant, oshcc $(command -v oshcc)"
  module -t list 2>&1 | sed 's/^/# module /'
  git describe --always --dirty --abbrev=10 2>/dev/null | sed 's/^/# rev /' || true

  make -C libtc clean
  make -C examples/uts clean
  make -C examples/bpc clean
  make -C examples/madness clean
  make -C examples/uts uts-scioto
  make -C examples/bpc bpc
  make -C examples/madness mad3d
  make -C tests test-tasktree test-tasktree-twotc test-locality test-laws test-laws-shrb bench-claim
} > "$log" 2>&1 || { echo "build failed; see $log" >&2; tail -30 "$log" >&2; exit 1; }

echo "built $variant in $E0_BUILD ($(cat libtc/buildinfo.h | tr '\n' ' '))"
