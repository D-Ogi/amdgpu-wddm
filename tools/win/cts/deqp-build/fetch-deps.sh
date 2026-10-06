#!/usr/bin/env bash
# Fetch the external sources of VK-GL-CTS vulkan-cts-1.4.6.2 into <source tree>/external.
#
#   fetch-deps.sh <source tree> [seed dir]
#
# Upstream's own external/fetch_sources.py checks out the pinned revisions and downloads the archive packages
# (zlib, libpng, the renderdoc header) with its checksum verification. A seed directory makes the clones local:
# it holds an earlier fetch of the same release (<seed>/<name>/src is a git tree of the same commits), so the
# git objects come from the disk and only the archives come from the network. Each origin is then reset to the
# upstream URL below. Without a seed every dependency is cloned from upstream. The GitHub shallow clone stalled
# on 2026-10-01, which is why the seed exists.
# Python comes from BC250_PYTHON when the environment sets it (the pinned build used Python 3.14.0).
set -u
SRC=${1:-}
SEED=${2:-}
[ -n "$SRC" ] || { sed -n '2,12p' "$0"; exit 2; }
[ -d "$SRC/external" ] || { echo "no $SRC/external"; exit 2; }
PY=${BC250_PYTHON:-/c/Python314/python.exe}
WORKTMP=${BC250_CTS_TMP:-$SRC/../tmp}
mkdir -p "$WORKTMP"
WORKTMP_WIN=$(cygpath -w "$WORKTMP" 2>/dev/null || echo "$WORKTMP")
export TEMP="$WORKTMP_WIN" TMP="$WORKTMP_WIN"
export PYTHONPYCACHEPREFIX="$WORKTMP_WIN\\pycache" PYTHONDONTWRITEBYTECODE=1
declare -A URL=(
  [spirv-tools]=https://github.com/KhronosGroup/SPIRV-Tools.git
  [glslang]=https://github.com/KhronosGroup/glslang.git
  [spirv-headers]=https://github.com/KhronosGroup/SPIRV-Headers.git
  [vulkan-docs]=https://github.com/KhronosGroup/Vulkan-Docs.git
  [amber]=https://github.com/google/amber.git
  [jsoncpp]=https://github.com/open-source-parsers/jsoncpp.git
  [vulkan-video-samples]=https://github.com/KhronosGroup/Vulkan-Video-Samples.git
  [video_generator]=https://github.com/Igalia/video_generator.git
)
for d in "${!URL[@]}"; do
  dst="$SRC/external/$d/src"
  if [ -d "$dst/.git" ]; then echo "seed $d: present"; continue; fi
  echo "seed $d"
  if [ -n "$SEED" ] && [ -d "$SEED/$d/src/.git" ]; then
    git clone --no-checkout "$SEED/$d/src" "$dst" || exit 1
  else
    git clone --no-checkout "${URL[$d]}" "$dst" || exit 1
  fi
  git -C "$dst" remote set-url origin "${URL[$d]}" || exit 1
done
cd "$SRC/external" || exit 1
"$PY" fetch_sources.py --verbose
rc=$?
echo "fetch_sources exit $rc"
# The pinned revisions and a clean tree are the record of this fetch; the README lists what they must be.
for d in "${!URL[@]}"; do
  printf '%s %s %s\n' "$d" "$(git -C "$SRC/external/$d/src" rev-parse HEAD)" "$(git -C "$SRC/external/$d/src" status --porcelain | wc -l)"
done
exit $rc
