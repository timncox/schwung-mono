import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = fs.readFileSync(path.join(root, 'src/ui_overtake.js'), 'utf8');

const MoveKnob1 = 20;
const MoveShift = 1;
const MoveMainKnob = 2;
const MoveMainButton = 3;
const MoveBack = 4;

const constants = {
    MoveKnob1, MoveShift, MoveMainKnob, MoveMainButton, MoveBack,
    MoveLeft: 5, MoveRight: 6, MoveUp: 7, MoveDown: 8, MoveRec: 9,
    MoveDelete: 10, MoveCopy: 11, MoveUndo: 12,
    Black: 0, White: 120, LightGrey: 1, BrightRed: 4, Blue: 44,
    Green: 16, BrightGreen: 8, Cyan: 40, Purple: 48,
    YellowGreen: 24, OrangeRed: 28
};

const params = new Map([
    ['track', '0'], ['page', '0'], ['step_page', '0'], ['machine', '0'],
    ['pattern_start', '0'], ['pattern_len', '16'], ['play_order', '0'],
    ['track_follow', '1'], ['track_start', '0'], ['track_len', '16'],
    ['track_rotate', '0'], ['track_div', '1'], ['keyboard_octave', '0'],
    ['steps', new Array(16).fill(0).join(',')],
    ['all_steps', new Array(64).fill(0).join(',')],
    ['track_states', new Array(6).fill(0).join(',')],
    ['status', '0:-1:120:0:0:0:16:0:0:0']
]);

let stateResponses = [];
let stateCalls = 0;
const savedFiles = new Map();
const presetFiles = [];
const announcements = [];

let textActive = false;
let textOptions = null;
let textMidiCalls = 0;
let textTickCalls = 0;
let textDrawCalls = 0;
let textCloseCalls = 0;

const textEntry = {
    openTextEntry(options) {
        textActive = true;
        textOptions = options;
    },
    closeTextEntry() {
        textActive = false;
        textCloseCalls++;
    },
    isTextEntryActive() { return textActive; },
    handleTextEntryMidi(message) {
        textMidiCalls++;
        if ((message[0] & 0xf0) === 0xb0 && message[1] === MoveBack && message[2] > 0) {
            const onCancel = textOptions?.onCancel;
            textActive = false;
            if (onCancel) onCancel();
        }
        return true;
    },
    drawTextEntry() { textDrawCalls++; },
    tickTextEntry() { textTickCalls++; return false; }
};

function confirmText(text) {
    assert(textActive, 'text entry must be open before confirming');
    const onConfirm = textOptions?.onConfirm;
    assert.equal(typeof onConfirm, 'function');
    onConfirm(text);
    textActive = false;
}

const osModule = {
    readdir(directory) {
        return directory.endsWith('/presets/mono') ? [...presetFiles] : [];
    },
    rename(from, to) {
        const payload = savedFiles.get(from);
        assert.notEqual(payload, undefined, `missing temporary preset ${from}`);
        savedFiles.delete(from);
        savedFiles.set(to, payload);
        const file = path.basename(to);
        if (!presetFiles.includes(file)) presetFiles.push(file);
        return 0;
    },
    remove(file) {
        savedFiles.delete(file);
        const index = presetFiles.indexOf(path.basename(file));
        if (index >= 0) presetFiles.splice(index, 1);
        return 0;
    },
    mkdir() { return 0; }
};

const context = vm.createContext({
    console,
    clear_screen() {}, print() {}, fill_rect() {},
    move_midi_internal_send() {},
    host_module_get_param(key) {
        if (key === 'state') {
            stateCalls++;
            return stateResponses.length ? stateResponses.shift() : undefined;
        }
        return params.get(key) ?? '0';
    },
    host_module_set_param(key, value) { params.set(key, String(value)); },
    host_module_set_param_blocking(key, value) { params.set(key, String(value)); },
    host_ensure_dir() { return true; },
    host_write_file(file, payload) { savedFiles.set(file, payload); return true; },
    host_read_file(file) { return savedFiles.get(file); }
});

function synthetic(exports) {
    return new vm.SyntheticModule(Object.keys(exports), function initialize() {
        for (const [name, value] of Object.entries(exports)) this.setExport(name, value);
    }, {context});
}

const modules = new Map([
    ['os', synthetic(osModule)],
    ['/data/UserData/schwung/shared/constants.mjs', synthetic(constants)],
    ['/data/UserData/schwung/shared/input_filter.mjs', synthetic({
        decodeDelta: value => value <= 63 ? value : value - 128,
        setLED() {}
    })],
    ['/data/UserData/schwung/shared/menu_layout.mjs', synthetic({
        drawMenuHeader() {}, drawMenuFooter() {}
    })],
    ['/data/UserData/schwung/shared/screen_reader.mjs', synthetic({
        announce: message => announcements.push(String(message)),
        announceParameter: (name, value) => announcements.push(`${name}: ${value}`),
        announceView: message => announcements.push(String(message))
    })],
    ['/data/UserData/schwung/shared/text_entry.mjs', synthetic(textEntry)]
]);

const uiModule = new vm.SourceTextModule(source, {
    context,
    identifier: path.join(root, 'src/ui_overtake.js')
});
await uiModule.link(specifier => {
    const dependency = modules.get(specifier);
    if (!dependency) throw new Error(`unexpected import: ${specifier}`);
    return dependency;
});
await uiModule.evaluate();

const ui = context;
ui.init();

const cc = (control, value) => ui.onMidiMessageInternal([0xb0, control, value]);
const openSaveKeyboard = () => {
    cc(MoveShift, 127);
    cc(MoveMainButton, 127);
    cc(MoveShift, 0);
    assert.equal(ui.wantsBack(), true, 'preset browser must claim Back');
    cc(MoveMainButton, 127);
    assert.equal(textActive, true, 'Save current must open text entry');
};

openSaveKeyboard();
ui.tick();
assert.equal(textTickCalls, 1, 'active text entry must receive UI ticks');
assert.equal(textDrawCalls, 1, 'active text entry must own the display');
cc(MoveMainKnob, 1);
assert.equal(textMidiCalls, 1, 'active text entry must receive MIDI input');

cc(MoveBack, 127);
assert.equal(textActive, false, 'Back must cancel text entry');
assert.equal(ui.wantsBack(), true, 'cancel returns to preset browser');
cc(MoveBack, 127);
assert.equal(ui.wantsBack(), false, 'second Back closes preset browser');

stateResponses = [undefined, undefined, '{"v":11,"data":"test"}'];
openSaveKeyboard();
confirmText('Feedback Fix');
assert.equal(stateCalls, 3, 'preset save must retry transient empty state reads');
assert.equal(ui.wantsBack(), false, 'successful save must return to Mono');
assert.equal(presetFiles.length, 1);
const saved = JSON.parse(savedFiles.get(
    '/data/UserData/schwung/presets/mono/Feedback Fix.json'));
assert.equal(saved.name, 'Feedback Fix');
assert.equal(saved.module, 'mono');
assert.deepEqual(saved.state, {v: 11, data: 'test'});
assert(announcements.includes('Saved Feedback Fix'));

openSaveKeyboard();
ui.onUnload();
assert.equal(textActive, false, 'unload must close active text entry');
assert.equal(textCloseCalls, 1);

console.log('mono overtake UI: preset save and navigation tests passed');
