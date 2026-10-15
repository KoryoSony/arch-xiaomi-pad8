// Quick settings for the Xiaomi Pad 8 (yupei): flashlight, automatic
// brightness (gsd-power's ambient light sensor setting) and temperatures.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import St from 'gi://St';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import {QuickMenuToggle, QuickToggle, SystemIndicator} from 'resource:///org/gnome/shell/ui/quickSettings.js';

const LEDS = '/sys/class/leds';
// Both flashlight diodes: their currents (mA, max 400) and the switch that
// drives them together (/etc/udev/rules.d/70-yupei-flashlight.rules makes
// these writable).
const TORCH_MA = 150;
const TORCHES = ['led:torch_0', 'led:torch_1'];
const TORCH_SWITCH = 'led:switch_2';

function readText(path) {
    try {
        const [, bytes] = GLib.file_get_contents(path);
        return new TextDecoder().decode(bytes).trim();
    } catch {
        return null;
    }
}

// A plain in-place write (sysfs attributes can't be replaced or created).
function writeText(path, text) {
    try {
        const io = Gio.File.new_for_path(path).open_readwrite(null);
        io.get_output_stream().write_all(new TextEncoder().encode(text), null);
        io.close(null);
        return true;
    } catch (e) {
        logError(e, `xiaomi-pad8: writing ${path}`);
        return false;
    }
}

const FlashlightToggle = GObject.registerClass(
class FlashlightToggle extends QuickToggle {
    _init() {
        super._init({title: 'Flashlight', iconName: 'media-flash-symbolic', toggleMode: true});
        this.sync();
        this.connect('clicked', () => this._set(this.checked));
    }

    sync() {
        this.checked = readText(`${LEDS}/${TORCH_SWITCH}/brightness`) === '1';
    }

    _set(on) {
        if (on) {
            for (const t of TORCHES)
                writeText(`${LEDS}/${t}/brightness`, `${TORCH_MA}`);
            writeText(`${LEDS}/${TORCH_SWITCH}/brightness`, '1');
        } else {
            writeText(`${LEDS}/${TORCH_SWITCH}/brightness`, '0');
            for (const t of TORCHES)
                writeText(`${LEDS}/${t}/brightness`, '0');
        }
        this.sync();
    }

    destroy() {
        if (this.checked)
            this._set(false);
        super.destroy();
    }
});

const AutoBrightnessToggle = GObject.registerClass(
class AutoBrightnessToggle extends QuickToggle {
    _init() {
        super._init({title: 'Auto Brightness', iconName: 'display-brightness-symbolic', toggleMode: true});
        this._settings = new Gio.Settings({schema_id: 'org.gnome.settings-daemon.plugins.power'});
        this._settings.bind('ambient-enabled', this, 'checked', Gio.SettingsBindFlags.DEFAULT);
    }
});

// name, thermal zone type prefix (hottest match wins)
const SENSORS = [
    ['CPU', 'cpu-'],
    ['GPU', 'gpu-'],
    ['Battery', 'battery'],
    ['Charger', 'charger_therm'],
    ['Display', 'display_therm'],
    ['Case', 'quiet_therm'],
];

const TemperatureToggle = GObject.registerClass(
class TemperatureToggle extends QuickMenuToggle {
    _init(extensionPath) {
        const gicon = Gio.icon_new_for_string(`${extensionPath}/icons/thermometer-symbolic.svg`);
        super._init({title: 'Temperature', gicon, toggleMode: false});
        this.menu.setHeader(gicon, 'Temperatures');
        this._zones = this._findZones();
        this._items = SENSORS.filter(([, prefix]) => this._zones[prefix].length).map(([name, prefix]) => {
            const item = new PopupMenu.PopupMenuItem(name, {reactive: false});
            item._value = new St.Label({text: '', x_expand: true, x_align: Clutter.ActorAlign.END});
            item.add_child(item._value);
            this.menu.addMenuItem(item);
            return [item, prefix];
        });
        this.connect('clicked', () => this.menu.open());
        this.update();
    }

    _findZones() {
        const zones = Object.fromEntries(SENSORS.map(([, p]) => [p, []]));
        const dir = Gio.File.new_for_path('/sys/class/thermal');
        try {
            const e = dir.enumerate_children('standard::name', Gio.FileQueryInfoFlags.NONE, null);
            let info;
            while ((info = e.next_file(null))) {
                const n = info.get_name();
                if (!n.startsWith('thermal_zone'))
                    continue;
                const type = readText(`/sys/class/thermal/${n}/type`) ?? '';
                for (const [, prefix] of SENSORS) {
                    if (type === prefix || (prefix.endsWith('-') && type.startsWith(prefix)))
                        zones[prefix].push(`/sys/class/thermal/${n}/temp`);
                }
            }
        } catch (e) {
            logError(e, 'xiaomi-pad8: thermal zones');
        }
        return zones;
    }

    _read(prefix) {
        let best = null;
        for (const p of this._zones[prefix]) {
            const v = parseInt(readText(p) ?? '', 10);
            if (!isNaN(v) && v > -40000 && v < 150000 && (best === null || v > best))
                best = v;
        }
        return best === null ? null : best / 1000;
    }

    update() {
        const cpu = this._read('cpu-');
        this.subtitle = cpu === null ? '' : `CPU ${cpu.toFixed(0)} °C`;
        for (const [item, prefix] of this._items) {
            const t = this._read(prefix);
            item._value.text = t === null ? '–' : `${t.toFixed(1)} °C`;
        }
    }
});

const Indicator = GObject.registerClass(
class Indicator extends SystemIndicator {
    _init(extensionPath) {
        super._init();
        this._flash = new FlashlightToggle();
        this._temps = new TemperatureToggle(extensionPath);
        this.quickSettingsItems.push(this._flash, new AutoBrightnessToggle(), this._temps);
    }

    // While the quick settings are open: keep the readings fresh.
    refresh(open) {
        if (this._timer) {
            GLib.source_remove(this._timer);
            this._timer = 0;
        }
        if (!open)
            return;
        this._flash.sync();
        this._temps.update();
        this._timer = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 2, () => {
            this._temps.update();
            return GLib.SOURCE_CONTINUE;
        });
    }

    destroy() {
        this.refresh(false);
        this.quickSettingsItems.forEach(i => i.destroy());
        super.destroy();
    }
});

export default class XiaomiPad8Extension extends Extension {
    enable() {
        this._indicator = new Indicator(this.path);
        const qs = Main.panel.statusArea.quickSettings;
        qs.addExternalIndicator(this._indicator);
        this._menuId = qs.menu.connect('open-state-changed', (m, open) => this._indicator.refresh(open));
    }

    disable() {
        Main.panel.statusArea.quickSettings.menu.disconnect(this._menuId);
        this._indicator.destroy();
        this._indicator = null;
    }
}
