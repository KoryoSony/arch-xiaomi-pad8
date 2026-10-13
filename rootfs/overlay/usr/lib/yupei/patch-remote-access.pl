#!/usr/bin/perl
# Patch GNOME Shell's status/remoteAccess.js (ScreenSharingIndicator) so the
# panel's own session is not shown as "screen is being shared". That session
# (yupei-present-gnome) is a RemoteDesktop handle plus a ScreenCast handle
# created together; a ScreenCast handle within 2 s of a RemoteDesktop handle
# (either order) is ignored with it. Other sharing (portal ScreenCast from a
# browser or call, alone) still shows the indicator. Exits 1 if the code no
# longer matches, so the caller leaves GNOME unpatched.
use strict;
local $/;
my $f = shift;
open my $in, '<', $f or die;
my $s = <$in>;
close $in;
my $old = <<'JS';
    _onNewHandle(handle) {
        if (handle.isRecording)
            return;

        this._handles.add(handle);
JS
my $new = <<'JS';
    _onNewHandle(handle) {
        if (handle.isRecording)
            return;

        // yupei: skip the panel's own RemoteDesktop + ScreenCast pair.
        const yupeiNow = GLib.get_monotonic_time();
        const yupeiType = handle.constructor.$gtype.name;
        const yupeiPaired = t => t !== undefined && yupeiNow - t < 2000000;
        if (yupeiType === 'MetaRemoteDesktopSessionHandle') {
            this._yupeiRdTime = yupeiNow;
            if (this._yupeiCast && yupeiPaired(this._yupeiCast.time)) {
                this._handles.delete(this._yupeiCast.handle);
                this._sync();
            }
            return;
        }
        if (yupeiPaired(this._yupeiRdTime))
            return;
        this._yupeiCast = {handle, time: yupeiNow};

        this._handles.add(handle);
JS
my $n = () = $s =~ /\Q$old\E/g;
exit 1 unless $n == 1;
$s =~ s/\Q$old\E/$new/;
open my $out, '>', $f or die;
print $out $s;
close $out;
exit 0;
