#!/bin/sh
# SoC temperature of the BC-250 under Windows, read from the development PC:  temp.sh [count [interval_s]]
# Needs bc250rd.sys loaded on the target (the service is set to start automatically).
ROOT=${BC250_ROOT:-/p/BC-250}
HOST=${BC250_WIN_HOST:-bc250@192.168.69.41}
exec ssh -i "$ROOT/secrets/client/bc250diag_ed25519" -o BatchMode=yes -o IdentitiesOnly=yes -o ConnectTimeout=10 \
    -o UserKnownHostsFile="$ROOT/secrets/client/known_hosts_win" "$HOST" \
    "C:\BC250\bc250rd\bc250rd_cli.exe temp ${1:-1} ${2:-1}"
