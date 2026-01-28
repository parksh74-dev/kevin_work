#!/bin/sh

INTERVAL=${1:-1}

while true; do
    printf "[%s] " "$(date '+%Y-%m-%d %H:%M:%S')"
    grep -E 'Mem(Total|Free)|Cma(Total|Free)' /proc/meminfo \
    | awk '{printf "%s=%s ", $1, $2} END {print ""}'
    sleep "$INTERVAL"
done
