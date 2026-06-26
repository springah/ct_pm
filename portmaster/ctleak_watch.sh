#!/bin/sh
# ctleak_watch.sh -- track ct RSS growth rate + map/fd/thread counts to localize a leak.
LOG=/tmp/ctleak.log
DUR="${1:-1200}"
: > "$LOG"
echo "# t_uptime  RSS_MB  VmData_MB  maps  fds  threads" >> "$LOG"
end=$(( $(date +%s) + DUR ))
while [ "$(date +%s)" -lt "$end" ]; do
  pid=$(pidof ct 2>/dev/null | awk '{print $1}')
  if [ -n "$pid" ] && [ -r "/proc/$pid/status" ]; then
    up=$(ps -o etime= -p "$pid" 2>/dev/null | tr -d ' ')
    rss=$(awk '/VmRSS/{print int($2/1024)}' "/proc/$pid/status")
    dat=$(awk '/VmData/{print int($2/1024)}' "/proc/$pid/status")
    mp=$(wc -l < "/proc/$pid/maps")
    fd=$(ls "/proc/$pid/fd" 2>/dev/null | wc -l)
    th=$(ls "/proc/$pid/task" 2>/dev/null | wc -l)
    printf '%-9s %6s %8s %5s %4s %5s\n' "$up" "$rss" "$dat" "$mp" "$fd" "$th" >> "$LOG"
  else
    echo "# $(date '+%H:%M:%S') ct not running" >> "$LOG"
  fi
  sleep 10
done
echo "# done" >> "$LOG"
