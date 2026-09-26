set -eu
r=/mnt/bc250-build/root
[ "$(cat "$r/work/parity002/window-radeonsi.exit")" = 0 ]
[ "$(cat "$r/work/parity002/xorg-build.exit")" = 0 ]
[ ! -e "$r/work/parity002/full007" ]
mkdir "$r/work/parity002/full007"
cat > "$r/work/parity002/full007/run.sh" <<'EOF'
#!/bin/sh
set -eu
export DISPLAY=:0 LD_LIBRARY_PATH=/opt/bc250/lib:/opt/piglit/lib/piglit/lib VK_DRIVER_FILES=/opt/bc250/share/vulkan/icd.d/radeon_icd.x86_64.json
export GALLIUM_DRIVER=zink MESA_LOADER_DRIVER_OVERRIDE=zink PIGLIT_PLATFORM=glx MESA_SHADER_CACHE_DISABLE=true
export PIGLIT_BUILD_DIR=/opt/piglit/lib/piglit PIGLIT_SOURCE_DIR=/opt/piglit/lib/piglit PYTHONPATH=/opt/piglit/lib/piglit
export BC250_PIGLIT_EVENTS=/work/parity002/full007/events.jsonl BC250_STOP_FILE=/work/parity002/STOP
export PATH=/opt/piglit/lib/piglit/bin:$PATH
unset RADV_DEBUG MESA_GL_VERSION_OVERRIDE MESA_GLSL_VERSION_OVERRIDE
sha256sum /opt/bc250/lib/libvulkan_radeon.so /opt/bc250/lib/libgallium*.so /work/parity002/piglit*guard.py > /work/parity002/full007/identity.sha256
python3 /work/src/piglit/piglit print-cmd --format '{name}' quick > /work/parity002/full007/cases.txt
grep -v '/piglit-linux-guard.py$' /work/parity002/full003/identity.sha256 | sha256sum -c - > /work/parity002/full007/identity-check.txt
export BC250_PIGLIT_RESUME_LIST=/work/parity002/remaining-full006.txt
python3 /work/parity002/piglit-linux-guard.py quick /work/parity002/full007/results
EOF
cat > "$r/work/parity002/full007/worker.sh" <<'EOF'
#!/bin/sh
set +e
chroot /mnt/bc250-build/root sh /work/parity002/full007/run.sh > /mnt/bc250-build/root/work/parity002/full007/stdout.txt 2> /mnt/bc250-build/root/work/parity002/full007/stderr.txt
rc=$?
echo "$rc" > /mnt/bc250-build/root/work/parity002/full007/exit.txt
EOF
nohup sh "$r/work/parity002/full007/worker.sh" > "$r/work/parity002/full007/launch.log" 2>&1 </dev/null &
echo "$!" > "$r/work/parity002/full007/worker.pid"
echo "full_worker_pid=$!"
