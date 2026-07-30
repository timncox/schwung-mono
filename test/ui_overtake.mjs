import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = fs.readFileSync(path.join(root, 'src/ui_overtake.js'), 'utf8');

const MoveKnob1 = 71;
const MoveShift = 1;
const MoveMainKnob = 2;
const MoveMainButton = 3;
const MoveBack = 4;
const MoveRec = 86;
const MoveMute = 88;
const MoveLeft = 5;
const MoveRight = 6;
const PAD_MACHINE = 98;
const TRACK_PAD_1 = 92;

const constants = {
    MoveKnob1, MoveShift, MoveMainKnob, MoveMainButton, MoveBack,
    MoveLeft, MoveRight, MoveUp: 7, MoveDown: 8, MoveRec, MoveMute,
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
/* Blocking round-trips to the shim. Counting them is the point: each costs a
 * whole SPI frame (~23 ms) and the channel serves only ~44 a second, so a UI
 * that is correct but chatty still freezes on the Move. */
let roundTrips = 0;
/* When positive, reads time out and return null — what actually happens once
 * the param channel is saturated. The mock used to answer '0' for everything,
 * so this failure mode could not be reproduced at all. */
let readFailures = 0;
const savedFiles = new Map();
const presetFiles = [];
const announcements = [];
const noteLedMessages = [];
const buttonLedMessages = [];

let textActive = false;
let textOptions = null;
let textMidiCalls = 0;
let textTickCalls = 0;
let textDrawCalls = 0;
let textCloseCalls = 0;
let suspendCalls = 0;

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

/* QuickJS `os.readdir` returns a [names, errno] tuple and the raw listing
 * includes "." and "..". Mocking it as a flat array of filenames hid a bug
 * that broke every save on hardware, so the stub mirrors the real contract.
 * `presetDirLeaders` controls whether "."/".." come first (so a real ".json"
 * sorts last) or last — readdir order is filesystem-defined, and the two
 * orderings used to fail in different ways. */
let presetDirLeaders = true;
let renameErrno = 0;
let writeFailure = false;

const osModule = {
    readdir(directory) {
        if (!directory.endsWith('/presets/mono')) return [[], 2];
        const dots = ['.', '..'];
        return [presetDirLeaders
            ? [...dots, ...presetFiles]
            : [...presetFiles, ...dots], 0];
    },
    /* QuickJS `os.rename` signals failure with a negative return code — it
     * never throws — so the stub reports refusal the same way. */
    rename(from, to) {
        if (renameErrno !== 0) return renameErrno;
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
        roundTrips++;
        if (key === 'state') {
            stateCalls++;
            return stateResponses.length ? stateResponses.shift() : undefined;
        }
        if (readFailures > 0) { readFailures--; return null; }
        return params.get(key) ?? '0';
    },
    /* BULK_GET, transcribed from shim_handle_param_bulk / bulk_next in
     * schwung's src/schwung_shim.c — NOT from the code under test. Request is
     * "<count>\n" then count records of "<len>\n<key>"; response is the same
     * shape carrying values in the same order. The shim rejects >64 items. */
    host_module_get_params(blob) {
        roundTrips++;
        if (readFailures > 0) { readFailures--; return null; }
        const text = String(blob);
        let at = 0;
        const readLen = () => {
            let n = 0, any = false;
            while (at < text.length && text[at] >= '0' && text[at] <= '9') {
                n = n * 10 + (text.charCodeAt(at) - 48); at++; any = true;
            }
            if (!any || text[at] !== '\n') return -1;
            at++;
            return n;
        };
        const count = readLen();
        if (count < 0 || count > 64) return null;
        const keys = [];
        for (let i = 0; i < count; i++) {
            const len = readLen();
            if (len < 0) return null;
            keys.push(text.slice(at, at + len));
            at += len;
        }
        let out = `${keys.length}\n`;
        for (const k of keys) {
            const value = params.get(k) ?? '0';
            out += `${value.length}\n${value}`;
        }
        return out;
    },
    host_module_set_param(key, value) { params.set(key, String(value)); },
    host_module_set_param_blocking(key, value) { params.set(key, String(value)); },
    host_ensure_dir() { return true; },
    host_write_file(file, payload) {
        if (writeFailure) return false;
        savedFiles.set(file, payload);
        if (/\.json$/.test(file)) {
            const name = path.basename(file);
            if (!presetFiles.includes(name)) presetFiles.push(name);
        }
        return true;
    },
    host_read_file(file) { return savedFiles.get(file); },
    host_suspend_overtake() { suspendCalls++; }
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
        setLED: (note, color, force) => noteLedMessages.push({note, color, force}),
        setButtonLED: (cc, color, force) => buttonLedMessages.push({cc, color, force})
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
const noteOn = (note, velocity = 100) => ui.onMidiMessageInternal([0x90, note, velocity]);
const noteOff = note => ui.onMidiMessageInternal([0x80, note, 0]);

noteLedMessages.length = 0;
buttonLedMessages.length = 0;
cc(MoveRec, 127);
assert.deepEqual(buttonLedMessages.at(-1), {cc: MoveRec, color: constants.BrightRed, force: false},
    'record arm must light the Record button through its CC LED');
assert.equal(noteLedMessages.some(message => message.note === MoveRec), false,
    'record arm must never light pad note 86');

cc(MoveShift, 127);
noteOn(99);
cc(MoveShift, 0);
/* Three detents to reach ROUTING + FX. This used to be one event carrying an
 * accumulated delta of 3, which jumped three pages at once — decodeDelta
 * reports accumulated movement, so that made a brisk flick skip most of the
 * setup pages. The jog now advances one page per event. */
cc(MoveMainKnob, 1);
cc(MoveMainKnob, 1);
cc(MoveMainKnob, 1);
cc(MoveKnob1, 4);
assert.equal(params.get('route_mode') ?? '0', '0',
    'Track 1 must reject neighbor routing because it has no source');
assert(announcements.at(-1).includes('Track 1 has no previous routing source'));
noteOn(93);
/* route_mode has five values, so a knob detent moves it one. This used to be a
 * single event carrying an accumulated delta of 4, which only landed on FM
 * because the number happened to match — any faster turn would have slammed to
 * the same end, and modes 1-3 were unreachable by a normal turn. */
cc(MoveKnob1, 1);
cc(MoveKnob1, 1);
cc(MoveKnob1, 1);
cc(MoveKnob1, 1);
assert.equal(params.get('route_mode'), '4',
    'Track 2 must accept FM from Track 1 on the receiving track');
assert(announcements.at(-1).includes('Input from Track 1'));
cc(MoveBack, 127);

noteOn(16);
cc(MoveRec, 127);
noteOff(16);
assert.equal(params.get('clear_step'), '1:0',
    'held step + Record must clear a step when Delete is owned by Move');

noteOn(17);
noteOn(PAD_MACHINE);
noteOff(17);
assert.equal(params.get('copy_step'), '1:1',
    'held step + Machine must copy a step when Copy is owned by Move');

noteOn(18);
cc(MoveShift, 127);
noteOn(PAD_MACHINE);
cc(MoveShift, 0);
noteOff(18);
assert.equal(params.get('paste_step'), '1:2',
    'held step + Shift + Machine must paste a step when Copy is owned by Move');

cc(MoveShift, 127);
cc(MoveLeft, 127);
cc(MoveRight, 127);
cc(MoveShift, 0);
assert.equal(params.get('copy_track'), '1', 'Shift + Left must copy the selected track');
assert.equal(params.get('paste_track'), '1', 'Shift + Right must paste the selected track');

cc(MoveShift, 127);
cc(MoveRec, 127);
cc(MoveShift, 0);
assert.equal(params.get('undo'), '1', 'Shift + Record must provide an Undo fallback');

cc(MoveMute, 127);
noteOn(TRACK_PAD_1);
cc(MoveMute, 0);
assert.equal(params.get('track_mute_toggle'), '0', 'Mute + track pad must toggle track mute');
cc(MoveShift, 127);
cc(MoveMute, 127);
noteOn(TRACK_PAD_1);
cc(MoveMute, 0);
cc(MoveShift, 0);
assert.equal(params.get('track_solo_toggle'), '0', 'Shift + Mute + track pad must toggle track solo');

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

/* Regression: a saved preset has to come back out of the directory listing.
 * The reported bug was that saving appeared to do nothing — the file landed on
 * disk but the browser stayed empty, because os.readdir's [names, errno] tuple
 * was read as a flat filename array. Reopen the browser and require the preset
 * to be listed and selectable. */
const openPresetBrowserUi = () => {
    cc(MoveShift, 127);
    cc(MoveMainButton, 127);
    cc(MoveShift, 0);
};

openPresetBrowserUi();
assert.equal(ui.wantsBack(), true, 'Shift + jog click must open the preset browser');
assert(announcements.includes('Mono presets, 1 saved'),
    'a saved preset must appear in the browser listing');
cc(MoveMainKnob, 1);
assert(announcements.at(-1).includes('Feedback Fix'),
    'the saved preset must be selectable in the list');
cc(MoveBack, 127);

/* readdir order is filesystem-defined. With "."/".." last the old code silently
 * listed nothing; with a real ".json" last it threw and killed the handler.
 * Both orderings must list exactly the saved presets, and never "." or "..". */
presetDirLeaders = false;
openPresetBrowserUi();
assert.equal(ui.wantsBack(), true,
    'preset browser must open regardless of readdir entry order');
assert(announcements.includes('Mono presets, 1 saved'),
    'trailing "."/".." entries must not hide the preset list');
cc(MoveBack, 127);
presetDirLeaders = true;

/* A refused rename must not be reported as a successful save. os.rename
 * returns -errno instead of throwing, so the old try/catch always claimed
 * success and left the patch stranded in the .tmp file. */
renameErrno = -13;
stateResponses = ['{"v":11,"data":"rename"}'];
openSaveKeyboard();
confirmText('Rename Fallback');
renameErrno = 0;
assert(presetFiles.includes('Rename Fallback.json'),
    'a refused rename must still persist the preset via a direct write');
assert(announcements.includes('Saved Rename Fallback'),
    'the fallback write is a real save and must be announced as one');
assert.equal(savedFiles.has(
    '/data/UserData/schwung/presets/mono/Rename Fallback.json.tmp'), false,
    'the temporary file must be cleaned up after the fallback write');

openPresetBrowserUi();
assert(announcements.includes('Mono presets, 2 saved'),
    'the fallback-written preset must appear in the browser');
cc(MoveBack, 127);

/* When the write itself fails there is nothing on disk: say so rather than
 * announcing a save that did not happen. */
writeFailure = true;
stateResponses = ['{"v":11,"data":"nowrite"}'];
openSaveKeyboard();
confirmText('Doomed Save');
writeFailure = false;
assert.equal(presetFiles.includes('Doomed Save.json'), false,
    'a failed write must not create a preset');
assert.equal(announcements.at(-1), 'Preset save failed',
    'a failed write must surface an error instead of a phantom save');
assert.equal(textActive, false, 'a failed save must still close the keyboard');
assert.equal(ui.wantsBack(), true,
    'a failed save must leave the browser open rather than pretending to finish');
cc(MoveBack, 127);
assert.equal(ui.wantsBack(), false, 'Back returns to Mono after a failed save');

for (const file of [...presetFiles]) {
    if (file !== 'Feedback Fix.json') {
        osModule.remove(`/data/UserData/schwung/presets/mono/${file}`);
    }
}

cc(MoveShift, 127);
cc(MoveMainButton, 127);
cc(MoveShift, 0);
cc(MoveMainKnob, 1);
cc(MoveShift, 127);
cc(MoveLeft, 127);
cc(MoveLeft, 127);
cc(MoveShift, 0);
assert.equal(presetFiles.length, 0, 'Shift + Left must confirm-delete a preset without Move Delete');
cc(MoveBack, 127);

cc(MoveBack, 127);
assert.equal(suspendCalls, 1, 'Back at the main screen must park a self-managed overtake');

const monoManifest = JSON.parse(fs.readFileSync(
    path.join(root, 'modules/overtake/mono/module.json'), 'utf8'));
assert.equal(monoManifest.capabilities.suspend_self_managed, true,
    'Mono must opt into Schwung 0.11.6 self-managed Back handling');

openSaveKeyboard();
ui.onUnload();
assert.equal(textActive, false, 'unload must close active text entry');
assert.equal(textCloseCalls, 1);
assert.deepEqual(buttonLedMessages.at(-1), {cc: MoveRec, color: constants.Black, force: true},
    'unload must clear the Record button');

/* ------------------------------------------------- param-channel behaviour
 *
 * These exist because Work shipped three hardware bugs that were all one
 * cause: host_module_get_param is a blocking round-trip to the shim, serviced
 * once per SPI frame (~23 ms) and abandoned after 100 ms. Reading dozens of
 * keys freezes the UI, saturating the channel makes OTHER reads time out, and
 * a timed-out read folded into a default silently rewrites the patch.
 */

/* The channel serves roughly 44 reads a second in total. A UI that asks for
 * more than that starves itself and everything else. */
ui.init();
roundTrips = 0;
for (let i = 0; i < 44; i++) ui.tick();          /* one second of ticks */
assert(roundTrips <= 30,
    `steady state costs ${roundTrips} blocking round-trips per second against a ` +
    'channel that serves about 44 — this is what makes reads time out');

/* ...and a full editor refresh must be batched, not sixty separate frames. */
let bulkSeen = false;
const realBulk = context.host_module_get_params;
context.host_module_get_params = function(blob) { bulkSeen = true; return realBulk(blob); };
roundTrips = 0;
for (let i = 0; i < 31; i++) ui.tick();          /* crosses the %30 refresh */
assert(bulkSeen, 'the full refresh never used the bulk get_params path');

/* Decoding the bulk response wrong shifts every value onto a neighbouring key,
 * which looks like working software right up until a knob edits the wrong
 * parameter. track_div can only have reached the mirror through a bulk
 * response, so continuing from it proves the alignment. */
/* Close whatever view earlier tests left open — Sequence Setup and the preset
 * browser both intercept the knobs before adjust() sees them. */
cc(MoveBack, 127);
cc(MoveBack, 127);
params.set('p1', '40');
params.set('p2', '90');
ui.init();
cc(MoveKnob1, 1);
assert.equal(params.get('p1'), '41',
    `p1 continued from the wrong base: got ${params.get('p1')}, expected 41`);
cc(MoveKnob1 + 1, 1);
assert.equal(params.get('p2'), '91',
    `p2 continued from the wrong base: got ${params.get('p2')}, expected 91`);

/* A dead param channel must not turn the mirror into zeros, and above all must
 * not write those zeros back to the DSP. */
params.set('track_level', '96');
ui.init();
readFailures = 500;
for (let i = 0; i < 60; i++) ui.tick();
readFailures = 0;
assert.equal(params.get('track_level'), '96',
    'a timed-out read must leave the DSP alone, not write a default back');


/* [file, table, engine count, consequence of drift] */
const FX_N = cEnumCount("src/mono_core.h", "MONO_MACHINE_COUNT");
const DESTS = cDefineCount("src/mono_core.h",
    (n) => n("MONO_PAGES") * n("MONO_PAGE_PARAMS") * 2 + 2);
const ENGINE_INDEXED_TABLES = [
    ["src/ui_overtake.js", "MACHINES",        FX_N,  "a machine would show no name"],
    ["src/ui_overtake.js", "MACHINE_SHORT",   FX_N,  "a machine would show no short name"],
    ["src/ui_overtake.js", "MACHINE_COLORS",  FX_N,  "a machine pad would light with no colour"],
    ["src/ui_overtake.js", "LFO_DESTS",       DESTS, "THE DANGEROUS ONE: this list is not labels, it is the destination VALUE written to the engine. Drift both mislabels every entry past the insertion point and routes modulation to the wrong parameter"],
    ["src/ui_overtake.js", "LFO_DEST_SCREEN", DESTS, "the screen labels would name the wrong parameter"],
    ["src/ui_chain.js",    "MACHINES",        FX_N,  "a machine would show no name"],
    ["src/ui_chain.js",    "LFO_DESTS",       DESTS, "modulation would route to the wrong parameter"],
    ["src/ui_chain.js",    "LFO_DEST_SCREEN", DESTS, "the screen labels would name the wrong parameter"]
];

/* ------------------------------------------------- engine-indexed tables
 *
 * These arrays are hand-maintained in JavaScript but INDEXED BY A CODE THE
 * ENGINE OWNS. Nothing links the two — no import, no compiler — so growing the
 * C enum silently leaves the UI stale, and the symptom is invisible: the last
 * entry becomes unreachable, or a label names the wrong thing. That exact shape
 * made two machines unreachable in the sibling module Work before anyone
 * noticed, because it never throws.
 *
 * They cannot be derived (a colour or an abbreviation is a design choice, not
 * engine data), so the drift is made LOUD here instead. The counts come from
 * the C header itself, which is the only source of truth in the repo.
 */
function cEnumCount(header, terminator) {
    const src = fs.readFileSync(path.join(root, header), 'utf8');
    const at = src.indexOf(terminator);
    assert(at > 0, `${terminator} not found in ${header}`);
    const open = src.lastIndexOf('{', at);
    const body = src.slice(open + 1, at)
        .replace(/\/\*[\s\S]*?\*\//g, '')
        .replace(/\/\/[^\n]*/g, '');
    return (body.match(/\b[A-Z_][A-Z0-9_]*\s*(?:=[^,]*)?,/g) || []).length;
}

function cDefineCount(header, expr) {
    const src = fs.readFileSync(path.join(root, header), 'utf8');
    const num = (name) => {
        const m = src.match(new RegExp('#define\\s+' + name + '\\s+(\\d+)'));
        return m ? parseInt(m[1], 10) : null;
    };
    return expr(num);
}

/* Count top-level items in `const NAME = [...]`, balancing brackets so nested
 * arrays and objects count as one. */
function jsArrayLen(file, name) {
    const src = fs.readFileSync(path.join(root, file), 'utf8');
    const m = src.match(new RegExp('const\\s+' + name + '\\s*=\\s*\\['));
    assert(m, `${name} not found in ${file}`);
    let i = m.index + m[0].length - 1, depth = 0, end = -1;
    for (let k = i; k < src.length; k++) {
        const c = src[k];
        if (c === '[') depth++;
        else if (c === ']') { depth--; if (depth === 0) { end = k; break; } }
    }
    const body = src.slice(i + 1, end)
        .replace(/\/\*[\s\S]*?\*\//g, '')
        .replace(/\/\/[^\n]*/g, '');
    let d = 0, count = 0, cur = '';
    for (const c of body) {
        if ('[{('.includes(c)) d++;
        else if (']})'.includes(c)) d--;
        if (c === ',' && d === 0) { if (cur.trim()) count++; cur = ''; }
        else cur += c;
    }
    if (cur.trim()) count++;
    return count;
}

for (const [file, table, expected, why] of ENGINE_INDEXED_TABLES) {
    const got = jsArrayLen(file, table);
    assert.equal(got, expected,
        `${file}: ${table} has ${got} entries but the engine has ${expected} — ${why}`);
}

console.log('mono overtake UI: presets, routing, LED, and param-channel tests passed');
