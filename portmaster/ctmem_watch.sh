#!/bin/sh
# ctmem_watch.sh -- sample ct's memory + swap during a playtest, watch for OOM.
# Runs on the TrimUI (busybox). Logs to /tmp/ctmem.log. Self-exits after DUR:secs.
LOG=/tmp/ctmem.log
DUR="${1:-1800}"            # default 30 min
: > "$LOG"
echo "# started $(date '+%H:%M:%S')  (MemTotal=$(awk '/MemTotal/{print int($2/1024)}' /proc/meminfo)MB, watch ${DUR}s)" >> "$LOG"
echo "# time      pid     ct_RSS  ct_swap  mem_avail  swap_used   peak_RSS" >> "$LOG"

end=$(( $(date +%s) + DUR ))
base_oom=$(dmesg | grep -c "Killed process.*(ct)")
peak=0
saw_ct=0
while [ "$(date +%s)" -lt "$end" ]; do
  ts=$(date '+%H:%M:%S')
  avail=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo)
  sused=$(free -m | awk '/Swap:/{print $3}')
  pid=$(pidof ct 2>/dev/null | awk '{print $1}')
  if [ -n "$pid" ] && [ -r "/proc/$pid/status" ]; then
    saw_ct=1
    rss=$(awk '/VmRSS/{print int($2/1024)}' "/proc/$pid/status")
    vsw=$(awk '/VmSwap/{print int($2/1024)}' "/proc/$pid/status")
    [ -z "$rss" ] && rss=0
    [ "$rss" -gt "$peak" ] && peak=$rss
    printf '%-10s pid=%-6s %5sMB  %5sMB   %5sMB    %5sMB    %5sMB\n' \
      "$ts" "$pid" "$rss" "$vsw" "$avail" "$sused" "$peak" >> "$LOG"
  else
    # ct gone: if we'd seen it and it just vanished, flag a possible OOM/exit
    note="(ct not running)"
    [ "$saw_ct" = 1 ] && note="(ct ENDED -- exit or kill)"
    printf '%-10s %-30s avail=%sMB swap_used=%sMB\n' "$ts" "$note" "$avail" "$sused" >> "$LOG"
    saw_ct=0
  fi
  now_oom=$(dmesg | grep -c "Killed process.*(ct)")
  if [ "$now_oom" -gt "$base_oom" ]; then
    echo "$ts  *** OOM-KILL *** $(dmesg | grep 'Killed process.*(ct)' | tail -1)" >> "$LOG"
    base_oom=$now_oom
  fi
  sleep 2
done
echo "# stopped $(date '+%H:%M:%S')  peak ct RSS=${peak}MB" >> "$LOG"
