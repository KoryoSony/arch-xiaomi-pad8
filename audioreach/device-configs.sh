#!/usr/bin/env bash
# Builds out/audioreach-yupei-configs.tar.gz: Xiaomi's audio configuration and
# calibration for the Pad 8 (from the stock odm/vendor images in $STOCK),
# adapted for AudioReach's Linux PAL/AGM, installed under /etc:
#   resourcemanager_tuna_mtp.xml / mixer_paths_tuna_mtp.xml   (PAL, by card name)
#   card-defs.xml, usecaseKvManager.xml, backend_conf.xml
#   acdbdata/tuna_mtp/Mi_*                                     (AGM: one .acdb + .qwsp)
# Run inside the build container (needs perl).
set -euo pipefail
. "$(dirname "$0")/../config.sh"
T=$(mktemp -d)
E=$T/etc
mkdir -p "$E/acdbdata/tuna_mtp"
O=$STOCK/odm/etc
cp "$O/audio/sku_tuna/mixer_paths_tuna_mtp.xml" "$O/usecaseKvManager.xml" "$O/backend_conf.xml" "$E/"
cp "$STOCK/vendor/etc/card-defs.xml" "$E/"
cp "$O/acdbdata/Mi/Mi_acdb_cal.acdb" "$O/acdbdata/Mi/Mi_workspaceFileXml.qwsp" "$E/acdbdata/tuna_mtp/"

# resourcemanager: Linux PAL needs <config_max_sessions> (Android's doesn't
# read it: "Could not find stream type map" and every stream fails), and the
# Android-only services must be off.
perl -0pe '
    s{(</config_params>\n)}{$1    <!-- yupei: required by the Linux PAL (from the SM8750 reference) -->
    <config_max_sessions>
        <stream name="PAL_STREAM_LOW_LATENCY" max_size="8" />
        <stream name="PAL_STREAM_DEEP_BUFFER" max_size="3" />
        <stream name="PAL_STREAM_GENERIC" max_size="2" />
        <stream name="PAL_STREAM_RAW" max_size="1" />
        <stream name="PAL_STREAM_PCM_OFFLOAD" max_size="2" />
        <stream name="PAL_STREAM_ULTRA_LOW_LATENCY" max_size="8" />
        <stream name="PAL_STREAM_VOIP_RX" max_size="1" />
        <stream name="PAL_STREAM_VOIP_TX" max_size="1" />
    </config_max_sessions>
} or die "no config_params\n";
    s{(key="context_manager_enable" value =")true}{${1}false};
    s{(key="connectivity_proxy_enabled" value =")true}{${1}false};
    s{(key="signal_handler" value =")true}{${1}false};
    s{(key="logging_level" value =")0xF}{${1}3};
' "$O/audio/sku_tuna/resourcemanager_tuna_mtp.xml" > "$E/resourcemanager_tuna_mtp.xml"

tar -C "$T" -czf "$OUT/audioreach-yupei-configs.tar.gz" etc
rm -rf "$T"
echo "$OUT/audioreach-yupei-configs.tar.gz"
