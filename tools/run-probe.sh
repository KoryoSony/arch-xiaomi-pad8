#!/usr/bin/env bash
# Flash an init_boot probe image (tablet in fastboot), boot it and report
# whether it reboot-looped back to fastboot (= reached its checkpoint).
img=${1:?image}
t() { perl -e 'alarm shift; exec @ARGV' "$@"; }
t 20 fastboot flash init_boot "$img" 2>&1 | grep -E 'Writing|FAIL'
t 10 fastboot set_active a >/dev/null 2>&1
t 10 fastboot reboot >/dev/null 2>&1
start=$(date +%s); sleep 15
while [ $(( $(date +%s) - start )) -lt ${2:-200} ]; do
    if t 2 fastboot devices 2>/dev/null | grep -q fastboot; then
        echo "REACHED: back in fastboot after $(( $(date +%s) - start ))s ($(t 8 fastboot getvar slot-retry-count:a 2>&1 | grep -o 'a: [0-9]*'))"
        exit 0
    fi
    sleep 4
done
echo "HUNG: no fastboot after $(( $(date +%s) - start ))s"
exit 1
