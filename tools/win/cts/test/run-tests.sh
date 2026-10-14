#!/usr/bin/env bash
# Offline checks of run-batch.ps1 with fake-deqp.exe standing in for deqp-vk.exe. No GPU, no lab, no network.
# It builds the fake into a scratch package root, drives each FAKE_MODE and checks the exit codes and cases.tsv,
# including the resume call after each failure mode, and the direct route's refusal of a release-flavour ICD for a
# run that sets RADV_EXPERIMENTAL or RADV_PERFTEST (BD-167). 14 checks; the exit code is the number of failures.
#
#   bash run-tests.sh [work dir]
#
# The work directory holds every temporary file and defaults to <BC250_ROOT>/scratch/cts/tmp (BC250_CTS_WORK
# overrides the <BC250_ROOT>/scratch/cts part). Nothing is written inside the repository.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
CTS=$(dirname "$HERE")
# BC250_ROOT is the workspace root; by default the parent directory of this repository
# (this script is tools/win/cts/test/run-tests.sh, so the repository root is four levels above it).
BC250_ROOT=${BC250_ROOT:-$(cd "$CTS/../../../.." && pwd)}
WORK=${1:-${BC250_CTS_WORK:-$BC250_ROOT/scratch/cts}/tmp}
ROOT=$WORK/fakeroot
PS=${BC250_PS51:-/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe}
win() { cygpath -w "$1" 2>/dev/null || echo "$1"; }

rm -rf "$ROOT"; mkdir -p "$ROOT/bin" "$ROOT/lists"
export TEMP=$(win "$WORK") TMP=$(win "$WORK")
cmd.exe //d //c "$(win "$HERE/build-fake.cmd")" "$(win "$WORK")" > "$WORK/fake-build.log" 2>&1 \
  || { cat "$WORK/fake-build.log"; exit 1; }
cp "$WORK/fake-deqp.exe" "$ROOT/bin/deqp-vk.exe"
cp "$CTS/run-batch.ps1" "$ROOT/"
printf 'a.case1\na.case2\na.case3\na.case4\n' > "$ROOT/lists/four.txt"
printf 'a.case1\na.missing2\na.case3\n' > "$ROOT/lists/miss.txt"
# The direct route copies the ICD and the forwarder next to each other; the fake loads neither. A release ICD
# holds no lab variable's name, a lab ICD holds both.
mkdir -p "$ROOT/direct"; printf 'MZ forwarder' > "$ROOT/direct/cts-direct.dll"
printf 'MZ release icd RADV_EXPERIMENTALS XRADV_PERFTEST' > "$WORK/icd-release.dll"
printf 'MZ lab icd\0RADV_EXPERIMENTAL\0RADV_PERFTEST\0' > "$WORK/icd-lab.dll"
fails=0
run() {  # run <runid> <list> <mode> <expected exit> [extra runner arguments]
  local id=$1 list=$2 mode=$3 want=$4; shift 4
  FAKE_MODE=$mode "$PS" -NoProfile -ExecutionPolicy Bypass -File "$(win "$ROOT/run-batch.ps1")" \
    -CaseList "$(win "$ROOT/lists/$list.txt")" -Results "$(win "$ROOT/results")" \
    -RunId "$id" -Route "${ROUTE:-registered}" -IcdModulePattern '(?i)kernel32' -ThermalCheckSeconds 0 -NoStopFlag "$@" > "$ROOT/out.txt"
  local rc=$?
  local tsv="$ROOT/results/$id/list-$list/cases.tsv"
  printf '%-10s %-9s exit %s (want %s) | %s\n' "$id" "$mode" "$rc" "$want" "$(tail -n +2 "$tsv" 2>/dev/null | cut -f1,2 | tr '\t\n' ': ' )"
  [ "$rc" -eq "$want" ] || { fails=$((fails+1)); cat "$ROOT/out.txt"; }
}
run normal four normal 0
run missing miss normal 1
run crash four crash 2
run crash four crash 1
run watchdog four watchdog 3
run watchdog four normal 1
run hang four hang 3 -BoundSeconds 20
run hang four normal 1
run devlost four devlost 3
run devlost four normal 1
ROUTE=direct run relsparse four normal 3 -IcdPath "$(win "$WORK/icd-release.dll")"
grep -q 'release-flavour ICD' "$ROOT/out.txt" || { fails=$((fails+1)); echo 'relsparse: no BD-167 reason'; cat "$ROOT/out.txt"; }
ROUTE=direct run relnone four normal 0 -IcdPath "$(win "$WORK/icd-release.dll")" -Experimental none
ROUTE=direct run relperf four normal 3 -IcdPath "$(win "$WORK/icd-release.dll")" -Experimental none -Perftest rtwave64
ROUTE=direct run labicd four normal 0 -IcdPath "$(win "$WORK/icd-lab.dll")" -Perftest rtwave64
echo "failures: $fails"
exit $fails
