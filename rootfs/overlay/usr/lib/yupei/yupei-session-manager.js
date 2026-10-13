// yupei-session-manager: the part of gnome-session that GNOME Shell needs for
// its power menu. GNOME Shell runs here without gnome-session (it is started
// headless by /usr/lib/yupei/gnome-session), so it would only offer "Log Out".
// This owns org.gnome.SessionManager on the session bus, reports power off and
// restart as available, opens GNOME Shell's own end-session dialog, and acts
// when the user confirms it. Run with: gjs -m yupei-session-manager.js
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

const IFACE = `<node><interface name="org.gnome.SessionManager">
  <method name="Logout"><arg type="u" direction="in"/></method>
  <method name="Shutdown"/>
  <method name="Reboot"/>
  <method name="Suspend"/>
  <method name="CanShutdown"><arg type="u" direction="out"/></method>
  <method name="CanReboot"><arg type="u" direction="out"/></method>
  <method name="CanSuspend"><arg type="u" direction="out"/></method>
  <method name="IsInhibited"><arg type="u" direction="in"/><arg type="b" direction="out"/></method>
  <property name="SessionIsActive" type="b" access="read"/>
  <signal name="InhibitorAdded"><arg type="o"/></signal>
  <signal name="InhibitorRemoved"><arg type="o"/></signal>
</interface></node>`;

// GNOME Shell's ActionAvailability and EndSessionDialog types.
const AVAILABLE = 3, UNAVAILABLE = 0;
const DIALOG_LOGOUT = 0, DIALOG_SHUTDOWN = 1, DIALOG_RESTART = 2;

const bus = Gio.DBus.session;

function run(argv) {
    print(`yupei-session-manager: ${argv.join(' ')}`);
    try {
        Gio.Subprocess.new(argv, Gio.SubprocessFlags.NONE);
    } catch (e) {
        logError(e, `running ${argv[0]}`);
    }
}

function openDialog(type) {
    bus.call('org.gnome.Shell', '/org/gnome/SessionManager/EndSessionDialog',
        'org.gnome.SessionManager.EndSessionDialog', 'Open',
        new GLib.Variant('(uuuao)', [type, 0, 60, []]),
        null, Gio.DBusCallFlags.NONE, -1, null, (conn, res) => {
            try {
                conn.call_finish(res);
            } catch (e) {
                logError(e, 'opening the end-session dialog');
            }
        });
}

const impl = {
    Logout(mode) {
        // 0 = ask first; 1/2 = no confirmation / forced.
        if (mode === 0)
            openDialog(DIALOG_LOGOUT);
        else
            run(['systemctl', 'restart', 'yupei-desktop.service']);
    },
    Shutdown() { openDialog(DIALOG_SHUTDOWN); },
    Reboot() { openDialog(DIALOG_RESTART); },
    Suspend() {},
    CanShutdown() { return AVAILABLE; },
    CanReboot() { return AVAILABLE; },
    CanSuspend() { return UNAVAILABLE; },   // no working suspend yet
    IsInhibited(flags) { return false; },
    get SessionIsActive() { return true; },
};

const exported = Gio.DBusExportedObject.wrapJSObject(IFACE, impl);
exported.export(bus, '/org/gnome/SessionManager');

// The user's answer in the dialog.
const actions = {
    ConfirmedShutdown: ['systemctl', 'poweroff'],
    ConfirmedReboot: ['systemctl', 'reboot'],
    ConfirmedLogout: ['systemctl', 'restart', 'yupei-desktop.service'],
};
for (const [signal, argv] of Object.entries(actions)) {
    bus.signal_subscribe(null, 'org.gnome.SessionManager.EndSessionDialog', signal,
        '/org/gnome/SessionManager/EndSessionDialog', null, Gio.DBusSignalFlags.NONE,
        () => run(argv));
}

Gio.bus_own_name_on_connection(bus, 'org.gnome.SessionManager', Gio.BusNameOwnerFlags.REPLACE,
    () => print('yupei-session-manager: owns org.gnome.SessionManager'),
    () => { print('yupei-session-manager: lost org.gnome.SessionManager'); loop.quit(); });

const loop = new GLib.MainLoop(null, false);
loop.run();
