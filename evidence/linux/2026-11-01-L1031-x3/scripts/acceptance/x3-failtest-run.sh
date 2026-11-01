#!/bin/sh
# Run the staged fault-control script as a file; "set" reboots only after it succeeded.
date -u
sh /root/x3/x3-failtest.sh "$1" || exit 1
if [ "$1" = set ]; then echo "reboot at $(date -u)"; reboot; fi
