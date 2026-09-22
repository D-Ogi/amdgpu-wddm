#!/bin/bash
# Pull /tmp/lx/out from the probe into scratch/lx/pull-<stamp>/ (tar through ssh).
cd /p/BC-250 || exit 1
eval "$(sed -n '4p' scratch/tmp/risky_run.sh | sed -E "${BC250_PROBE:+s/root@[0-9.]+/root@${BC250_PROBE}/}; s/-o BatchMode=yes/-o BatchMode=yes -o StrictHostKeyChecking=accept-new/")"
D=/p/BC-250/scratch/lx/pull-$(date +%H%M%S)
mkdir -p "$D"
$BC250_SSH "tar -C /tmp/lx -cf - out ${1:-}" | tar -C "$D" -xf -
echo "$D: $(find "$D" -type f | wc -l) files"
