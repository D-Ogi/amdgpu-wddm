#!/usr/bin/env bash
# One frame from the USB camera the owner attached to the development PC to watch the lab (2026-10-01): a one-shot
# ffmpeg DirectShow capture, no window, no resident process. The camera warms up for ~1 s (auto exposure), so the
# 30th frame is kept. Owner: crop the image hard ("Należy znacząco przyciąć obraz z kamery"): only unit A's monitor
# and case are kept (fractions of the frame, measured on the 2026-10-01 view; CROP overrides as
# "w:h:x:y" fractions if the camera moves). The camera also sees the owner's room, so an uncropped frame is never
# kept and never viewed.
# Frames stay under <BC250_ROOT>/scratch/labcam and never enter a repository. BC250_ROOT defaults to the parent
# directory of this repository; LABCAM_OUT overrides the output directory; CAMERA names another DirectShow device.
# Usage: snap.sh            CROP=0.23:0.39:0.395:0.61 snap.sh
crop="${CROP:-0.23:0.39:0.395:0.61}"
IFS=: read -r cw ch cx cy <<< "$crop"
here=$(cd "$(dirname "$0")" && pwd) || exit 1
root=${BC250_ROOT:-$(cd "$here/../../../.." && pwd)}
d=${LABCAM_OUT:-$root/scratch/labcam}
mkdir -p "$d" || exit 1
out="$d/cam-$(date -u +%Y%m%dT%H%M%SZ).jpg"
timeout 30 ffmpeg -hide_banner -loglevel error -f dshow -i "video=${CAMERA:-HD 1080P  PC-Camera}" \
  -vf "select=gte(n\,30),crop=iw*$cw:ih*$ch:iw*$cx:ih*$cy" -frames:v 1 -q:v 4 -y "$out" && echo "$out"
