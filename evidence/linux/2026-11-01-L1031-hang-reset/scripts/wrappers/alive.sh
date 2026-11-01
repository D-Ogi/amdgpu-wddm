date -u; uptime
D=/sys/kernel/debug/dri/0000:01:00.0
timeout 5 cat $D/amdgpu_compute_sched_mask
ps -o pid,stat,comm,args | grep -E 'python|h-jobs|i5-submit' | grep -v grep
for p in $(pgrep python3); do echo "== $p $(cat /proc/$p/stat | cut -d' ' -f3)"; timeout 3 cat /proc/$p/stack; done
