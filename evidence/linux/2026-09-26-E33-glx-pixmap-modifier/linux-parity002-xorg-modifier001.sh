set -eu
r=/mnt/bc250-build/root/work/parity002
[ "$(cat "$r/full012/exit.txt")" = 3 ]
[ "$(cat "$r/tfp-native.exit")" = 0 ]
pid=$(cat "$r/xorg-recovery003.pid")
[ "$(cat /proc/$pid/comm)" = Xorg ]
kill -TERM "$pid"
for i in 1 2 3 4 5; do [ ! -d /proc/$pid ] && break; sleep 1; done
if [ -d /proc/$pid ]; then [ "$(awk '{print $3}' /proc/$pid/stat)" = Z ]; fi
[ ! -e "$r/xorg-modifier001.pid" ]
nohup chroot /mnt/bc250-build/root /usr/bin/env AMD_DEBUG=export_modifier LD_LIBRARY_PATH=/opt/bc250-xorg/lib:/opt/bc250/lib GALLIUM_DRIVER=radeonsi MESA_LOADER_DRIVER_OVERRIDE=radeonsi /usr/lib/xorg/Xorg :0 -config /work/parity002/xorg-glamor.conf -logfile /work/parity002/Xorg-modifier001.log -nolisten tcp -noreset -ac vt7 > "$r/xorg-modifier001-launch.log" 2>&1 </dev/null &
echo "$!" > "$r/xorg-modifier001.pid"
echo "recovery_xorg_pid=$!"
