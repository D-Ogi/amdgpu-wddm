for c in /sys/kernel/debug/dri/*/DP-1/; do
  echo "=== $c ==="
  cat "${c}link_settings" 2>&1
done
