// yupei refresh-switcher: lowers the panel's refresh rate when nothing is
// happening, like Android does. The display is most of the idle power draw:
// 120 Hz -> 60 Hz saves ~180 mA, 30 Hz another ~100 mA (docs/PORTING.md).
//   input            -> the rate chosen in Settings (~/.config/monitors.xml)
//   3 s without input  -> 60 Hz (video still smooth)
//   30 s without input, no MPRIS player playing -> 30 Hz
// Uses mutter's IdleMonitor watches (no polling) and applies modes as
// *temporary* monitor configs, so the saved configuration is never changed.
// The Xiaomi Pad 8 panel switches these modes without flicker.
// Run with: gjs -m refresh-switcher.js (GNOME session autostart)
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

const IDLE_MID_MS = 3000, IDLE_LOW_MS = 30000;
const MID_HZ = 60, LOW_HZ = 30;
const CONNECTOR = 'DSI-1';

const bus = Gio.DBus.session;
const loop = new GLib.MainLoop(null, false);

function call(dest, path, iface, method, params, type) {
    return bus.call_sync(dest, path, iface, method, params,
        type ? new GLib.VariantType(type) : null, Gio.DBusCallFlags.NONE, 5000, null);
}

const DC = ['org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig',
    'org.gnome.Mutter.DisplayConfig'];
const IM = ['org.gnome.Mutter.IdleMonitor', '/org/gnome/Mutter/IdleMonitor/Core',
    'org.gnome.Mutter.IdleMonitor'];

// The user's chosen rate: the saved (persistent) config, else the current one.
function preferredRate(current) {
    try {
        const path = GLib.build_filenamev([GLib.get_user_config_dir(), 'monitors.xml']);
        const [, bytes] = GLib.file_get_contents(path);
        const xml = new TextDecoder().decode(bytes);
        const m = xml.match(/<connector>DSI-1<\/connector>[\s\S]*?<rate>([0-9.]+)<\/rate>/);
        if (m)
            return parseFloat(m[1]);
    } catch (e) {
        // no saved config yet
    }
    return current;
}

let applying = null;   // mode id we last applied ourselves

function setRate(hz) {
    const [serial, monitors, logicals] = call(...DC, 'GetCurrentState', null,
        '(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})').deepUnpack();
    const mon = monitors.find(m => m[0][0] === CONNECTOR);
    if (!mon)
        return;
    const modes = mon[1];
    const current = modes.find(m => m[6]['is-current']?.deepUnpack());
    if (!current)
        return;
    const [w, h] = [current[1], current[2]];
    // The mode closest to the wanted rate at the current resolution.
    const cands = modes.filter(m => m[1] === w && m[2] === h);
    const want = cands.reduce((a, b) => Math.abs(b[3] - hz) < Math.abs(a[3] - hz) ? b : a);
    if (want[0] === current[0])
        return;
    const lms = logicals.map(([x, y, scale, transform, primary, mons]) =>
        [x, y, scale, transform, primary,
            mons.map(([conn]) => [conn, conn === CONNECTOR ? want[0] : current[0], {}])]);
    applying = want[0];
    try {
        call(...DC, 'ApplyMonitorsConfig', new GLib.Variant('(uua(iiduba(ssa{sv}))a{sv})',
            [serial, 1 /* temporary */, lms, {}]), null);
        print(`refresh-switcher: ${want[0]}`);
    } catch (e) {
        logError(e, 'ApplyMonitorsConfig');
    }
}

function currentRate() {
    const [, monitors] = call(...DC, 'GetCurrentState', null,
        '(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})').deepUnpack();
    const mon = monitors.find(m => m[0][0] === CONNECTOR);
    const cur = mon?.[1].find(m => m[6]['is-current']?.deepUnpack());
    return cur ? cur[3] : 60;
}

function mediaPlaying() {
    try {
        const [names] = call('org.freedesktop.DBus', '/org/freedesktop/DBus',
            'org.freedesktop.DBus', 'ListNames', null, '(as)').deepUnpack();
        return names.filter(n => n.startsWith('org.mpris.MediaPlayer2.')).some(n => {
            try {
                const [v] = call(n, '/org/mpris/MediaPlayer2', 'org.freedesktop.DBus.Properties',
                    'Get', new GLib.Variant('(ss)', ['org.mpris.MediaPlayer2.Player', 'PlaybackStatus']),
                    '(v)').deepUnpack();
                return v.deepUnpack() === 'Playing';
            } catch (e) {
                return false;
            }
        });
    } catch (e) {
        return false;
    }
}

let preferred = preferredRate(currentRate());
let activeWatch = 0;
const [midWatch] = call(...IM, 'AddIdleWatch', new GLib.Variant('(t)', [IDLE_MID_MS]), '(u)').deepUnpack();
const [lowWatch] = call(...IM, 'AddIdleWatch', new GLib.Variant('(t)', [IDLE_LOW_MS]), '(u)').deepUnpack();

function armActive() {
    if (!activeWatch)
        [activeWatch] = call(...IM, 'AddUserActiveWatch', null, '(u)').deepUnpack();
}

bus.signal_subscribe('org.gnome.Mutter.IdleMonitor', 'org.gnome.Mutter.IdleMonitor', 'WatchFired',
    '/org/gnome/Mutter/IdleMonitor/Core', null, Gio.DBusSignalFlags.NONE,
    (c, s, p, i, sig, params) => {
        const [id] = params.deepUnpack();
        if (id === activeWatch) {
            activeWatch = 0;
            setRate(preferred);
        } else if (id === midWatch && preferred > MID_HZ) {
            setRate(MID_HZ);
            armActive();
        } else if (id === lowWatch && !mediaPlaying()) {
            setRate(Math.min(preferred, LOW_HZ));
            armActive();
        }
    });

// The user changed the display settings: follow their new rate.
bus.signal_subscribe('org.gnome.Mutter.DisplayConfig', 'org.gnome.Mutter.DisplayConfig',
    'MonitorsChanged', '/org/gnome/Mutter/DisplayConfig', null, Gio.DBusSignalFlags.NONE, () => {
        const mode = (() => {
            try {
                return currentRate();
            } catch (e) {
                return null;
            }
        })();
        if (applying && mode !== null && applying.endsWith(`@${mode.toFixed(3)}`)) {
            applying = null;   // our own change
            return;
        }
        applying = null;
        preferred = preferredRate(mode ?? preferred);
    });

// Watches only fire when the idle time crosses them: handle starting idle.
const [idleNow] = call(...IM, 'GetIdletime', null, '(t)').deepUnpack();
if (idleNow >= IDLE_LOW_MS && !mediaPlaying()) {
    setRate(Math.min(preferred, LOW_HZ));
    armActive();
} else if (idleNow >= IDLE_MID_MS && preferred > MID_HZ) {
    setRate(MID_HZ);
    armActive();
}

print(`refresh-switcher: preferred ${preferred} Hz`);
loop.run();
