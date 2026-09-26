set -eu
r=/mnt/bc250-build/root/work/parity002
[ "$(cat "$r/xorg-build.exit")" = 0 ]
old=$(cat "$r/xorg-zink.pid")
if [ -d /proc/$old ]; then [ "$(awk '{print $3}' /proc/$old/stat)" = Z ]; fi
[ ! -e "$r/xorg-radeonsi.pid" ]
nohup chroot /mnt/bc250-build/root /usr/bin/env LD_LIBRARY_PATH=/opt/bc250-xorg/lib:/opt/bc250/lib GALLIUM_DRIVER=radeonsi MESA_LOADER_DRIVER_OVERRIDE=radeonsi /usr/lib/xorg/Xorg :0 -config /work/parity002/xorg-glamor.conf -logfile /work/parity002/Xorg-radeonsi.log -nolisten tcp -noreset -ac vt7 > "$r/xorg-radeonsi-launch.log" 2>&1 </dev/null &
echo "$!" > "$r/xorg-radeonsi.pid"
echo "radeonsi_xorg_pid=$!"
