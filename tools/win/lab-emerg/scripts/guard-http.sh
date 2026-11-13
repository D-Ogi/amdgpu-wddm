#!/usr/bin/env bash
# Game-session temperature guard over the emergency channel (HTTP, no held SSH session; BD-051): reads the KMD dpm
# line every 30 s for SECONDS and ends the game by the stop rule of the lab levels (tools/win/lab-thermal, BD-376):
# at the first reading at or above STOP_AT_ONCE_C, or when the readings stay at or above STOP_HELD_C for
# STOP_HELD_S. With readings 30 s apart, the held rule acts at the second hot reading in a row. With RESET_FLOOR=1 it
# also runs `dpm tune reset` at the end (sessions that set a floor). Usage: guard-http.sh SECONDS [OUT]
here=$(cd "$(dirname "$0")" && pwd) || exit 1
emerg="$here/../lab-emerg.py"
levels=$(python "$here/../../lab-thermal/thermal_levels.py" --shell) || { echo "guard-http: no thermal levels" >&2; exit 1; }
eval "$levels"
secs="$1"; out="${2:-/dev/stdout}"; hot_since=''; end=$(( $(date +%s) + secs ))
# The client, resolved on the lab at every call: the release's (tester.10 on: Release key InstallRoot\tools), else the
# kmd185 copy, so the guard never loses its reading over a missing key.
cli='$c=$null; try { $c=Join-Path ([string](Get-ItemProperty -LiteralPath "HKLM:\SOFTWARE\amdgpu-wddm\Release" -Name InstallRoot -ErrorAction Stop).InstallRoot) "tools\bc250kmd_cli.exe" } catch {}; if (!$c -or !(Test-Path -LiteralPath $c)) { $c="C:\BC250\kmd185\bc250kmd_cli.exe" }; & $c'
read_dpm() {
  timeout 40 python "$emerg" ps -c "$cli dpm 1 1000 2>&1 | ForEach-Object { [string]\$_ }" 30 2>&1 \
    | grep -o '[0-9]* MHz  *[0-9]* mV.*C busy *[0-9.]*%' | head -1
}
# Game runner: GAME_PROCESSES (comma-separated Get-Process names of the session's game, set by run-game.sh) are
# ended through the channel's ps action; without it the channel's kill-game (witcher3), as before.
kill_game() {
  if [ -n "${GAME_PROCESSES:-}" ]; then
    names="'$(echo "$GAME_PROCESSES" | sed "s/,/','/g")'"
    timeout 40 python "$emerg" ps -c "Get-Process -Name $names -ErrorAction SilentlyContinue | ForEach-Object { 'ending ' + \$_.ProcessName + ' ' + \$_.Id; Stop-Process -Id \$_.Id -Force }" 30
  else
    timeout 40 python "$emerg" kill-game
  fi
}
{
  echo "guard-http $(date -u +%H:%M:%S) for $secs s"
  # spawn-loop-ok: one emergency-channel request every 30 s from this PC, no ssh session on the lab
  while [ "$(date +%s)" -lt "$end" ]; do
    line=$(read_dpm); t=$(echo "$line" | grep -o '[0-9.]* C busy' | grep -o '^[0-9.]*')
    echo "$(date -u +%H:%M:%S) $line"
    now=$(date +%s)
    # An empty reading neither starts nor ends a hot run.
    if [ -n "$t" ]; then
      if awk "BEGIN{exit !($t >= $STOP_AT_ONCE_C)}"; then
        echo "STOP Tctl $t C at or above $STOP_AT_ONCE_C C"; kill_game; break
      fi
      if awk "BEGIN{exit !($t >= $STOP_HELD_C)}"; then hot_since=${hot_since:-$now}; else hot_since=''; fi
    fi
    if [ -n "$hot_since" ] && [ $(( now - hot_since )) -ge "${STOP_HELD_S%.*}" ]; then
      echo "STOP Tctl at or above $STOP_HELD_C C, held $(( now - hot_since )) s"; kill_game; break
    fi
    sleep 30
  done
  if [ "${RESET_FLOOR:-0}" = 1 ]; then
    timeout 40 python "$emerg" ps -c "$cli dpm tune reset 2>&1 | ForEach-Object { [string]\$_ }" 30 2>&1 | grep -E 'floor' | head -1
  fi
  echo "end $(date -u +%H:%M:%S)"
} >> "$out" 2>&1
