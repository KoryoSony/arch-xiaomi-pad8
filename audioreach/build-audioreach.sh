#!/usr/bin/env bash
# Builds Qualcomm's AudioReach userspace for Linux (github.com/AudioReach, the
# same stack meta-audioreach ships): graph services (GSL + ACDB loader), graph
# manager (AGM), PAL, and the PipeWire plugin, plus tinyalsa/tinycompress/
# audio-route. They build the DSP audio graphs from the ACDB calibration; the
# kernel side (spf_core, audio_pkt) comes from Xiaomi's vendor modules.
# Output: out/audioreach.tar.gz, installed under /usr.
# Run inside the arm64 build container.
set -euo pipefail
. "$(dirname "$0")/../config.sh"
pacman -S --noconfirm --needed autoconf automake libtool pkgconf cmake meson ninja \
    glib2 glib2-devel expat dbus pipewire libpipewire alsa-lib >/dev/null
A=$WORK/audioreach
D=$WORK/audioreach-dest
mkdir -p "$A"
rm -rf "$D"

get() {   # get <dir> <url> [branch]
    [ -d "$A/$1" ] || git clone -q --depth 1 ${3:+-b $3} "$2" "$A/$1"
}
autobuild() {   # autobuild <dir> [configure args...]
    local d=$1; shift
    echo "== $d"
    cd "$A/$d"
    [ -x configure ] || autoreconf -fi >/dev/null 2>&1
    ./configure -q --prefix=/usr --sysconfdir=/etc "$@"
    make -s -j"$(nproc)"
    # Into the (throwaway) container too: libtool relinks against /usr/lib.
    make -s install >/dev/null
    make -s DESTDIR="$D" install >/dev/null
}

get tinyalsa     https://github.com/tinyalsa/tinyalsa
get tinycompress https://github.com/alsa-project/tinycompress
get audio-utils  https://github.com/AudioReach/audioreach-audio-utils main
get graphservices https://github.com/AudioReach/audioreach-graphservices
get kernel       https://github.com/AudioReach/audioreach-kernel
get graphmgr     https://github.com/AudioReach/audioreach-graphmgr
get pal          https://github.com/AudioReach/audioreach-pal
get conf         https://github.com/AudioReach/audioreach-conf
get pipewire-plugin https://github.com/AudioReach/audioreach-pipewire-plugin

echo "== tinyalsa (with plugins: PAL's virtual card is served by AGM's plugins)"
cmake -S "$A/tinyalsa" -B "$A/tinyalsa/cbuild" -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=ON -DTINYALSA_USES_PLUGINS=ON -DTINYALSA_BUILD_UTILS=ON >/dev/null
cmake --build "$A/tinyalsa/cbuild" -j"$(nproc)" >/dev/null
cmake --install "$A/tinyalsa/cbuild" >/dev/null
DESTDIR=$D cmake --install "$A/tinyalsa/cbuild" >/dev/null
autobuild tinycompress
autobuild audio-utils/audio-route
# Shared memory for the DSP: DMA-BUF from /dev/dma_heap/system, mapped through
# Xiaomi's audpkt_ion module (/dev/msm_audio_ion); headers from audioreach-kernel.
CPPFLAGS="-I$A/kernel/include/uapi" autobuild graphservices --with-syslog --with-glib --without-cutils \
    --without-ats_transport_tcp_ip --without-ats_data_logging --with-audio_dma_support --with-qcom
autobuild conf --with-qcom
autobuild graphmgr --with-glib --with-syslog --with-no-ipc
autobuild pal --with-glib --with-syslog
# Playback streams only got FL/FR in their channel map, so a 4-channel sink
# (the Pad 8's four speaker amps) left slots 2/3 silent: fill in LB/RB as PAL's
# default 4-channel device map does, so each sink channel reaches one amp.
grep -q "yupei: 4ch playback map" "$A/pipewire-plugin/src/pw-pal-plugin.c" || perl -0pi -e '
    s{(        udata->stream_attributes->out_media_config.ch_info.ch_map\[1\] = PAL_CHMAP_CHANNEL_FR;\n)}
     {$1        if (udata->info.channels >= 4) { /* yupei: 4ch playback map */
            udata->stream_attributes->out_media_config.ch_info.ch_map[2] = PAL_CHMAP_CHANNEL_LB;
            udata->stream_attributes->out_media_config.ch_info.ch_map[3] = PAL_CHMAP_CHANNEL_RB;
        }
} or die "pw-pal-plugin: playback ch_map not found\n";
' "$A/pipewire-plugin/src/pw-pal-plugin.c"
autobuild pipewire-plugin

# Our own PAL sink list replaces it (rootfs/overlay/etc/pipewire/pipewire.conf.d).
rm -f "$D/usr/share/pipewire/pipewire.conf.d/pw-pal-plugin.conf"
tar -C "$D" -czf "$OUT/audioreach.tar.gz" .
echo "audioreach: $OUT/audioreach.tar.gz"
