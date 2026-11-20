# runs/e0/env.sh -- load the toolchain for one SOS variant on lotus.
#   source env.sh <variant>
#     xpmem : openshmem/SOS-xpmem (system default since 2026-11-18; UCX + XPMEM)
#     main  : openshmem/SOS-main  (UCX only; the pre-XPMEM behavior)

e0_variant=${1:?usage: source env.sh <xpmem|main>}

# /etc/profile.d/lmod.sh is a no-op inside Slurm jobs (it expects the submitting
# shell's environment), and jobs submitted from zsh carry no bash module setup.
# Initialize Lmod directly so this works from any shell, login node or job.
if ! type module >/dev/null 2>&1; then
  : "${MODULEPATH:=/opt/tools/modules/modulefiles:/opt/ohpc/pub/modulefiles:/opt/tools/eb/modules/all}"
  export MODULEPATH
  source /opt/ohpc/admin/lmod/lmod/init/bash >/dev/null
fi

module purge >/dev/null 2>&1
module load gnu14 hwloc pmix ucx >/dev/null 2>&1
case $e0_variant in
  xpmem) module load openshmem/SOS-xpmem >/dev/null 2>&1 ;;
  main)  module load openshmem/SOS-main  >/dev/null 2>&1 ;;
  *)     echo "env.sh: unknown variant '$e0_variant'" >&2; return 1 ;;
esac

if ! module -t list 2>&1 | grep -q "openshmem/SOS-$e0_variant"; then
  echo "env.sh: failed to load openshmem/SOS-$e0_variant" >&2
  return 1
fi

export E0_ROOT=${E0_ROOT:-$HOME/laws}
export E0_BUILD=$E0_ROOT/build-$e0_variant${E0_BUILD_SUFFIX:-}   # suffix: a side build (e.g. -e3) while jobs use the main one
