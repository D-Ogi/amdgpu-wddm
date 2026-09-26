set -eu
r=/mnt/bc250-build/root/work/parity002
[ "$(cat "$r/full007/exit.txt")" = 3 ]
[ "$(cat "$r/posttimeout-swap.exit")" = 134 ]
pid=$(cat "$r/xorg-radeonsi.pid")
[ "$(cat /proc/$pid/comm)" = Xorg ]
kill -TERM "$pid"
for i in 1 2 3 4 5; do [ ! -d /proc/$pid ] && break; sleep 1; done
if [ -d /proc/$pid ]; then [ "$(awk '{print $3}' /proc/$pid/stat)" = Z ]; fi
[ ! -e "$r/xorg-recovery001.pid" ]
nohup chroot /mnt/bc250-build/root /usr/bin/env LD_LIBRARY_PATH=/opt/bc250-xorg/lib:/opt/bc250/lib GALLIUM_DRIVER=radeonsi MESA_LOADER_DRIVER_OVERRIDE=radeonsi /usr/lib/xorg/Xorg :0 -config /work/parity002/xorg-glamor.conf -logfile /work/parity002/Xorg-recovery001.log -nolisten tcp -noreset -ac vt7 > "$r/xorg-recovery001-launch.log" 2>&1 </dev/null &
echo "$!" > "$r/xorg-recovery001.pid"
echo "recovery_xorg_pid=$!"
