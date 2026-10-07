#!/bin/sh
# L1007b setup: the stick's local repository has no alsa-utils; add the online Alpine v3.24 repositories (as L1007).
grep -q dl-cdn /etc/apk/repositories || printf '%s\n' https://dl-cdn.alpinelinux.org/alpine/v3.24/main https://dl-cdn.alpinelinux.org/alpine/v3.24/community >> /etc/apk/repositories
apk update 2>&1 | tail -2
apk add alsa-utils python3 2>&1 | tail -2
apk info -v 2>/dev/null | grep -E '^(alsa-utils|python3)'
lsmod | grep -q '^amdgpu' || { modprobe amdgpu; sleep 8; }
lsmod | grep -E '^(amdgpu|snd_hda_intel)'
aplay -l
