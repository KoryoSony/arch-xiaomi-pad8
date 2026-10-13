#!/usr/bin/env bash
# Bring-up probe: builds out/init_boot-probe-K.img whose /init loads the first
# K modules of the vendor first-stage list (with cmdline args, minus blocklist)
# and then reboots. A reboot loop ending in fastboot means "got past K";
# a frozen boot means one of the first K hangs.
set -euo pipefail
. "$(dirname "$0")/../config.sh"
K=${1:?usage: $0 K [list]}
LIST=${2:-modules.load}
d=$WORK/probe-$K
rm -rf "$d"; mkdir -p "$d"/{bin,dev,proc,sys,run}
cp "$WORK/initramfs/bin/busybox" "$d/bin/busybox"
cat > "$d/init" <<EOS
#!/bin/busybox sh
/bin/busybox --install -s /bin
mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t devtmpfs devtmpfs /dev
mod_args() { for t in \$(cat /proc/cmdline); do case \$t in "\$1".*=*) printf '%s ' "\${t#"\$1".}" ;; esac; done; }
blocked() { grep -qE "^blocklist +\$1(\\.ko)?\\\$" /lib/modules/modules.blocklist 2>/dev/null; }
i=0
while read -r m; do
    [ \$i -ge $K ] && break
    [ -f "/lib/modules/\$m" ] || continue
    f=\$(basename "\$m" .ko); n=\$(echo "\$f" | tr - _)
    blocked "\$f" && continue
    echo "probe: insmod \$f" > /dev/kmsg
    insmod "/lib/modules/\$m" \$(mod_args "\$n")
    i=\$((i+1))
done < /lib/modules/$LIST
echo "probe: loaded \$i, rebooting" > /dev/kmsg
reboot -f
EOS
chmod 755 "$d/init"
(cd "$d" && find . | sort | cpio -o -H newc -R 0:0 --quiet) | lz4 -l -12 -f - "$WORK/probe-$K.lz4" >/dev/null
mkbootimg --header_version 4 --ramdisk "$WORK/probe-$K.lz4" -o "$OUT/init_boot-probe-$K.img"
echo "$OUT/init_boot-probe-$K.img"
