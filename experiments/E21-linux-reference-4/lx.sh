#!/bin/bash
# Linux probe helper. Usage: lx.sh <shell command on the probe>   (or: lx.sh wait)
# The ssh command line (key, known_hosts, address) comes from risky_run.sh line 4 and is never printed.
cd /p/BC-250 || exit 1
eval "$(sed -n '4p' scratch/tmp/risky_run.sh | sed -E "${BC250_PROBE:+s/root@[0-9.]+/root@${BC250_PROBE}/}; s/-o BatchMode=yes/-o BatchMode=yes -o StrictHostKeyChecking=accept-new/")"
if [ "$1" = "wait" ]; then
    for i in $(seq 1 90); do
        if $BC250_SSH 'echo probe up: $(uname -r) $(cat /proc/cmdline | tr " " "\n" | grep bc250.mode)' 2>/dev/null; then exit 0; fi
        sleep 5
    done
    echo "probe did not answer in 450 s"; exit 1
fi
$BC250_SSH "$@"
