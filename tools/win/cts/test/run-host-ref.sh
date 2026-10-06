#!/usr/bin/env bash
# Reference run of a case list on the development PC's own GPU with the same runner the lab uses, route
# "registered" (the system Vulkan loader and the installed ICD). It gives every case a known status and a
# known duration from a mature implementation, which is what the batch plan and the comparison in
# summarize.py need. It needs a deqp-vk package, so it is not an offline check.
#
#   run-host-ref.sh <package dir> <chunk dir> <results dir> [log file]
#
# The package directory holds bin\deqp-vk.exe and its data; the chunk directory holds the case lists, one per
# call (1000 cases each in the 2026-10-01 run). Each chunk is called again while the runner answers 2 (bound
# reached, resumable). Everything the processes write goes outside drive C:. RUNID names the run, EXPECT_DEVICE
# and ICD_PATTERN are the device and module witness of the GPU in this PC (defaults: the 2026-10-01 reference).
set -u
PKG=${1:-}
CHUNKS=${2:-}
RESULTS=${3:-}
[ -n "$RESULTS" ] || { sed -n '2,15p' "$0"; exit 2; }
RUNID=${RUNID:-host-ref}
EXPECT_DEVICE=${EXPECT_DEVICE:-RTX 4090}
ICD_PATTERN=${ICD_PATTERN:-(?i)nvoglv64}
HERE=$(cd "$(dirname "$0")" && pwd)
RUNNER=$(dirname "$HERE")/run-batch.ps1
PS=${BC250_PS51:-/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe}
win() { cygpath -w "$1" 2>/dev/null || echo "$1"; }
log=${4:-$RESULTS/run-$RUNID.log}
mkdir -p "$RESULTS"
WORKTMP=${BC250_CTS_TMP:-$RESULTS/tmp}
mkdir -p "$WORKTMP"
export TEMP=$(win "$WORKTMP") TMP=$(win "$WORKTMP")
# No driver shader disk cache: a cached pipeline would make the second run of a case faster than the first.
export __GL_SHADER_DISK_CACHE=0 __GL_SHADER_DISK_CACHE_PATH=$(win "$WORKTMP")
rc=0
for f in "$CHUNKS"/*.txt; do
  for attempt in 1 2 3 4 5 6 7 8 9 10; do
    "$PS" -NoProfile -ExecutionPolicy Bypass -File "$(win "$RUNNER")" \
      -CaseList "$(win "$f")" -Root "$(win "$PKG")" -Results "$(win "$RESULTS")" -RunId "$RUNID" \
      -Route registered -ExpectDevice "$EXPECT_DEVICE" -IcdModulePattern "$ICD_PATTERN" \
      -ThermalCheckSeconds 0 -NoStopFlag >> "$log" 2>&1
    rc=$?
    echo "$(date -u +%FT%TZ) $(basename "$f") attempt $attempt exit $rc" >> "$log"
    [ $rc -eq 2 ] || break
  done
  if [ $rc -eq 3 ]; then echo "stop on $(basename "$f")" >> "$log"; exit 3; fi
done
echo "$(date -u +%FT%TZ) done" >> "$log"
