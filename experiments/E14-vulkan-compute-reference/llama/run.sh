#!/bin/sh
# E14, the inference half: greedy generation (temperature 0, fixed seed) on the GPU through Vulkan and on the CPU, the
# GPU run repeated to see whether its output is deterministic, llama-bench, and the amdgpu trace events around one short
# GPU run (what a kernel driver is asked to do by a real Vulkan compute workload). Output under /tmp/e14/llama (RAM).
# The reference for Windows is the hash of the GPU's text, not the CPU's: the two may part at a near-tie of two tokens.
set -u
O=/tmp/e14/llama
M=/tmp/e14/models
T=/sys/kernel/tracing
mkdir -p $O
llama-bench --list-devices 2>&1 | tail -3

gen() {      # gen <model file without .gguf> <tag> <tokens> <ngl> <prompt>
	timeout 300 llama-completion -m "$M/$1.gguf" -p "$5" -n "$3" --temp 0 --seed 1 -ngl "$4" -no-cnv > "$O/$2-ngl$4.out" 2> "$O/$2-ngl$4.err" < /dev/null
	echo "$2 ngl=$4 exit=$? bytes=$(wc -c < "$O/$2-ngl$4.out") sha256=$(sha256sum < "$O/$2-ngl$4.out" | cut -c1-16)"
	grep -E 'eval time' "$O/$2-ngl$4.err" | sed 's/.*common_perf_print: *//'
}
again() {    # three more GPU runs, hashes only
	for i in 1 2 3; do
		timeout 300 llama-completion -m "$M/$1.gguf" -p "$4" -n "$3" --temp 0 --seed 1 -ngl 99 -no-cnv 2>/dev/null < /dev/null | sha256sum | cut -c1-16
	done | sort | uniq -c | sed "s/^/$2 repeat: /"
}
for ngl in 99 0; do
	gen stories260K stories260K 96 $ngl "Once upon a time"
	gen stories15M-q4_0 stories15M 96 $ngl "Once upon a time"
	gen tinyllama-1.1b-chat-v1.0.Q4_0 tinyllama 64 $ngl "The capital of France is"
done
again stories15M-q4_0 stories15M 96 "Once upon a time"
again tinyllama-1.1b-chat-v1.0.Q4_0 tinyllama 64 "The capital of France is"

echo "== llama-bench"
timeout 300 llama-bench -m $M/stories15M-q4_0.gguf -ngl 99,0 -p 512 -n 128 -r 3 2>&1 | tee $O/bench.txt | grep -E '^\|'
timeout 600 llama-bench -m $M/tinyllama-1.1b-chat-v1.0.Q4_0.gguf -ngl 99,0 -p 512 -n 128 -r 3 2>&1 | tee $O/bench-tinyllama.txt | grep -E '^\|'

echo "== amdgpu events around one short GPU run (stories260K, 16 tokens)"
mountpoint -q $T || mount -t tracefs nodev $T
echo 0 > $T/tracing_on; echo 65536 > $T/buffer_size_kb; echo > $T/trace
echo ':mod:amdgpu' > $T/set_event 2>/dev/null || echo 'amdgpu:*' > $T/set_event
echo 1 > $T/tracing_on
echo "bc250 begin llama" > $T/trace_marker
timeout 120 llama-completion -m $M/stories260K.gguf -p "Once upon a time" -n 16 --temp 0 --seed 1 -ngl 99 -no-cnv > /dev/null 2>&1 < /dev/null
echo "bc250 end llama" > $T/trace_marker
echo 0 > $T/tracing_on
cat $T/trace > $O/amdgpu-events-llama260k.txt
echo "lines $(wc -l < $O/amdgpu-events-llama260k.txt) overruns $(cat $T/per_cpu/cpu*/stats | awk '/overrun|dropped/ {s+=$2} END {print s+0}')"
awk '{for (i=1;i<=NF;i++) if ($i ~ /^amdgpu_[a-z_]+:$/) c[$i]++} END {for (k in c) print c[k], k}' $O/amdgpu-events-llama260k.txt | sort -rn
grep 'amdgpu_iv' $O/amdgpu-events-llama260k.txt | sed 's/.*client_id/client_id/' | awk '{print $1, $2, $3}' | sort | uniq -c | sort -rn
for h in /sys/class/drm/card*/device/hwmon/hwmon*; do echo "after: power_uW $(cat $h/power1_input) temp_mC $(cat $h/temp1_input) sclk_Hz $(cat $h/freq1_input)"; done
dmesg | grep -iE 'amdgpu.*(timeout|reset|ring .* timed|page fault)' | tail -5
