#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Resource stats for rhkd on a camera, as a Markdown table for PR descriptions and release notes.
#   tools/rhkd-stats.sh <camera-ip> [seconds]       (uses $SSHPASS with sshpass if set, else plain ssh)
# Measures over [seconds] (default 20): CPU of rhkd and the raptor daemons it uses (from /proc/<pid>/stat ticks),
# rhkd memory (RSS, peak RSS, private dirty), file sizes on flash, overlay use and free RAM. Start a live view or
# recording during the window to measure that load; the table says what was running (live sessions, recording).
set -eu
HOST=${1:?usage: $0 <camera-ip> [seconds]}
SECS=${2:-20}
SSH="ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8"
[ -n "${SSHPASS:-}" ] && SSH="sshpass -e $SSH"

$SSH root@"$HOST" SECS="$SECS" sh -s <<'EOF'
hz=100
ticks() { [ -r /proc/$1/stat ] && awk '{print $14 + $15}' /proc/$1/stat || echo 0; }
pids=""
for d in rhkd rvd rad; do pids="$pids $d:$(pidof $d | awk '{print $1}')"; done
before=""
for e in $pids; do before="$before $(ticks ${e#*:})"; done
t0=$(awk '{print $1}' /proc/uptime)
sleep "$SECS"
t1=$(awk '{print $1}' /proc/uptime)
set -- $before
rhkd=$(pidof rhkd | awk '{print $1}')
ver=$(/usr/bin/rhkd --version 2>/dev/null | head -1)
soc=$(soc -m 2>/dev/null || awk -F: '/system type|machine/{print $2; exit}' /proc/cpuinfo)
live=$(logread | grep "rhkd\[$rhkd\]" | grep -c "Start [01]:" || true)
ended=$(logread | grep "rhkd\[$rhkd\]" | grep -c "Ending stream" || true)
echo "**$(hostname)** ($soc), $ver, measured over ${SECS} s"
echo
echo "| | value |"
echo "|---|---|"
for e in $pids; do
	name=${e%%:*}; pid=${e#*:}
	b=$1; shift
	[ -n "$pid" ] || { echo "| CPU $name | not running |"; continue; }
	a=$(ticks "$pid")
	echo "| CPU $name | $(awk -v a="$a" -v b="$b" -v t0="$t0" -v t1="$t1" -v hz=$hz 'BEGIN{printf "%.1f %%", (a-b)/hz/(t1-t0)*100}') |"
done
if [ -n "$rhkd" ]; then
	awk '/VmRSS|VmHWM/{printf "| rhkd %s | %s kB |\n", ($1=="VmRSS:"?"RSS":"peak RSS"), $2}' /proc/$rhkd/status
	[ -r /proc/$rhkd/smaps ] && awk '/Private_Dirty/{s+=$2} END{printf "| rhkd private dirty | %d kB |\n", s}' /proc/$rhkd/smaps
	echo "| rhkd threads | $(ls /proc/$rhkd/task | wc -l) |"
fi
for f in /usr/bin/rhkd /usr/bin/hap-setupgen /usr/sbin/rhkd-setup /etc/init.d/S98rhkd; do
	[ -f "$f" ] && echo "| $f | $(ls -l "$f" | awk '{printf "%.0f KB", $5/1024}') |"
done
[ -d /etc/homekit ] && echo "| pairing store /etc/homekit | $(du -sk /etc/homekit | awk '{print $1}') KB |"
df -k /overlay 2>/dev/null | awk 'NR==2{printf "| overlay (flash) used | %d / %d KB (%s) |\n", $3, $2, $5}'
# Kernel 3.10 (T31) has no MemAvailable: estimate it as free + buffers + cached.
awk '{v[$1]=$2} END{a=("MemAvailable:" in v) ? v["MemAvailable:"] : v["MemFree:"]+v["Buffers:"]+v["Cached:"];
	printf "| RAM available | %d / %d kB%s |\n", a, v["MemTotal:"], ("MemAvailable:" in v) ? "" : " (free+buffers+cached)"}' /proc/meminfo
echo "| live-view sessions started / ended since rhkd start | $live / $ended |"
EOF
